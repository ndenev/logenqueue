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

#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#include <sys/types.h>
#include <sys/param.h>

#define HOST_NAME_MAX	255

int	parse_config();
int	parse_opts(int*, char***);

extern  int     debug;
extern  int     verbose;

/* DEFAULTS */
#define AMQP_HOST	"localhost"
#define AMQP_PORT	5672
#define	AMQP_USER	"guest"
#define AMQP_PASS	"guest"
#define AMQP_VHOST	"/"
#define AMQP_EXNAME

struct amqp_config {
	char	host[HOST_NAME_MAX];
	u_int	port;
	char	user[255];
	char	pass[255];
	char	vhost[255];
	char	ex_name[255];
	char	ex_type[64];
	int	fd;
};

struct syslog_listener {
	char	bind[HOST_NAME_MAX];
	u_int	port;
	int	fd;
	int	workers;
};

struct gelf_listener {
	char	bind[HOST_NAME_MAX];
	u_int	port;
	int	fd;
	int	workers;

};

struct config {
	struct amqp_config amqp;
	struct syslog_listener syslog;
	struct gelf_listener gelf;
};

#endif
