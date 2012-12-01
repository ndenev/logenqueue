/*-
 * Copyright (c) 2012 Nikolay Denev <ndenev@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 * derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <netdb.h>
#include <math.h>
#include <pthread.h>
#include <pthread_np.h>

#include <zlib.h>
#include <amqp.h>
#include <amqp_framing.h>

#include "logenqueue.h"
#include "config.h"

struct  config  cfg;

volatile static int msg_rcvd = 0;
volatile static int msg_pub = 0;
volatile static u_int64_t msg_cnt_ts;

#define STATS_TIMEOUT	5
#define	ZLIBD	0
#define	GZIPD	1
#define	CHUNKD	2

enum worker_type {
	SYSLOG,
	GELF,
};

struct worker_data {
	int	id;
	enum worker_type type;
	pthread_cond_t cnd;
	pthread_mutex_t mtx;
	u_char	buf[9216];
	int	buf_len;
	struct  sockaddr from;
};

static const char gelf_magic[3][2] = {
	{ 0x78, 0x9c }, 
	{ 0x1f, 0x8b },
	{ 0x1e, 0x0f },
};

struct amqp_state_t {
	amqp_connection_state_t conn;
	amqp_basic_properties_t props;
};

static const char *
fac2str(int facility)
{
	static const char *f2s[] = {
		"kernel",
		"user-level",
		"mail",
		"system daemon",
		"security/authorization",
		"syslogd",
		"lpr",
		"network news",
		"uucp",
		"clock",
		"security/authorization",
		"ftp",
		"ntp",
		"log audit",
		"log alert",
		"clock",
		"local0",
		"local1",
		"local2",
		"local3",
		"local4",
		"local5",
		"local6",
		"local7",
		"GELF"
	};
	if (facility > 0 && facility < 23)
		return (f2s[facility]);
	else
		return (f2s[24]);
}

void
message_stats(void *arg __unused)
{
	for (;;) {
		printf("incoming msg rate  : %d msg/sec\n", msg_rcvd / STATS_TIMEOUT);
		printf("publish msg rate  : %d msg/sec\n", msg_pub / STATS_TIMEOUT);
		msg_rcvd = 0;
		msg_pub = 0;
		sleep(STATS_TIMEOUT);
	};
}

void
sighandler_int(int sig)
{
	exit(0);
}

void
syslog_worker(void *arg)
{
	struct worker_data *self = (struct worker_data *)arg;

	struct  amqp_state_t amqp;
	struct hostent *hp;
	char 	host[256];
	char *msg, *msg2;
	u_char esc_buf[8129*2];
	int r, i1, i2;
	int pri;
	struct tm tim;
	time_t ts;
	amqp_bytes_t msgb;
	int flush;
	z_stream strm;
	u_char in[9216*2];
	u_char out[9216*2];

	printf("syslog worker thread #%d started\n", self->id);

	amqp.props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	amqp.props.delivery_mode = 2; /* persistent delivery mode */
	amqp.props.content_type = amqp_cstring_bytes("application/octet-stream");

	amqp.conn = amqp_new_connection();
	cfg.amqp.fd = amqp_open_socket(cfg.amqp.host, cfg.amqp.port);
	amqp_set_sockfd(amqp.conn, cfg.amqp.fd);
	amqp_login(amqp.conn, cfg.amqp.vhost, 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, cfg.amqp.user, cfg.amqp.pass);
	amqp_channel_open(amqp.conn, 1);
	amqp_get_rpc_reply(amqp.conn);


	for (;;) {
		//pthread_mutex_lock(&self->mtx);
		//pthread_cond_wait(&self->cnd, &self->mtx);

		unsigned int ip_len;
		r = recvfrom(cfg.syslog.fd, self->buf, sizeof(self->buf), 0, &self->from, &ip_len);
		if (r < 0) {
			printf("recvfrom problem\n");
			continue;
		}
		if (r == 0) {
			printf("zero data read\n");
		}
		self->buf[r] = '\0';
		self->buf_len = r;
		msg_rcvd++;

		hp = NULL;
		if ((hp = gethostbyaddr((const void *)&self->from.sa_data+2, sizeof(struct in_addr), AF_INET)))
			strncpy(host, hp->h_name, sizeof(host));
		else
			inet_ntop(self->from.sa_family, self->from.sa_data+2, host, sizeof(host));

		/* escape back slashes and quotes in json */
		i2 = 0;
		for (i1 = 0; i1 < strlen((char *)self->buf); i1++) {
			/* escape control chars */
			if (self->buf[i1] < 0x1f) {
				r = snprintf((char *)esc_buf+i2,
						sizeof(esc_buf) - i2,
						"\\u%04x",
						self->buf[i1]);
				i2 += r;
			} else {
				if (self->buf[i1] == '\\' || self->buf[i1] == '"')
					esc_buf[i2++] = '\\';
				esc_buf[i2++] = self->buf[i1];
			}
		}
		esc_buf[i2] = '\0';

		if (esc_buf[0] != '<') {
			printf("invalid syslog format from (%s). msg: \"%s\"\n", host, esc_buf);
			return;
		}
		msg = strchr((char *)esc_buf, '>');
		msg++;
		pri = (int)strtol((char *)esc_buf+1,(char **)NULL,10);

		int severity = pri & 0x07;
		int facility = pri >> 3;

		/* try to parse time from the message
		 * or fall back to using current time
		 */
		msg2 = strptime(msg, "%b %d %H:%M:%S", &tim);
		if (msg2) {
			ts = mktime(&tim);
			msg = msg2;
		} else {
			ts = time(NULL);
		}

		/* ugly home grown json */
		snprintf((char *)in, sizeof(in),
			"{ \"version\": \"%s\", \"host\": \"%s\","
			"\"short_message\": \"%s\", \"full_message\": \"%s\","
			"\"timestamp\": \"%ld\", \"level\": %d,"
			"\"facility\": \"%s\", \"file\": \"%s\","
			"\"line\": %d }",
			"1.0", host, msg, esc_buf, (long int)ts,
			severity, fac2str(facility), "", 0);

		/* allocate deflate state */
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		deflateInit(&strm, Z_DEFAULT_COMPRESSION);

		strm.avail_in = strlen((char *)in);
		strm.next_in = in;
		strm.avail_out = 9216*2;
		strm.next_out = out;
		flush = Z_FINISH;
		deflate(&strm, flush);

		(void)deflateEnd(&strm);

		msgb.len = (9216*2) - strm.avail_out;
		msgb.bytes = out;

		amqp_basic_publish(amqp.conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
				    &amqp.props, msgb);

		msg_pub++;
		//pthread_mutex_unlock(&self->mtx);
	}

}

