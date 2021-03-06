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
#include "config.h"
#include "dnscache.h"

/*
 * This function tries to get reverse DNS
 * on the given host and return the hostname,
 * or if it fails returns the IP.
 * The result is cached.
 */
void __inline
trytogetrdns(pthread_mutex_t *stat_mtx, struct sockaddr *from, char *host, struct dnscache_t *cache)
{
	struct	hostent *hp = NULL;
	void	*src = from->sa_data+2;

	int i;
	int oldest_ts;
	int oldest_idx;
	int now = time(NULL);

	/* fast path, reader only when cache warm */
	for (i = 0; i < DNSCACHESIZE; i++) {
		pthread_rwlock_rdlock(cache->lock);
		if (cache->entry[i].from == *(u_int32_t *)src) {
			pthread_mutex_lock(stat_mtx);
			cache->hit++;
			pthread_mutex_unlock(stat_mtx);
			strncpy(host, cache->entry[i].host, _POSIX_HOST_NAME_MAX);
			cache->entry[i].ts = now;
			pthread_rwlock_unlock(cache->lock);
			return;
		}
		pthread_rwlock_unlock(cache->lock);
	}

	/* fall back to dns query and cache update */
	pthread_rwlock_wrlock(cache->lock);

	/* retry the query from cache, in case other thread won the race and cached it */
	for (i = 0; i < DNSCACHESIZE; i++) {
		if (cache->entry[i].from == *(u_int32_t *)src) {
			/* we lost the race but some other thread did the dirty work */
			pthread_mutex_lock(stat_mtx);
			cache->hit++;
			pthread_mutex_unlock(stat_mtx);
			strncpy(host, cache->entry[i].host, _POSIX_HOST_NAME_MAX);
			cache->entry[i].ts = now;
			pthread_rwlock_unlock(cache->lock);
			return;
		}
	}

	hp = gethostbyaddr((const void *)src, sizeof(struct in_addr), AF_INET);
	if (hp)
		strncpy(host, hp->h_name, _POSIX_HOST_NAME_MAX);
	else
		inet_ntop(from->sa_family, src, host, _POSIX_HOST_NAME_MAX);

	oldest_ts = now;
	oldest_idx = DNSCACHESIZE+1;

	for (i = 0; i < DNSCACHESIZE; i++) {
		if (cache->entry[i].ts < oldest_ts) {
			oldest_ts = cache->entry[i].ts;
			oldest_idx = i;
		}
	}

	cache->miss++;

	if (oldest_idx == DNSCACHESIZE+1 || cache->size == DNSCACHESIZE) {
		cache->full++;
	} else {
		if (cache->entry[oldest_idx].ts == 0)
			cache->size++;

		cache->entry[oldest_idx].from = *(u_int32_t *)src;
		strncpy(cache->entry[oldest_idx].host, host, _POSIX_HOST_NAME_MAX);
		cache->entry[oldest_idx].ts = now;
	}
	pthread_rwlock_unlock(cache->lock);
}

void
dnscache_expire(void *arg)
{
	struct dnscache_t *cache = (struct dnscache_t *)arg;
	int i;
	int oldest_ts;
	int tosleep;
	int now;

	tosleep = DNSCACHETTL;

	DEBUG("dnscache cleaner thread starting\n");

	for (;;) {
		DEBUG("dnscache cleaner thread sleeping for %d secs\n", tosleep);
		sleep(tosleep);
		if (dying) {
			DEBUG("dnscache cleaner thread terminating\n");
			pthread_exit(NULL);
		}
		pthread_rwlock_wrlock(cache->lock);
		oldest_ts = now = time(NULL);
		for (i = 0; i < DNSCACHESIZE; i++) {
			if (!cache->entry[i].ts)
				continue;
			/* purge old entry */
			if (cache->entry[i].ts < now - DNSCACHETTL) {
				DEBUG("dnscache cleaner thread purging entry: %s\n", cache->entry[i].host);
				cache->entry[i].ts = 0;
				cache->entry[i].from = 0;
				cache->entry[i].host[0] = '\0';
				cache->size--;
                continue;
			}
			/* remember oldest entry */
			if (cache->entry[i].ts < oldest_ts) {
				oldest_ts = cache->entry[i].ts;
			}
		}
		oldest_ts = now - oldest_ts;
        if (oldest_ts > DNSCACHETTL)
            tosleep = oldest_ts;
        else
            tosleep = DNSCACHETTL;

		pthread_rwlock_unlock(cache->lock);
	}
}
