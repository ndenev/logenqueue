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
//#include "gelf.h"
#include "common.h"
//#include "logenqueue.h"
#include "config.h"
//#include "dnscache.h"

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