void
syslog_listener(void *arg)
{
	struct worker_data *thr = (struct worker_data *)arg;
	unsigned int ip_len;
	int r;
	int t_cur, t_max;

	t_cur = 0;
	t_max = cfg.syslog.workers - 1;
	ip_len = sizeof(struct sockaddr);

	DEBUG("SYSLOG listener thread started\n");

	for (;;) {
		if(!pthread_mutex_trylock(&(thr+t_cur)->mtx)) {
			r = recvfrom(cfg.syslog.fd, (thr+t_cur)->buf, sizeof((thr+t_cur)->buf), 0, &(thr+t_cur)->from, &ip_len);
			if (r < 0)
				continue;
			(thr+t_cur)->buf[r] = '\0';
			(thr+t_cur)->buf_len = r;
			msg_rcvd++;
			pthread_mutex_unlock(&(thr+t_cur)->mtx);
			pthread_cond_signal(&(thr+t_cur)->cnd);
		}
		if (t_cur < t_max)
			t_cur++;
		else
			t_cur = 0;
	}
}

void
gelf_worker(void *arg)
{
	struct worker_data *self = (struct worker_data *)arg;

	struct  amqp_state_t amqp;
	amqp_bytes_t msgb;

	printf("gelf worker thread #%d started\n", self->id);
	amqp.props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	amqp.props.delivery_mode = 2; /* persistent delivery mode */
	amqp.props.content_type = amqp_cstring_bytes("application/octet-stream");
	amqp.conn = amqp_new_connection();
	cfg.amqp.fd = amqp_open_socket(cfg.amqp.host, cfg.amqp.port);
	amqp_set_sockfd(amqp.conn, cfg.amqp.fd);
	amqp_login(amqp.conn, cfg.amqp.vhost, 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, cfg.amqp.user, cfg.amqp.pass);
	amqp_channel_open(amqp.conn, 1);
	amqp_get_rpc_reply(amqp.conn);

	for (;;) {
		pthread_mutex_lock(&self->mtx);
                pthread_cond_wait(&self->cnd, &self->mtx);

#define	GELF_MAGIC(type)	bcmp(self->buf, gelf_magic[type], sizeof(gelf_magic[type]))

		if (!GELF_MAGIC(ZLIBD)) {
			//DEBUG("Received ZLIB'd GELF message.\n");
		} else if (!GELF_MAGIC(GZIPD)) {
			//DEBUG("Received GZIP'd GELF message.\n");
		} else if (!GELF_MAGIC(CHUNKD)) {
			//DEBUG("Received CHUNKED GELF message.\n");
		} else {
			LOG("Unknown GELF type. Maybe RAW? Bailing out.");
			continue;
		}

		msgb.len = self->buf_len;
		msgb.bytes = self->buf;

		amqp_basic_publish(amqp.conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
				    &amqp.props, msgb);

		msg_pub++;


	}
}

