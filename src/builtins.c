#include <string.h>

#include "builtins.h"
#include "io_helpers.h"
#include "commands.h"


// ====== Command execution =====

/* Return: index of builtin or -1 if cmd doesn't match a builtin
 */
bn_ptr check_builtin(const char *cmd) {
    ssize_t cmd_num = 0;
    while (cmd_num < BUILTINS_COUNT &&
           strncmp(BUILTINS[cmd_num], cmd, MAX_STR_LEN) != 0) {
        cmd_num += 1;
    }
    return BUILTINS_FN[cmd_num];
}


// ===== Builtins =====

/* Prereq: tokens is a NULL terminated sequence of strings.
 * Return 0 on success and -1 on error ... but there are no errors on echo. 
 */
ssize_t bn_echo(char **tokens) {
    int printed_any = 0;

    for (ssize_t i = 1; tokens[i] != NULL; i++) {
        if (tokens[i][0] == '\0') {
            continue;
        }

        if (printed_any) {
            display_message(" ");
        }
        display_message(tokens[i]);
        printed_any = 1;
    }

    display_message("\n");
    return 0;
}

