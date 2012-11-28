#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <sys/param.h>

#include "logenqueue.h"
#include "config.h"

#ifndef CONFIG_FILE
#define CONFIG_FILE "logenqueue.conf"
#endif

char    config_file[MAXPATHLEN] = CONFIG_FILE;
extern	struct config cfg;

int parse_config()
{
	FILE	*conf = NULL;
	char	line[1024];
	int	linelen;
	char	*key, *val;

	conf = fopen(CONFIG_FILE, "r");
	if (conf == NULL) {
		fprintf(stderr, "Unable to open config file: %s\n", CONFIG_FILE);
		exit(-1);
	}

	while (fgets(line, sizeof(line), conf)) {
		linelen = strlen(line);
		/* chomp */
		if (line[linelen-1] == '\n')
			line[linelen-1] = '\0';
		key = line;
		val = strchr(line, '=');
		if (!val)
			continue;
		*val = '\0';
		val++;
#define CFGCPY(dst)	strncpy(dst, val, sizeof(dst));
#define CFGNCPY(dst)	dst = (int)strtol(val,(char **)NULL,10);
		if (strcasecmp(key, "amqp_host") == 0) {
			CFGCPY(cfg.amqp.host);
		} else if (strcasecmp(key, "amqp_port") == 0) {
			CFGNCPY(cfg.amqp.port);
		} else if (strcasecmp(key, "amqp_user") == 0) {
			CFGCPY(cfg.amqp.user);
		} else if (strcasecmp(key, "amqp_pass") == 0) {
			CFGCPY(cfg.amqp.pass);
		} else if (strcasecmp(key, "amqp_vhost") == 0) {
			CFGCPY(cfg.amqp.vhost);
		} else if (strcasecmp(key, "amqp_exchange_name") == 0) {
			CFGCPY(cfg.amqp.exch_name);
		} else if (strcasecmp(key, "amqp_exchange_type") == 0) {
			CFGCPY(cfg.amqp.exch_type);
		} else if (strcasecmp(key, "syslog_listen") == 0) {
			CFGCPY(cfg.listener.syslog.bind);
		} else if (strcasecmp(key, "syslog_port") == 0) {
			CFGNCPY(cfg.listener.syslog.port);
		} else if (strcasecmp(key, "gelf_listen") == 0) {
			CFGCPY(cfg.listener.gelf.bind);
		} else if (strcasecmp(key, "gelf_port") == 0) {
			CFGNCPY(cfg.listener.gelf.port);
		}
	}

	return 0;
}

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
