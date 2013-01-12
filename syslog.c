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

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>

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

#define SYSLOG_BUF 65535

static __inline const char *
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
		"GELF",
		NULL
	};

	if (facility > 0 && facility < 23)
		return (f2s[facility]);
	else
		return (f2s[24]);
}

/*
 * json_escape copies one string to another
 * escaping chars that must be escaped in json (no shit?)
 * these are backslashes, quotes, and special chars.
 */
static __inline int
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
static __inline char *
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
	struct	thr_data_t *self = (struct thr_data_t *)arg;

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
#ifdef DO_ZLIB
	int	flush;
	z_stream strm;
	u_char	out[SYSLOG_BUF*3];
#endif
	u_char	in[SYSLOG_BUF*3];

	DEBUG("syslog worker thread #%d started\n", self->id);

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
            DEBUG("syslog worker thread #%d terminating\n", self->id);
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

		trytogetrdns(&self->stat_mtx, &from, host, self->cache);

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

#ifdef DO_ZLIB
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
#else
		msgb.len = strlen((char *)in);
		msgb.bytes = in;
#endif
		q = amqp_basic_publish(amqp.conn, 1,
					amqp_cstring_bytes(cfg.amqp.ex_name),
					amqp_cstring_bytes(cfg.amqp.host), 0, 0,
					&amqp.props, msgb);
		if (q < 0)
			LOG("failure publishing message to amqp\n");

	}

}
