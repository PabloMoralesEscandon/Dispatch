#include <string.h>

#include "parser.h"
#include "memory.h"

int main (int argc, char **argv) {
    load(FILE_NAME);
    update_recurrent(FILE_NAME);
    if(argv[1]==NULL){
	print_help();
	return 0;
    }
    char *command = argv[1];
    char *options[NUMBER_OPT] = {0};
    char *words = parse_words(argc, argv);
    int id = -1;
    if(words!= NULL){
	id = parse_id_name(words);
	if(id==-1){ 
	    // Si el argumento libre no es numerico, se interpreta como nombre.
	    if(strcmp(command, "list")) options[NAME] = strdup(words);
	    else if(!strcmp(words, "projects")) command = strdup("list_projects");
	}
    }
    parse_options(argc, argv, options);
    return dispatch_command(command, options, id);
}