void
gelf_listener(void *arg)
{
        struct worker_data *thr = (struct worker_data *)arg;
	int r;
        int t_cur, t_max;

	DEBUG("GELF listener thread started\n");

        t_cur = 0;
        t_max = cfg.gelf.workers - 1;

	for (;;) {
                if(!pthread_mutex_trylock(&(thr+t_cur)->mtx)) {
			r = recvfrom(cfg.gelf.fd, &(thr+t_cur)->buf, sizeof((thr+t_cur)->buf), 0, NULL, NULL);
			if (r < 0)
				continue;
			msg_rcvd++;
		}
                if (t_cur < t_max)
                        t_cur++;
                else
                        t_cur = 0;
		pthread_mutex_unlock(&(thr+t_cur)->mtx);
		pthread_cond_signal(&(thr+t_cur)->cnd);
	}
}

int
udp_listen(char *bindaddr, u_int port)
{
	int udpsock_fd, noptval, ret;
	struct sockaddr_in staddr;

	udpsock_fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (udpsock_fd == -1) {
		printf("error creating socket!: %s\n", strerror(errno));
		exit(-1);
	}

	memset(&staddr, 0, sizeof(struct sockaddr_in));
	staddr.sin_addr.s_addr = inet_addr(bindaddr);
	staddr.sin_port = htons(port);
	staddr.sin_family = AF_INET;

	noptval = 1024 * 1024;
	ret = setsockopt(udpsock_fd, SOL_SOCKET, SO_RCVBUF,
		(const void *)&noptval, sizeof(noptval));

	noptval = 1;
	ret = setsockopt(udpsock_fd, SOL_SOCKET, SO_REUSEADDR,
		(const void *)&noptval, sizeof(noptval));
	if (ret == -1) {
		printf("error calling setsockopt: %s\n", strerror(errno));
		exit(-1);
	}

	ret = bind(udpsock_fd, (struct sockaddr *)&staddr, sizeof(staddr));
	if (ret != 0) {
		printf("bind: %s\n", bindaddr);
		printf("error binding to socket: %s\n", strerror(errno));
		exit(-1);
	}

	return(udpsock_fd);
}

