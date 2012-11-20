#include <stdio.h>
#include <yaml.h>

#define CONFIG_FILE "logenqueue-conf.yml"

int parse_config()
{
	yaml_parser_t	parser;
	yaml_event_t	event;
	FILE		*conf = NULL;

	conf = fopen(CONFIG_FILE, "r");
	if (conf == NULL) {
		fprintf(stderr, "Unable to open config file: %s\n", CONFIG_FILE);
		exit(-1);
	}


	if (!yaml_parser_initialize(&parser)) {
		fprintf(stderr, "Unable to initialize YAML parser!\n");
		exit(-1);
	}

	yaml_parser_set_input_file(&parser, conf); 

	memset(&event, 0, sizeof(yaml_event_t));


	do {
		yaml_parser_parse(&parser, &event);

		switch(event.type)
		{ 
			case YAML_NO_EVENT: puts("No event!"); break;
			/* Stream start/end */
			case YAML_STREAM_START_EVENT: puts("STREAM START"); break;
			case YAML_STREAM_END_EVENT:   puts("STREAM END");   break;
			/* Block delimeters */
			case YAML_DOCUMENT_START_EVENT: puts("Start Document"); break;
			case YAML_DOCUMENT_END_EVENT:   puts("End Document");   break;
			case YAML_SEQUENCE_START_EVENT: puts("Start Sequence"); break;
			case YAML_SEQUENCE_END_EVENT:   puts("End Sequence");   break;
			case YAML_MAPPING_START_EVENT:  puts("Start Mapping");  break;
			case YAML_MAPPING_END_EVENT:    puts("End Mapping");    break;
			/* Data */
			case YAML_ALIAS_EVENT:  printf("Got alias (anchor %s)\n", event.data.alias.anchor); break;
			case YAML_SCALAR_EVENT: printf("Got scalar (value %s)\n", event.data.scalar.value); break;
		}
		if(event.type != YAML_STREAM_END_EVENT)
			yaml_event_delete(&event);
	} while(event.type != YAML_STREAM_END_EVENT);
		yaml_event_delete(&event);

	return 0;
}
