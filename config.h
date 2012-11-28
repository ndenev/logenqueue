#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#include <sys/types.h>
#include <sys/param.h>

#define HOST_NAME_MAX	255

int	parse_config();
void	parse_opts(int*, char***);

struct amqp_config {
	char	host[HOST_NAME_MAX];
	u_int	port;
	char	user[255];
	char	pass[255];
	char	vhost[255];
	char	exch_name[255];
	char	exch_type[64];
	int	fd;
};

struct syslog_listener {
	char	bind[HOST_NAME_MAX];
	u_int	port;
	int	fd;
};

struct gelf_listener {
	char	bind[HOST_NAME_MAX];
	u_int	port;
	int	fd;

};

struct config {
	struct amqp_config amqp;
	struct syslog_listener syslog;
	struct gelf_listener gelf;
};

#endif