int
main(int argc, char **argv)
{
	int	pid;
	int	i;
	struct  amqp_state_t amqp;
	pthread_t syslog_listen_thr, gelf_listen_thr, stats_thread, *syslog_workers, *gelf_workers;
	struct worker_data *syslog_workers_data, *gelf_workers_data;
	char tname[32];

	signal(SIGINT, sighandler_int);

	parse_opts(&argc, &argv);

	parse_config();
	
	if (!debug) {
		pid = fork();
		if (pid < 0) {
			printf("unable to fork: %s\n", strerror(errno));
			exit(-1);
		}
		if (pid > 0) {
			printf("logenqueue started and going into background\n");
			_Exit(0);
		}
		setsid();
	}
	
	VERBOSE("syslog listen : %s:%d\n", cfg.syslog.bind, cfg.syslog.port);
	VERBOSE("gelf listen : %s:%d\n", cfg.gelf.bind, cfg.gelf.port);

	cfg.syslog.fd = udp_listen(cfg.syslog.bind, cfg.syslog.port);
	cfg.gelf.fd = udp_listen(cfg.gelf.bind, cfg.gelf.port);

	amqp.conn = amqp_new_connection();
	cfg.amqp.fd = amqp_open_socket(cfg.amqp.host, cfg.amqp.port);
	amqp_set_sockfd(amqp.conn, cfg.amqp.fd);
	amqp_login(amqp.conn, cfg.amqp.vhost, 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, cfg.amqp.user, cfg.amqp.pass);
	amqp_channel_open(amqp.conn, 1);
	amqp_get_rpc_reply(amqp.conn);

	amqp_exchange_declare(amqp.conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				       amqp_cstring_bytes(cfg.amqp.exch_type),
				       0,
				       0,
				       amqp_empty_table);

	syslog_workers = calloc(cfg.syslog.workers, sizeof(pthread_t));
	gelf_workers = calloc(cfg.gelf.workers, sizeof(pthread_t));

	syslog_workers_data = calloc(cfg.syslog.workers, sizeof(struct worker_data));
	gelf_workers_data = calloc(cfg.gelf.workers, sizeof(struct worker_data));

	for (i = 0; i < cfg.syslog.workers; i++) {
		pthread_mutex_init(&(syslog_workers_data+i)->mtx, NULL);
		pthread_cond_init(&(syslog_workers_data+i)->cnd, NULL);
		(syslog_workers_data+i)->id = i;
		(syslog_workers_data+i)->type = SYSLOG;
		pthread_create(syslog_workers+i, NULL, (void *)&syslog_worker, syslog_workers_data+i);
		snprintf(tname, sizeof(tname), "syslog_worker[%d]", i);
		pthread_set_name_np(*(syslog_workers+i), tname);
	}

	for (i = 0; i < cfg.gelf.workers; i++) {
		pthread_mutex_init(&(gelf_workers_data+i)->mtx, NULL);
		pthread_cond_init(&(gelf_workers_data+i)->cnd, NULL);
		(gelf_workers_data+i)->id = i;
		(gelf_workers_data+i)->type = GELF;
		pthread_create(gelf_workers+i, NULL, (void *)&gelf_worker, gelf_workers_data+i);
		snprintf(tname, sizeof(tname), "gelf_worker[%d]", i);
		pthread_set_name_np(*(gelf_workers+i), tname);
	}

	//pthread_create(&syslog_listen_thr, NULL, (void *)&syslog_listener, syslog_workers_data);
	//snprintf(tname, sizeof(tname), "syslog_listener");
	//pthread_set_name_np(syslog_listen_thr, tname);

	pthread_create(&gelf_listen_thr, NULL, (void *)&gelf_listener, gelf_workers_data);
	snprintf(tname, sizeof(tname), "gelf_listener");
	pthread_set_name_np(gelf_listen_thr, tname);

	pthread_create(&stats_thread, NULL, (void *)&message_stats, NULL);
	snprintf(tname, sizeof(tname), "stats_thread");
	pthread_set_name_np(stats_thread, tname);

	pthread_join(stats_thread, NULL);

	return 0;
}
