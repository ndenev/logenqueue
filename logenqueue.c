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
#if __FreeBSD__
#include <pthread_np.h>
#endif

#include <zlib.h>
#include <amqp.h>
#include <amqp_framing.h>

#include "logenqueue.h"
#include "config.h"

struct  config  cfg;

volatile static int msg_rcvd = 0;

#define STATS_TIMEOUT 10
#define	ZLIBD	0
#define	GZIPD	1
#define	CHUNKD	2

#define SYSLOG_BUF 1024
#define GELF_BUF 65536

static const char *
fac2str(int facility)
{
	if (facility > 0 && facility < 23)
		return (f2s[facility]);
	else
		return (f2s[24]);
}

int
amqp_link(struct amqp_state_t *amqp)
{
	amqp_rpc_reply_t r;

	amqp->props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	amqp->props.delivery_mode = 2; /* persistent delivery mode */
	amqp->props.content_type = amqp_cstring_bytes("application/octet-stream");

	amqp->conn = amqp_new_connection();
	cfg.amqp.fd = amqp_open_socket(cfg.amqp.host, cfg.amqp.port);
	if (cfg.amqp.fd < 0) {
		printf("unable to open amqp socket!\n");
		return(-1);
	}
	amqp_set_sockfd(amqp->conn, cfg.amqp.fd);
	r = amqp_login(amqp->conn, cfg.amqp.vhost, 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, cfg.amqp.user, cfg.amqp.pass);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		printf("problem logging in amqp broker\n");
		return(-1);
	}
	amqp_channel_open(amqp->conn, 1);
	r = amqp_get_rpc_reply(amqp->conn);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		printf("problem opening amqp channel\n");
		return(-1);
	}
	return(0);
}

void
message_stats(void *arg __unused)
{
	for (;;) {
		VERBOSE("incoming msg rate  : %d msg/sec\n", msg_rcvd / STATS_TIMEOUT);
		setproctitle("%d msg/sec", msg_rcvd / STATS_TIMEOUT);
		msg_rcvd = 0;
		sleep(STATS_TIMEOUT);
	};
}

void
reload(int sig)
{
	printf("Here goes code to reload the config\n");
	return;
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

	struct	amqp_state_t amqp;
	struct	hostent *hp;
	struct  sockaddr from;
	u_int ip_len;
	char 	host[_POSIX_HOST_NAME_MAX];
	char	*msg, *msg2;
	u_char	buf[SYSLOG_BUF];
	u_char	esc_buf[SYSLOG_BUF*2];
	int	r, i1, i2;
	int	q;
	int	pri;
	struct	tm tim;
	time_t	ts;
	amqp_bytes_t msgb;
	int	flush;
	z_stream strm;
	u_char	in[SYSLOG_BUF*3];
	u_char	out[SYSLOG_BUF*3];

	//DEBUG("syslog worker thread #%d started\n", self->id);

	if (amqp_link(&amqp) < 0) {
		printf("problem with amqp connection in syslog worker #%d\n", self->id);
		return;
	}

	for (;;) {
		r = recvfrom(cfg.syslog.fd, buf, sizeof(buf), 0, &from, &ip_len);
		if (r < 0) {
			printf("recvfrom problem\n");
			continue;
		}
		if (r == 0) {
			printf("zero data read\n");
			continue;
		}
		buf[r] = '\0';
		msg_rcvd++;

		hp = NULL;
		if ((hp = gethostbyaddr((const void *)&from.sa_data+2, sizeof(struct in_addr), AF_INET)))
			strncpy(host, hp->h_name, sizeof(host));
		else
			inet_ntop(from.sa_family, from.sa_data+2, host, sizeof(host));

		/* escape back slashes and quotes in json */
		i2 = 0;
		for (i1 = 0; i1 < strlen((char *)buf); i1++) {
			/* escape control chars */
			if (buf[i1] < 0x1f) {
				r = snprintf((char *)esc_buf+i2,
						sizeof(esc_buf) - i2,
						"\\u%04x",
						buf[i1]);
				i2 += r;
			} else {
				if (buf[i1] == '\\' || buf[i1] == '"')
					esc_buf[i2++] = '\\';
				esc_buf[i2++] = buf[i1];
			}
		}
		esc_buf[i2] = '\0';

		if ( (esc_buf[0] != '<') || (!(msg = strchr((char *)esc_buf, '>'))) ) {
			VERBOSE("invalid syslog format from (%s). msg: \"%s\"\n", host, esc_buf);
			continue;	
		}
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
		strm.avail_out = SYSLOG_BUF*3;
		strm.next_out = out;
		flush = Z_FINISH;
		deflate(&strm, flush);

		(void)deflateEnd(&strm);

		msgb.len = (SYSLOG_BUF*3) - strm.avail_out;
		msgb.bytes = out;

		q = amqp_basic_publish(amqp.conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
				    &amqp.props, msgb);
		if (q < 0)
			printf("failure publishing message to amqp\n");

	}

}

void
gelf_worker(void *arg)
{
	struct worker_data *self = (struct worker_data *)arg;

	struct  amqp_state_t amqp;
	amqp_bytes_t msgb;
	u_char	buf[GELF_BUF];
	int	r;
	int	q;

	//DEBUG("gelf worker thread #%d started\n", self->id);

	if (amqp_link(&amqp) < 0) {
		printf("problem with amqp connection in gelf worker #%d\n", self->id);
		return;
	}

	for (;;) {
		r = recvfrom(cfg.gelf.fd, &buf, sizeof(buf), 0, NULL, NULL);
		if (r < 0) {
			printf("recvfrom problem\n");
			continue;
		}
		if (r == 0) {
			printf("zero data read\n");
		}
		msg_rcvd++;
#define	GELF_MAGIC(type)	bcmp(buf, gelf_magic[type], sizeof(gelf_magic[type]))

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

		msgb.len = r;
		msgb.bytes = buf;

		q = amqp_basic_publish(amqp.conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
				    &amqp.props, msgb);
		if (q < 0)
			printf("failure publishing message to amqp\n");

	}
}

