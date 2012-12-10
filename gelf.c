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
#include "gelf.h"
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

#define GELF_BUF 65535

void
gelf_worker(void *arg)
{
	struct gelf_thr_dat *self = (struct gelf_thr_dat *)arg;

	struct  amqp_state_t amqp;
	struct  sockaddr from;
	u_int   ip_len;
	char    host[_POSIX_HOST_NAME_MAX+1];
	amqp_bytes_t msgb;
	u_char	buf[GELF_BUF];
	int	r;
	int	q;
	u_int64_t msg_id;
	u_int8_t msg_seq_num;
	u_int8_t msg_chunks;

	//DEBUG("gelf worker thread #%d started\n", self->id);

	if (amqp_link(&amqp) < 0) {
		LOG("can't connect to amqp from gelf thr #%d\n", self->id);
		return;
	}

	self->mc = 0;
	self->old_mc = 0;
	for (;;) {
		r = recvfrom(cfg.gelf.fd, buf, sizeof(buf),
				MSG_WAITALL, &from, &ip_len);
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

#define	GELF_MAGIC(buf, type) bcmp(buf, gelf_magic[type], sizeof(gelf_magic[type]))

		if (!GELF_MAGIC(buf, ZLIBD)) {
			//DEBUG("Received ZLIB'd GELF message.\n");
		} else if (!GELF_MAGIC(buf, GZIPD)) {
			//DEBUG("Received GZIP'd GELF message.\n");
		} else if (!GELF_MAGIC(buf, CHUNKD)) {
			trytogetrdns(&self->stat_mtx, &from, host, self->cache);
			/* skip over chunked gelf magic and parse header */
			bcopy(buf+2, &msg_id, 8);
			bcopy(buf+10, &msg_seq_num, 1);
			bcopy(buf+11, &msg_chunks, 1);

			LOG("Received CHUNKED GELF message from (%s)\n", host);
			LOG("chunked_gelf: size: %d msg_id:%lu msg_seq_num:%u msg_chunks:%u\n",
				r, msg_id, msg_seq_num, msg_chunks);

			// XXX: drop broken gelf messages for now, possible false positives but unlikely	
			//if (msg_chunks > 128) {
			if (msg_chunks > 10) {
				LOG("chunked gelf chunks too much!\n");
				continue;
			}

			if (msg_seq_num > 127) {
				LOG("chunked gelf seq number too big\n");
				continue;
			}

			/* check chunked gelf inner header magic */
			/* unfortunately we can do this only on the first msg */
			if (msg_seq_num == 0) {
				if (!GELF_MAGIC(buf+12, ZLIBD)) {
					LOG("ZLIBD CHUNKED GELF\n");
				} else if (!GELF_MAGIC(buf+13, GZIPD)) {
					LOG("GZIPD CHUNKED GELF\n");
				} else {
					LOG("BAD CHUNKED GELF?\n");
					continue;
				}
			}

#if 0
			int i;
			int l = 1;
			for (i=0; i<r; i++) {
				printf("%2x ", buf[i]);
				if (l++ == 20) {
					printf("\n");
					l = 1;
				}
			}
			printf("\n");
			for (i=0; i<r; i++) {
				printf("%c", buf[i]);
				if (l++ == 20) {
					printf("\n");
					l = 1;
				}
			}
			printf("\n");
#endif
		} else {
			trytogetrdns(&self->stat_mtx, &from, host, self->cache);
			LOG("Received message with unknown GELF type from (%s). Maybe RAW? Bailing out.\n",
				host);
			continue;
		}

		msgb.len = r;
		msgb.bytes = buf;

		q = amqp_basic_publish(amqp.conn, 1,
					amqp_cstring_bytes(cfg.amqp.ex_name),
					amqp_cstring_bytes(cfg.amqp.host), 0, 0,
					&amqp.props, msgb);
		if (q < 0)
			LOG("failure publishing message to amqp\n");

	}
}
