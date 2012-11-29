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
#include <zlib.h>
#include <json/json.h>
#include <event2/event.h>
#include <amqp.h>
#include <amqp_framing.h>
#include <netdb.h>

#include "logenqueue.h"
#include "config.h"

struct  config  cfg;

static int msg_rcvd = 0;
static int msg_pub = 0;

#define STATS_TIMEOUT	10

#define	ZLIBD	0
#define	GZIPD	1
#define	CHUNKD	2

static const char gelf_magic[3][2] = {
	{ 0x78, 0x9c }, 
	{ 0x1f, 0x8b },
	{ 0x1e, 0x0f },
};

static const char *
fac2str(int facility) {
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
	if (facility > 0 && facility < 23)
		return (f2s[facility]);
	else
		return (f2s[24]);
}

void
message_stats(int fd __unused, short event __unused, void *arg __unused)
{
	printf("Receiving %d msgs/sec\n", msg_rcvd / STATS_TIMEOUT);
	printf("Publishing %d msgs/sec\n", msg_pub / STATS_TIMEOUT);
	msg_rcvd = 0;
	msg_pub = 0;
}

void
got_syslog_msg(int fd, short event, void *arg)
{
	amqp_connection_state_t *conn = arg;
	amqp_basic_properties_t props;
	struct sockaddr from;
	struct hostent *hp;
	char *host;
	char ip[256];
	char *msg, *msg2;
	unsigned int ip_len;
	char buf[8129];
	int r;
	int pri;
	struct tm tim;
	time_t ts;
	amqp_bytes_t msgb;

	if (event != EV_READ) {
		fprintf(stderr, "not read event?\n");
		return;
	}

	ip_len = sizeof(from);
	r = recvfrom(fd, buf, sizeof(buf), 0, &from, &ip_len);
	if (r < 0)
		return;
	msg_rcvd++;
	if ((hp = gethostbyaddr((const void *)&from.sa_data+2, sizeof(struct in_addr), AF_INET))) {
		host = hp->h_name;
	} else {
		inet_ntop(from.sa_family, from.sa_data+2, ip, sizeof(ip));
		host = ip;
	}

	buf[r] = '\0';

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.delivery_mode = 2; /* persistent delivery mode */
	props.content_type = amqp_cstring_bytes("application/octet-stream");

	if (buf[0] != '<') {
		printf("INVALID SYSLOG FORMAT! : %s\n", buf);
		return;
	}
	msg = strchr(buf, '>');
	msg++;
	pri = (int)strtol(buf+1,(char **)NULL,10);

	int severity = pri & 0x07;
	int facility = pri >> 3;

	msg2 = strptime(msg, "%b %d %H:%M:%S", &tim);
	if (msg2) {
		ts = mktime(&tim);
		msg = msg2;
	} else {
		ts = time(NULL);
	}

	json_object * jobj = json_object_new_object();

	json_object *j_version = json_object_new_string("1.0");
	json_object *j_host = json_object_new_string(host);
	json_object *j_short_message = json_object_new_string(msg);
	json_object *j_full_message = json_object_new_string(buf);
	json_object *j_timestamp = json_object_new_double(ts);
	json_object *j_level = json_object_new_int(severity);
	json_object *j_facility = json_object_new_string(fac2str(facility));
	json_object *j_file = json_object_new_string("");
	json_object *j_line = json_object_new_int(0);

	json_object_object_add(jobj,"version", j_version);
	json_object_object_add(jobj,"host", j_host);
	json_object_object_add(jobj,"short_message", j_short_message);
	json_object_object_add(jobj,"full_message", j_full_message);
	json_object_object_add(jobj,"timestamp", j_timestamp);
	json_object_object_add(jobj,"level", j_level);
	json_object_object_add(jobj,"facility", j_facility);
	json_object_object_add(jobj,"file", j_file);
	json_object_object_add(jobj,"line", j_line);

#define	 CHUNK	9216
	int flush;
	z_stream strm;
	u_char in[CHUNK];
	u_char out[CHUNK*2];

	memset(in, 0, CHUNK);
	memset(out, 0, CHUNK*2);

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	deflateInit(&strm, Z_DEFAULT_COMPRESSION);

	strncpy((char *)in, json_object_to_json_string(jobj), sizeof(in));
	strm.avail_in = strlen((char *)in);
	strm.next_in = in;
	strm.avail_out = CHUNK*2;
	strm.next_out = out;
	flush = Z_FINISH;
	deflate(&strm, flush);

	(void)deflateEnd(&strm);

	msgb.len = (CHUNK*2) - strm.avail_out;
	msgb.bytes = out;

	amqp_basic_publish(*conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
			    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
			    &props, msgb);
	msg_pub++;

	return;
}

