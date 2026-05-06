#ifndef DISPATCH_CLI_H
#define DISPATCH_CLI_H

int dispatch_cli_is_command(const char *command);
int dispatch_cli_dispatch(int argc, char **argv);
void dispatch_cli_print_help(void);

#endif
