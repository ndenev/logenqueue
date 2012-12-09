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
#include <unistd.h>
#include <netdb.h>
#include <math.h>
#include <pthread.h>
#if __FreeBSD__
#include <pthread_np.h>
#endif

#include <amqp.h>
#include <amqp_framing.h>

#include "common.h"
#include "logenqueue.h"
#include "config.h"
#include "dnscache.h"

#ifdef DO_ZLIB
#include <zlib.h>
#endif

#define STATS_TIMEOUT 5
#define	ZLIBD	0
#define	GZIPD	1
#define	CHUNKD	2

#define SYSLOG_BUF 65535
#define GELF_BUF 65535

pthread_cond_t	stats_nap;
volatile int dying = 0;

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
	"GELF",
	NULL
};

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

	amqp->props._flags =
		AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	amqp->props.delivery_mode = 2;
	amqp->props.content_type =
		amqp_cstring_bytes("application/octet-stream");

	amqp->conn = amqp_new_connection();
	cfg.amqp.fd = amqp_open_socket(cfg.amqp.host, cfg.amqp.port);
	if (cfg.amqp.fd < 0) {
		LOG("unable to open amqp socket!\n");
		return(-1);
	}
	amqp_set_sockfd(amqp->conn, cfg.amqp.fd);
	r = amqp_login(amqp->conn, cfg.amqp.vhost, 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, cfg.amqp.user, cfg.amqp.pass);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		LOG("problem logging in amqp broker\n");
		return(-1);
	}
	amqp_channel_open(amqp->conn, 1);
	r = amqp_get_rpc_reply(amqp->conn);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		LOG("problem opening amqp channel\n");
		return(-1);
	}
	return(0);
}

void
message_stats(void *arg)
{
	struct	thr_dat *workers_data = (struct thr_dat *)arg;
	struct	dnscache *cache = workers_data->syslog->cache;
	struct	syslog_thr_dat *stp;
	struct	gelf_thr_dat *gtp;
	int	i;
	u_int	mc;		/* message count */
	u_int	mcs;		/* message count syslog */
	u_int	mcg;		/* message count gelf */
	int	cache_hits, cache_miss, cache_full, cache_size;
	pthread_mutex_t dummy;
        struct timeval tv;
        struct timespec ts;

	pthread_mutex_init(&dummy, NULL);
	pthread_cond_init(&stats_nap, NULL);

	for (;;) {
		gettimeofday(&tv, NULL);
		ts.tv_sec = tv.tv_sec + STATS_TIMEOUT;
		ts.tv_nsec = 0;
		pthread_mutex_lock(&dummy);
		i = pthread_cond_timedwait(&stats_nap, &dummy, &ts);
		pthread_mutex_unlock(&dummy);
		if (dying) {
			pthread_exit(NULL);
		}
		mc = mcs = mcg = 0;
		cache_hits = cache_miss = cache_full = cache_size = 0;

		/* get dns cache stats */
		pthread_rwlock_wrlock(cache->lock);
		cache_hits	= cache->hit;
		cache_miss	= cache->miss;
		cache_full	= cache->full;
		cache_size	= cache->size;
		cache->hit = 0;
		cache->miss = 0;
		pthread_rwlock_unlock(cache->lock);

		DEBUG("dns cache size : %d/%d\n", cache_size, DNSCACHESIZE);
		DEBUG("dns cache hit  : %d/sec\n", cache_hits / STATS_TIMEOUT);
		DEBUG("dns cache miss : %d/sec\n", cache_miss / STATS_TIMEOUT);
		DEBUG("dns cache full : %d\n", cache_full);

		for (i = 0; i < cfg.syslog.workers; i++) {
			stp = &workers_data->syslog[i];
			pthread_mutex_lock(&stp->stat_mtx);
			/* get message count stats and detect wraps */
			if (stp->mc >= stp->old_mc) {
				mcs += stp->mc - stp->old_mc;
				stp->old_mc = stp->mc;
			} else {
				mcs += (UINT_MAX - stp->old_mc) + stp->mc;
			}
			pthread_mutex_unlock(&stp->stat_mtx);
		}
		for (i = 0; i < cfg.gelf.workers; i++) {
			gtp = &workers_data->gelf[i];
			pthread_mutex_lock(&gtp->stat_mtx);
			if (gtp->mc >= gtp->old_mc) {
				mcg += gtp->mc - gtp->old_mc;
				gtp->old_mc = gtp->mc;
			} else {
				mcg += (UINT_MAX - gtp->old_mc) + gtp->mc;
			}
			pthread_mutex_unlock(&gtp->stat_mtx);
		}
		mcs = mcs / STATS_TIMEOUT;
		mcg = mcg / STATS_TIMEOUT;
		mc = mcs + mcg;
		VERBOSE("msg rate total  : %d msg/sec\n", mc);
		VERBOSE("msg rate syslog : %d msg/sec\n", mcs);
		VERBOSE("msg rate gelf   : %d msg/sec\n", mcg);
#if __FreeBSD__ || __linux__
		setproctitle("%d msg/sec", mc);
#endif
		VERBOSE("\n");
	};
}