void
got_gelf_msg(int fd, short event, void *arg)
{
	amqp_connection_state_t *conn = arg;
	amqp_basic_properties_t props;
	char buf[8129];
	int r;
	amqp_bytes_t msgb;

	if (event != EV_READ) {
		fprintf(stderr, "not read event?\n");
		return;
	}

	r = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
	if (r < 0)
		return;
#define	GELF_MAGIC(type)	bcmp(buf, gelf_magic[type], sizeof(gelf_magic[type]))

	if (!GELF_MAGIC(ZLIBD)) {
		DEBUG("Received ZLIB'd GELF message.\n");
	} else if (!GELF_MAGIC(GZIPD)) {
		DEBUG("Received GZIP'd GELF message.\n");
	} else if (!GELF_MAGIC(CHUNKD)) {
		DEBUG("Received CHUNKED GELF message.\n");
	} else {
		LOG("Unknown GELF type. Maybe RAW? Bailing out.");
		return;
	}

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.delivery_mode = 2; /* persistent delivery mode */
	props.content_type = amqp_cstring_bytes("application/octet-stream");

	msgb.len = r;
	msgb.bytes = buf;

	amqp_basic_publish(*conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
			    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
			    &props, msgb);
	msg_pub++;

	return;
}

int udp_listen(char *bindaddr, u_int port)
{
	int udpsock_fd, nreqflags, noptval, ret;
	struct sockaddr_in staddr;

	udpsock_fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (udpsock_fd == -1) {
		printf("error creating socket!: %s\n", strerror(errno));
		exit(-1);
	}

	nreqflags = fcntl(udpsock_fd, F_GETFL, 0);
	ret = fcntl(udpsock_fd, F_SETFL, nreqflags | O_NONBLOCK);
	if (ret == -1) {
		printf("error calling fcntl F_SETFL: %s\n", strerror(errno));
		exit(-1);
	}
	memset(&staddr, 0, sizeof(struct sockaddr_in));
	staddr.sin_addr.s_addr = inet_addr(bindaddr);
	staddr.sin_port = htons(port);
	staddr.sin_family = AF_INET;

	noptval = 1024 * 1024;
	ret = setsockopt(udpsock_fd, SOL_SOCKET, SO_RCVBUF,
		(const void *)&noptval, sizeof(noptval));

	noptval = 1;
	ret = setsockopt(udpsock_fd, SOL_SOCKET, SO_REUSEADDR,
		(const void *)&noptval, sizeof(noptval));
	if (ret == -1) {
		printf("error calling setsockopt: %s\n", strerror(errno));
		exit(-1);
	}

	ret = bind(udpsock_fd, (struct sockaddr *)&staddr, sizeof(staddr));
	if (ret != 0) {
		printf("bind: %s\n", bindaddr);
		printf("error binding to socket: %s\n", strerror(errno));
		exit(-1);
	}

	return(udpsock_fd);
}

int main(int argc, char **argv)
{
	struct event *eve;
	struct event_base *base;
	struct timeval tv = { STATS_TIMEOUT, 0 };

	amqp_connection_state_t conn;

	parse_opts(&argc, &argv);

	parse_config();

	base = event_base_new();

	cfg.syslog.fd = udp_listen(cfg.syslog.bind,
			cfg.syslog.port);
	cfg.gelf.fd = udp_listen(cfg.gelf.bind,
			cfg.gelf.port);

	conn = amqp_new_connection();
	cfg.amqp.fd = amqp_open_socket(cfg.amqp.host, cfg.amqp.port);
	amqp_set_sockfd(conn, cfg.amqp.fd);
	amqp_login(conn, cfg.amqp.vhost, 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, cfg.amqp.user, cfg.amqp.pass);
	amqp_channel_open(conn, 1);
	amqp_get_rpc_reply(conn);

	amqp_exchange_declare(conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				       amqp_cstring_bytes(cfg.amqp.exch_type),
				       0,
				       0,
				       amqp_empty_table);

	eve = event_new(base, cfg.syslog.fd, EV_READ | EV_PERSIST, got_syslog_msg, &conn);
	event_add(eve, NULL);

	eve = event_new(base, cfg.gelf.fd, EV_READ | EV_PERSIST, got_gelf_msg, &conn);
	event_add(eve, NULL);

	eve = event_new(base, -1, EV_PERSIST, message_stats, NULL);
	event_add(eve, &tv);

	event_base_dispatch(base);

	return 0;
}
