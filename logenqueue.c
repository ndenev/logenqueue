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

#include "amqp.h"
#include "syslog.h"
#include "gelf.h"
#include "common.h"
#include "logenqueue.h"
#include "config.h"
#include "dnscache.h"
#include "stats.h"

#ifdef DO_ZLIB
#include <zlib.h>
#endif

#define STATS_TIMEOUT 5
#define	ZLIBD	0
#define	GZIPD	1
#define	CHUNKD	2

#define SYSLOG_BUF 65535
#define GELF_BUF 65535

volatile int dying = 0;

static void
reload(int sig)
{
	LOG("Here goes code to reload the config\n");
}

static void
die(int sig)
{
	VERBOSE("Got shutdown request, waiting threads to finish\n");
	dying = 1;
	shutdown(cfg.syslog.fd, SHUT_RD);
	shutdown(cfg.gelf.fd, SHUT_RD);
	pthread_cond_signal(&stats_nap);
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
	struct all_thr_data	workers_data;
	struct thr_data		*stp, *gtp;
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

	if (!dontfork) {
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
					sizeof(struct thr_data));
	workers_data.gelf  = calloc(cfg.gelf.workers,
					sizeof(struct thr_data));

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
		gtp->cache = &dnscache;
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
