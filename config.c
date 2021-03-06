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

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <sys/param.h>

#include "common.h"
#include "config.h"

#ifndef CONFIG_FILE
#define CONFIG_FILE "logenqueue.conf"
#endif

#define MAXVERBOSE 3

char    config_file[MAXPATHLEN] = CONFIG_FILE;
struct config cfg;

int     dontfork = 0;
int     verbose = 0;
int	stats = 0;

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
#define CFGCPY(dst)	strncpy(dst, val, sizeof(dst)-1);
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
			CFGCPY(cfg.amqp.ex_name);
		} else if (strcasecmp(key, "amqp_exchange_type") == 0) {
			CFGCPY(cfg.amqp.ex_type);
		} else if (strcasecmp(key, "syslog_listen") == 0) {
			CFGCPY(cfg.syslog.bind);
		} else if (strcasecmp(key, "syslog_port") == 0) {
			CFGNCPY(cfg.syslog.port);
		} else if (strcasecmp(key, "syslog_workers") == 0) {
			CFGNCPY(cfg.syslog.workers);
		} else if (strcasecmp(key, "gelf_listen") == 0) {
			CFGCPY(cfg.gelf.bind);
		} else if (strcasecmp(key, "gelf_port") == 0) {
			CFGNCPY(cfg.gelf.port);
		} else if (strcasecmp(key, "gelf_workers") == 0) {
			CFGNCPY(cfg.gelf.workers);
		}
	}

	return 0;
}

void
usage()
{
	printf("logenqueue [-dv] [-c /path/to/config.file]\n");
	printf("	-c 	--conf");
	printf("		specify config file to use\n");
	printf("	-d	--dontfork");
	printf("	stay in foreground\n");
	printf("	-h	--help");
	printf("		this help screen\n");
	printf("	-s	--stats");	
	printf("		show stats in foreground  mode\n");
	printf("	-v	--verbose");
	printf("	verbose mode, can be specified multiple times\n");
	exit(0);
}

int
parse_opts(int *argc, char ***argv)
{
	int opt;

	static struct option longopts[] = {
		{ "conf",	required_argument,	NULL,	'c' },
		{ "dontfork",	no_argument,		NULL,	'd' },
		{ "help",	no_argument,		NULL,	'h' },
		{ "stats",	no_argument,		NULL,	's' },
		{ "verbose",	no_argument,		NULL,	'v' },
		{ NULL,		0,			NULL,	0 },
	};

        while ((opt = getopt_long(*argc, *argv,
                                "c:dhsv", longopts, NULL)) != -1) {
                switch (opt) {
                        case 'c':
				strncpy(config_file, optarg, MAXPATHLEN);
                                break;
                        case 'd':
                                dontfork = 1;
                                break;
			case 's':
				stats = 1;
				break;
                        case 'v':
                                verbose++;
                                break;
			default:
				usage();
				/* does not return */
				break;
		}
	}
	*argc -= optind;
	*argv += optind;
	if (*argc > 0) {
		printf("too many arguments\n");
		return(-1);
	}
	return(0);
}
