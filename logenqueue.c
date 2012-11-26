#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <event2/event.h>
#include <yaml.h>
#include <amqp.h>
#include <amqp_framing.h>

#include "config.h"
#include "logenqueue.h"

#ifndef DEFAULT_CONFIG_FILE
#define DEFAULT_CONFIG_FILE "logenqueue-conf.yml"
#endif

const char *exchange = "syslog";
const char *exchangetype = "topic";
const char *routingkey = "#";

int	debug = 0;
int	verbose = 0;
char	config_file[MAXPATHLEN] = DEFAULT_CONFIG_FILE;

void
parse_opts(int *argc, char ***argv)
{
	int opt;

	static struct option longopts[] = {
		{ "conf",	required_argument,	NULL,	'c' },
		{ "debug",	no_argument,		NULL,	'd' },
		{ "verbose",	no_argument,		NULL,	'v' },
		{ NULL,		0,			NULL,	0 },
	};

        while ((opt = getopt_long(*argc, *argv,
                                "c:dv", longopts, NULL)) != -1) {
                switch (opt) {
                        case 'c':
				strncpy(config_file, optarg, MAXPATHLEN);
                                break;
                        case 'd':
                                debug++;
                                break;
                        case 'v':
                                verbose++;
                                break;
			default:
				break;
		}
	}
}

void
got_msg(int fd, short event, void *arg)
{
	amqp_connection_state_t *conn = arg;
	amqp_basic_properties_t props;
	struct sockaddr from;
	char host[32];
	unsigned int host_len;
	char buf[8129];
	int r;

	if (event != EV_READ) {
		fprintf(stderr, "not read event?\n");
		return;
	}

	host_len = sizeof(from);
	r = recvfrom(fd, buf, sizeof(buf), 0, &from, &host_len);
	buf[r] = '\0';
	inet_ntop(from.sa_family, from.sa_data+2, host, sizeof(host));

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");
	props.delivery_mode = 2; /* persistent delivery mode */
	amqp_basic_publish(*conn, 1, amqp_cstring_bytes(exchange),
				    amqp_cstring_bytes(host),
				    0,
				    0,
				    &props,
				    amqp_cstring_bytes(buf));

	printf("Got Message from %s: %s", host, buf);

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
		exit(-1);
	}

	return(udpsock_fd);
}

int main(int argc, char **argv)
{
	struct event *eve;
	struct event_base *base;
	int udpsock_fd, amqpsock_fd;
	amqp_connection_state_t conn;

	parse_opts(&argc, &argv);

	parse_config();

	base = event_base_new();

	udpsock_fd = udp_listen("0.0.0.0", 5140);

	conn = amqp_new_connection();
	//amqpsock_fd = amqp_open_socket("10.128.2.10", 5672);
	amqpsock_fd = amqp_open_socket("10.0.0.13", 5672);
	amqp_set_sockfd(conn, amqpsock_fd);
	amqp_login(conn, "/", 0, 131072, 0,
		AMQP_SASL_METHOD_PLAIN, "guest", "guest");
	amqp_channel_open(conn, 1);
	amqp_get_rpc_reply(conn);

	amqp_exchange_declare(conn, 1, amqp_cstring_bytes(exchange),
				       amqp_cstring_bytes(exchangetype),
				       0,
				       0,
				       amqp_empty_table);

	eve = event_new(base, udpsock_fd, EV_READ | EV_PERSIST, got_msg, &conn);
	event_add(eve, NULL);

	event_base_dispatch(base);

	return 0;
}