void
reload(int sig)
{
	LOG("Here goes code to reload the config\n");
}

void
die(int sig)
{
	VERBOSE("Got shutdown request, waiting threads to finish\n");
	dying = 1;
	shutdown(cfg.syslog.fd, SHUT_RD);
	shutdown(cfg.gelf.fd, SHUT_RD);
	pthread_cond_signal(&stats_nap);
}

/*
 * json_escape copies one string to another
 * escaping chars that must be escaped in json (no shit?)
 * these are backslashes, quotes, and special chars.
 */
int
json_escape(char *dst, char *src, int dst_len)
{
	int dst_idx = 0;
	int src_idx, r;

	for (src_idx = 0; src_idx < strlen(src); src_idx++) {
		/* escape control chars */
		if (src[src_idx] < 0x1f) {
			r = snprintf(dst+dst_idx,
					dst_len - dst_idx,
					"\\u%04x",
					src[src_idx]);
			dst_idx += r;
		} else {
			if (src[src_idx] == '\\' || src[src_idx] == '"')
				dst[dst_idx++] = '\\';
			dst[dst_idx++] = src[src_idx];
		}
	}
	dst[dst_idx] = '\0';
	return(strlen(dst));
}

/*
 * parse_syslog_prio() tries to parse syslog
 * message priority from a message string.
 * if successfull it will return pointer
 * to the first char after the syslog priority field,
 * and write the priority to the
 * int pointed by the prio argument.
 * On invalid syslog message it will return NULL.
 */
char *
parse_syslog_prio(char *msg, int *prio)
{
#define SL_PRI_MIN 0
#define SL_PRI_MAX 191
	if (sscanf(msg, "<%3d>", prio) != 1)
		return(NULL);
	if (*prio < SL_PRI_MIN || *prio > SL_PRI_MAX)
		return(NULL);
	return (strchr(msg,'>')+1);
}

void
syslog_worker(void *arg)
{
	struct	syslog_thr_dat *self = (struct syslog_thr_dat *)arg;

	struct	amqp_state_t amqp;
	struct  sockaddr from;
	u_int	ip_len;
	char 	host[_POSIX_HOST_NAME_MAX+1];
	char	*msg, *msg2;
	u_char	buf[SYSLOG_BUF];
	u_char	esc_buf[SYSLOG_BUF*2];
	int	r;
	int	q;
	int	pri;
	struct	tm tim;
	time_t	ts;
	amqp_bytes_t msgb;
	u_char	in[SYSLOG_BUF*3];

	//DEBUG("syslog worker thread #%d started\n", self->id);

	if (amqp_link(&amqp) < 0) {
		LOG("can't connect to amqp from syslog thr #%d\n", self->id);
		return;
	}

	self->mc = 0;
	self->old_mc = 0;
	for (;;) {
		r = recvfrom(cfg.syslog.fd, buf, sizeof(buf),
				MSG_WAITALL, &from, &ip_len);
		if (dying) {
			pthread_exit(NULL);
		}
		if (r < 0) {
			LOG("recvfrom error: %s\n", strerror(errno));
			continue;
		}
		buf[r] = '\0';
		pthread_mutex_lock(&self->stat_mtx);
		self->mc++;
		pthread_mutex_unlock(&self->stat_mtx);

		trytogetrdns(self, &from, host, self->cache);

		json_escape((char *)esc_buf, (char *)buf, sizeof(esc_buf));

		msg = parse_syslog_prio((char *)esc_buf, &pri);
		if (!msg) {
			VERBOSE("invalid syslog [%s]->\"%s\"\n", host, esc_buf);
			continue;
		}

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

#if 0
		/* allocate deflate state */
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
		if (ret != Z_OK) {
			printf("deflateInit error\n");
			continue;
		}

		strm.avail_in = strlen((char *)in);
		strm.next_in = in;
		strm.avail_out = SYSLOG_BUF*3;
		strm.next_out = out;
		flush = Z_FINISH;
		ret = deflate(&strm, flush);

		(void)deflateEnd(&strm);

		msgb.len = (SYSLOG_BUF*3) - strm.avail_out;
		msgb.bytes = out;
#endif
		msgb.len = strlen((char *)in);
		msgb.bytes = in;

		q = amqp_basic_publish(amqp.conn, 1,
					amqp_cstring_bytes(cfg.amqp.ex_name),
					amqp_cstring_bytes(cfg.amqp.host), 0, 0,
					&amqp.props, msgb);
		if (q < 0)
			LOG("failure publishing message to amqp\n");

	}

}