int
udp_listen(char *bindaddr, u_int port)
{
	int udpsock_fd, noptval, r;
	struct sockaddr_in staddr;

	udpsock_fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (udpsock_fd == -1) {
		printf("error creating socket!: %s\n", strerror(errno));
		return(-1);
	}

	memset(&staddr, 0, sizeof(struct sockaddr_in));
	staddr.sin_addr.s_addr = inet_addr(bindaddr);
	staddr.sin_port = htons(port);
	staddr.sin_family = AF_INET;

	noptval = 1024 * 1024;
	r = setsockopt(udpsock_fd, SOL_SOCKET, SO_RCVBUF,
		(const void *)&noptval, sizeof(noptval));
	if (r == -1) {
		printf("error calling setsockopt: %s\n", strerror(errno));
		return(-1);
	}

	noptval = 1;
	r = setsockopt(udpsock_fd, SOL_SOCKET, SO_REUSEADDR,
		(const void *)&noptval, sizeof(noptval));
	if (r == -1) {
		printf("error calling setsockopt: %s\n", strerror(errno));
		return(-1);
	}

	r = bind(udpsock_fd, (struct sockaddr *)&staddr, sizeof(staddr));
	if (r != 0) {
		printf("bind: %s\n", bindaddr);
		printf("error binding to socket: %s\n", strerror(errno));
		return(-1);
	}

	return(udpsock_fd);
}

int
main(int argc, char **argv)
{
	int	pid;
	int	i;
	struct	amqp_state_t amqp;
	pthread_t stats_thread, *syslog_workers, *gelf_workers;
	struct	worker_data *syslog_workers_data, *gelf_workers_data;
	char	tname[17];
	amqp_rpc_reply_t r;

	signal(SIGHUP, reload);
	signal(SIGINT, sighandler_int);
	signal(SIGTERM, sighandler_int);

	if (parse_opts(&argc, &argv) < 0) {
		printf("problem parsing command line arguments/options\n");
		exit(-1);
	}

	parse_config();
	
	if (!debug) {
		pid = fork();
		if (pid < 0) {
			printf("unable to fork: %s\n", strerror(errno));
			exit(-1);
		}
		if (pid > 0) {
			VERBOSE("logenqueue started and going into background\n");
			_Exit(0);
		}
		setsid();
	}
	
	DEBUG("syslog listen : %s:%d\n", cfg.syslog.bind, cfg.syslog.port);
	DEBUG("gelf listen : %s:%d\n", cfg.gelf.bind, cfg.gelf.port);

	cfg.syslog.fd = udp_listen(cfg.syslog.bind, cfg.syslog.port);
	cfg.gelf.fd = udp_listen(cfg.gelf.bind, cfg.gelf.port);
	if (cfg.syslog.fd == -1 || cfg.gelf.fd == -1) {
		printf("problem listening\n");
		return(-1);
	}

	if (amqp_link(&amqp) < 0) {
		printf("problem with amqp connection!\n");
		return(-1);
	}

	amqp_exchange_declare(amqp.conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				       amqp_cstring_bytes(cfg.amqp.exch_type), 0, 0,
				       amqp_empty_table);
        r = amqp_get_rpc_reply(amqp.conn);
        if (r.reply_type != AMQP_RESPONSE_NORMAL) {
                printf("problem declaring amqp exchange\n");
                return(-1);
        }

	syslog_workers = calloc(cfg.syslog.workers, sizeof(pthread_t));
	gelf_workers = calloc(cfg.gelf.workers, sizeof(pthread_t));

	syslog_workers_data = calloc(cfg.syslog.workers, sizeof(struct worker_data));
	gelf_workers_data = calloc(cfg.gelf.workers, sizeof(struct worker_data));

	for (i = 0; i < cfg.syslog.workers; i++) {
		(syslog_workers_data+i)->id = i;
		pthread_create(syslog_workers+i, NULL, (void *)&syslog_worker, syslog_workers_data+i);
#if __FreeBSD__
		snprintf(tname, sizeof(tname), "syslog_wrkr[%d]", i);
		pthread_set_name_np(*(syslog_workers+i), tname);
#endif
	}

	for (i = 0; i < cfg.gelf.workers; i++) {
		(gelf_workers_data+i)->id = i;
		pthread_create(gelf_workers+i, NULL, (void *)&gelf_worker, gelf_workers_data+i);
#if __FreeBSD__
		snprintf(tname, sizeof(tname), "gelf_wrkr[%d]", i);
		pthread_set_name_np(*(gelf_workers+i), tname);
#endif
	}

	pthread_create(&stats_thread, NULL, (void *)&message_stats, NULL);
#if __FreeBSD__
	snprintf(tname, sizeof(tname), "stats_thread[]");
	pthread_set_name_np(stats_thread, tname);
#endif

	pthread_join(stats_thread, NULL);

	/* STFU Clang */
	free(syslog_workers);
	free(gelf_workers);
	free(syslog_workers_data);
	free(gelf_workers_data);

	return 0;
}
