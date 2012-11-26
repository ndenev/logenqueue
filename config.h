#include <sys/types.h>
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX	255
#endif

struct amqp_config {
	char	host[HOST_NAME_MAX];
	u_int	port;
	char	user[255];
	char	pass[255];
	char	vhost[255];
	char	exch_name[255];
	char	exch_type[64];
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