void
gelf_worker(void *arg)
{
	struct gelf_thr_dat *self = (struct gelf_thr_dat *)arg;

	struct  amqp_state_t amqp;
	amqp_bytes_t msgb;
	u_char	buf[GELF_BUF];
	int	r;
	int	q;

	int	ret = 0;
	z_stream strm;
	u_char	out[GELF_BUF*3];


	//DEBUG("gelf worker thread #%d started\n", self->id);

	if (amqp_link(&amqp) < 0) {
		LOG("can't connect to amqp from gelf thr #%d\n", self->id);
		return;
	}

	self->mc = 0;
	self->old_mc = 0;
	for (;;) {
		r = recvfrom(cfg.gelf.fd, &buf, sizeof(buf), 0, NULL, NULL);
		if (dying) {
			pthread_exit(NULL);
		}
		if (r < 0) {
			LOG("recvfrom error: %s\n", strerror(errno));
			continue;
		}

		pthread_mutex_lock(&self->stat_mtx);
		self->mc++;
		pthread_mutex_unlock(&self->stat_mtx);

		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		strm.avail_in = 0;
		strm.next_in = Z_NULL;

#define	GELF_MAGIC(type) !bcmp(buf, gelf_magic[type], sizeof(gelf_magic[type]))

		if ( (GELF_MAGIC(ZLIBD)) || (GELF_MAGIC(GZIPD)) ) {
			//DEBUG("Received ZLIB'd GELF message.\n");
			ret = inflateInit2(&strm, 32+MAX_WBITS);
		} else if (GELF_MAGIC(GZIPD)) {
			//DEBUG("Received GZIP'd GELF message.\n");
			ret = inflateInit2(&strm, 32+MAX_WBITS);
		} else if (GELF_MAGIC(CHUNKD)) {
			LOG("Received CHUNKED GELF message. It's not supported currently!\n");
			continue;
		} else {
			LOG("Unknown GELF type. Maybe RAW? Bailing out.");
			continue;
		}

		if (ret != Z_OK) {
			printf("inflateInit error\n");
			continue;
		}

		strm.avail_in = r;
		strm.next_in = buf;
		strm.avail_out = GELF_BUF*2;
		strm.next_out = out;
		ret = inflate(&strm, Z_FINISH);

		msgb.len = (GELF_BUF*2) - strm.avail_out;			
		msgb.bytes = out;

		q = amqp_basic_publish(amqp.conn, 1,
					amqp_cstring_bytes(cfg.amqp.ex_name),
					amqp_cstring_bytes(cfg.amqp.host), 0, 0,
					&amqp.props, msgb);
		if (q < 0)
			LOG("failure publishing message to amqp\n");

		(void)inflateEnd(&strm);

	}
}

