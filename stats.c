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

#include "dnscache.h"
#include "logenqueue.h"
#include "common.h"
#include "config.h"


#define STATS_TIMEOUT 5

pthread_cond_t	stats_nap;

void
message_stats(void *arg)
{
	struct	all_thr_data_t *workers_data = (struct all_thr_data_t *)arg;
	struct	dnscache_t *cache = workers_data->syslog->cache;
	struct	thr_data_t *stp, *gtp;
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
		pthread_cond_timedwait(&stats_nap, &dummy, &ts);
		pthread_mutex_unlock(&dummy);
		if (dying) {
			pthread_exit(NULL);
		}

		mcs	= 0;
		mcg	= 0;

		if (stats) {
			/* get dns cache stats */
			pthread_rwlock_wrlock(cache->lock);
			cache_hits	= cache->hit;
			cache_miss	= cache->miss;
			cache_full	= cache->full;
			cache_size	= cache->size;
			cache->hit	= 0;
			cache->miss	= 0;
			pthread_rwlock_unlock(cache->lock);

			LOG("dns cache size: [%d/%d] hit/miss:[%d/%d] full:[%d]\n",
				cache_size, DNSCACHESIZE,
				cache_hits / STATS_TIMEOUT,
				cache_miss / STATS_TIMEOUT,
				cache_full);
		}

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
			/* get message count stats and detect wraps */
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

		if (stats) {
			LOG("log msg rate:  %d gelf / %d syslog / %d total\n",
				mcg, mcs, mc);
			LOG("\n");
		}

#if __FreeBSD__ || __linux__
		setproctitle("%d msg/sec", mc);
#endif
	};
}
