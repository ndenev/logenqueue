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

#ifndef LOGENQUEUE_H_INCLUDED
#define LOGENQUEUE_H_INCLUDED

#include <limits.h>

#define LOG(...)	printf(__VA_ARGS__)
#define DEBUG(...)      if (debug>0) printf(__VA_ARGS__)
#define VERBOSE(...)    if (verbose>0) printf(__VA_ARGS__)

int     debug = 0;
int     verbose = 0;

#define DNSCACHESIZE 4096
struct dnscache_entry {
	u_int32_t       from;
        char    host[_POSIX_HOST_NAME_MAX+1];
        int     ts;
};

struct dnscache {
        struct  dnscache_entry entry[DNSCACHESIZE];
        int     size;
        int     hit;
        int     miss;
        int     full;
	pthread_rwlock_t *lock;
};

struct syslog_thr_dat {
	int     id;
	u_int	msg_count;
	u_int	old_msg_count;
	pthread_mutex_t stat_mtx;
	struct dnscache *cache;
};

struct gelf_thr_dat {
	int     id;
	u_int	msg_count;
	u_int	old_msg_count;
	pthread_mutex_t stat_mtx;
};

struct thr_dat {
	struct syslog_thr_dat *syslog;
	struct gelf_thr_dat *gelf;
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


#endif