int
udp_listen(char *bindaddr, u_int port)
{
	int udpsock_fd, noptval, r;
	struct sockaddr_in staddr;

	udpsock_fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (udpsock_fd == -1) {
		LOG("error creating socket!: %s\n", strerror(errno));
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
		LOG("error calling setsockopt: %s\n", strerror(errno));
		return(-1);
	}

	noptval = 1;
	r = setsockopt(udpsock_fd, SOL_SOCKET, SO_REUSEADDR,
		(const void *)&noptval, sizeof(noptval));
	if (r == -1) {
		LOG("error calling setsockopt: %s\n", strerror(errno));
		return(-1);
	}

	r = bind(udpsock_fd, (struct sockaddr *)&staddr, sizeof(staddr));
	if (r != 0) {
		LOG("bind: %s\n", bindaddr);
		LOG("error binding to socket: %s\n", strerror(errno));
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
	pthread_t	stats_thread;
	pthread_t	dnscache_cleaner;
	pthread_t	*syslog_workers;
	pthread_t	*gelf_workers;
	struct thr_dat	 	 workers_data;
	struct syslog_thr_dat	*stp;
	struct gelf_thr_dat	*gtp;
	struct dnscache		dnscache;
	pthread_rwlock_t	dnscache_lock;
	char	tname[17];
	amqp_rpc_reply_t r;

	signal(SIGHUP, reload);
	signal(SIGINT, die);
	signal(SIGTERM, die);

	if (parse_opts(&argc, &argv) < 0) {
		LOG("problem parsing command line arguments/options\n");
		exit(-1);
	}

	parse_config();

	if (!debug) {
		pid = fork();
		if (pid < 0) {
			LOG("unable to fork: %s\n", strerror(errno));
			exit(-1);
		}
		if (pid > 0) {
			VERBOSE("logenqueue started...\n");
			_Exit(0);
		}
		setsid();
	}

	DEBUG("syslog listen : %s:%d\n", cfg.syslog.bind, cfg.syslog.port);
	DEBUG("gelf listen : %s:%d\n", cfg.gelf.bind, cfg.gelf.port);

	cfg.syslog.fd = udp_listen(cfg.syslog.bind, cfg.syslog.port);
	cfg.gelf.fd = udp_listen(cfg.gelf.bind, cfg.gelf.port);
	if (cfg.syslog.fd == -1 || cfg.gelf.fd == -1) {
		LOG("problem listening\n");
		return(-1);
	}

	if (amqp_link(&amqp) < 0) {
		LOG("problem with amqp connection!\n");
		return(-1);
	}

	amqp_exchange_declare(amqp.conn, 1,
				amqp_cstring_bytes(cfg.amqp.ex_name),
				amqp_cstring_bytes(cfg.amqp.ex_type),
				0, 0, amqp_empty_table);
        r = amqp_get_rpc_reply(amqp.conn);
        if (r.reply_type != AMQP_RESPONSE_NORMAL) {
                LOG("problem declaring amqp exchange\n");
                return(-1);
        }

	/* allocate thread and thread storage structures */
	syslog_workers = calloc(cfg.syslog.workers, sizeof(pthread_t));
	gelf_workers = calloc(cfg.gelf.workers, sizeof(pthread_t));

	workers_data.syslog = calloc(cfg.syslog.workers,
					sizeof(struct syslog_thr_dat));
	workers_data.gelf  = calloc(cfg.gelf.workers,
					sizeof(struct gelf_thr_dat));

	pthread_rwlock_init(&dnscache_lock, NULL);
	memset(&dnscache, 0, sizeof(dnscache));
	dnscache.lock = &dnscache_lock;

	/* create syslog workers */
	for (i = 0; i < cfg.syslog.workers; i++) {
		stp = &workers_data.syslog[i];
		stp->id = i;
		stp->cache = &dnscache;
		pthread_mutex_init(&stp->stat_mtx, NULL);
		pthread_create(&syslog_workers[i], NULL,
				(void *)&syslog_worker, stp);
		snprintf(tname, sizeof(tname), "syslog_wrkr[%d]", i);
#if __FreeBSD__
		pthread_set_name_np(*(&syslog_workers[i]), tname);
#endif
	}

	/* create gelf workers */
	for (i = 0; i < cfg.gelf.workers; i++) {
		gtp = &workers_data.gelf[i];
		gtp->id = i;
		pthread_mutex_init(&gtp->stat_mtx, NULL);
		pthread_create(&gelf_workers[i], NULL,
				(void *)&gelf_worker, gtp);
		snprintf(tname, sizeof(tname), "gelf_wrkr[%d]", i);
#if __FreeBSD__
		pthread_set_name_np(gelf_workers[i], tname);
#endif
	}

	/* create statistics thread */
	pthread_create(&stats_thread, NULL,
			(void *)&message_stats, &workers_data);
	snprintf(tname, sizeof(tname), "stats_thread[]");
#if __FreeBSD__
	pthread_set_name_np(stats_thread, tname);
#endif

	/* create dnscache purge thread */
	pthread_create(&dnscache_cleaner, NULL,
			(void *)&dnscache_expire, &dnscache);

	snprintf(tname, sizeof(tname), "dns_cleaner[]");
#if __FreeBSD__
	pthread_set_name_np(dnscache_cleaner, tname);
#endif

	/* wait for syslog worker threads */
	for (i = 0; i < cfg.syslog.workers; i++)
		pthread_join(syslog_workers[i], NULL);

	/* wait for gelf worker threads */
	for (i = 0; i < cfg.gelf.workers; i++)
		pthread_join(gelf_workers[i], NULL);

	/* wait for stats and dnscache cleaner threads to finish */
	pthread_join(stats_thread, NULL);
	pthread_join(dnscache_cleaner, NULL);

	/* STFU Clang */
	free(syslog_workers);
	free(gelf_workers);
	free(workers_data.syslog);
	free(workers_data.gelf);

	return 0;
}
