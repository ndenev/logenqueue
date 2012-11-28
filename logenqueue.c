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

#include "config.h"

struct  config  cfg;

char *fac2str[] = {
	"kernel",
	"user-level",
	"mail",
	"system daemon",
	"security/authorization",
	"syslogd",
	"line printer",
	"network news",
	"UUCP",
	"clock",
	"security/authorization",
	"FTP",
	"NTP",
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
	"local7"
};

void
got_msg(int fd, short event, void *arg)
{
	amqp_connection_state_t *conn = arg;
	amqp_basic_properties_t props;
	struct sockaddr from;
	struct hostent *hp;
	char host[256];
	char *msg, *msg2;
	unsigned int host_len;
	char buf[8129];
	int r;
	int pri;
	struct tm tim;
	time_t ts;

	if (event != EV_READ) {
		fprintf(stderr, "not read event?\n");
		return;
	}

	host_len = sizeof(from);
	r = recvfrom(fd, buf, sizeof(buf), 0, &from, &host_len);
	if ((hp = gethostbyaddr((const void *)&from.sa_data+2, sizeof(struct in_addr), AF_INET))) {
		strncpy(host, hp->h_name, sizeof(host));
	} else {
		inet_ntop(from.sa_family, from.sa_data+2, host, sizeof(host));
	}

	buf[r] = '\0';

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.delivery_mode = 2; /* persistent delivery mode */
	//props.content_type = amqp_cstring_bytes("text/plain");
	props.content_type = amqp_cstring_bytes("application/octet-stream");

	if (fd == cfg.syslog.fd) {
		if (buf[0] != '<') {
			printf("INVALID SYSLOG FORMAT!\n");
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
		json_object *j_facility = json_object_new_string(fac2str[facility]);
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
		strm.avail_out = CHUNK;
		strm.next_out = out;
		flush = Z_FINISH;
		deflate(&strm, flush);

		//out[strm.avail_out+1] = '\0';

		(void)deflateEnd(&strm);

		//printf("%s\n",json_object_to_json_string(jobj));

		amqp_basic_publish(*conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
				    &props, amqp_cstring_bytes((char *)out));
	} else if (fd == cfg.gelf.fd) {
		amqp_basic_publish(*conn, 1, amqp_cstring_bytes(cfg.amqp.exch_name),
				    amqp_cstring_bytes(cfg.amqp.host), 0, 0,
				    &props, amqp_cstring_bytes(buf));
	}

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

	noptval = 1;
	ret = setsockopt(udpsock_fd, SOL_SOCKET, SO_REUSEADDR,
		(const void *)&noptval, sizeof(noptval));
	if (ret == -1) {
		printf("error calling setsockopt: %s\n", strerror(errno));
		exit(-1);
	}

	ret = bind(udpsock_fd, (struct sockaddr *)&staddr, sizeof(staddr));
	if (ret != 0) {
		printf("error binding to socket: %s\n", strerror(errno));
		printf("bind: %s\n", bindaddr);
		exit(-1);
	}

	return(udpsock_fd);
}

int main(int argc, char **argv)
{
	struct event *eve;
	struct event_base *base;

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

	eve = event_new(base, cfg.syslog.fd, EV_READ | EV_PERSIST, got_msg, &conn);
	event_add(eve, NULL);
	eve = event_new(base, cfg.gelf.fd, EV_READ | EV_PERSIST, got_msg, &conn);
	event_add(eve, NULL);

	event_base_dispatch(base);

	return 0;
}
