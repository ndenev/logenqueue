#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <err.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <event2/event.h>
#include <yaml.h>
#include <amqp.h>
#include <amqp_framing.h>

#include "logenqueue.h"

#define CONFIG_FILE "logenqueue-conf.yml"

const char *exchange = "syslog";
const char *exchangetype = "topic";
const char *routingkey = "#";

void got_msg(int fd, short event, void *arg)
{
	amqp_connection_state_t *conn = arg;
	amqp_basic_properties_t props;
	char buf[8129];
	int r;

	if (event != EV_READ)
		return;

	r = read(fd, buf, sizeof(buf));
	buf[r] = '\0';

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.content_type = amqp_cstring_bytes("text/plain");
	props.delivery_mode = 2; /* persistent delivery mode */
	amqp_basic_publish(*conn, 1, amqp_cstring_bytes(exchange),
				    amqp_cstring_bytes(routingkey),
				    0,
				    0,
				    &props,
				    amqp_cstring_bytes(buf));
	return;
}

int main()
{
	struct event *eve;
	struct event_base *base;
	int udpsock_fd, amqpsock_fd;
	struct sockaddr_in staddr;
	amqp_connection_state_t conn;

	parse_config();

	base = event_base_new();

	udpsock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	int nreqflags = fcntl(udpsock_fd, F_GETFL, 0);
	fcntl(udpsock_fd, F_SETFL, nreqflags | O_NONBLOCK);
	memset(&staddr, 0, sizeof(struct sockaddr_in));
	staddr.sin_addr.s_addr = INADDR_ANY;
	staddr.sin_port = htons(5140);
	staddr.sin_family = AF_INET;

	conn = amqp_new_connection();
	amqpsock_fd = amqp_open_socket("10.128.2.10", 5672);
	amqp_set_sockfd(conn, amqpsock_fd);
	amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "graylog2", "gaylord");
	amqp_channel_open(conn, 1);
	amqp_get_rpc_reply(conn);

	amqp_exchange_declare(conn, 1, amqp_cstring_bytes(exchange),
				       amqp_cstring_bytes(exchangetype),
				       0,
				       0,
				       amqp_empty_table);

	int noptval = 1;
	setsockopt(udpsock_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&noptval, sizeof(noptval));

	bind(udpsock_fd, (struct sockaddr *)&staddr, sizeof(staddr));
	eve = event_new(base, udpsock_fd, EV_READ | EV_PERSIST, got_msg, &conn);
	event_add(eve, NULL);

	event_base_dispatch(base);

	return 0;
}
