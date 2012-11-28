#include <sys/types.h>
#include <sys/param.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX	255
#endif

int	parse_config();
void	parse_opts(int*, char***);

const char *exchange = "syslog";
const char *exchangetype = "topic";
const char *routingkey = "#";

struct amqp_config {
	char	host[HOST_NAME_MAX];
	u_int	port;
	char	user[255];
	char	pass[255];
	char	vhost[255];
	char	exch_name[255];
	char	exch_type[64];
	char	rt_key[255];
};

struct syslog_listener {
	char	bind[HOST_NAME_MAX];
	u_int	port;
};

struct gelf_listener {
	char	bind[HOST_NAME_MAX];
	u_int	port;

};

struct listener_config {
	struct syslog_listener syslog;
	struct gelf_listener gelf;
};

struct config {
	struct amqp_config amqp;
	struct listener_config listener;
};
