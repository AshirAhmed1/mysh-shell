#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "builtins.h"
#include "io_helpers.h"
#include "variables.h"
#include "commands.h"

#define PENDING_CAP 4096

static char *expand_token(const char *token, VarStore *store) {
    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        display_error("ERROR: ", "memory allocation failed");
        return NULL;
    }
    buf[0] = '\0';

    size_t i = 0;
    while (token[i] != '\0') {
        if (token[i] == '$') {
            i++;
            char name[MAX_STR_LEN + 1];
            size_t n = 0;

            while (token[i] != '\0' &&
                   token[i] != '$' &&
                   token[i] != ' ' &&
                   token[i] != '\t' &&
                   token[i] != '\n' &&
                   n < MAX_STR_LEN) {
                name[n++] = token[i++];
            }
            name[n] = '\0';

            const char *val = store_get(store, name);
            if (val != NULL) {
                for (size_t k = 0; val[k] != '\0'; k++) {
                    if (len < MAX_STR_LEN) {
                        if (len + 1 >= cap) {
                            cap *= 2;
                            char *tmp = realloc(buf, cap);
                            if (!tmp) {
                                free(buf);
                                display_error("ERROR: ", "memory allocation failed");
                                return NULL;
                            }
                            buf = tmp;
                        }
                        buf[len++] = val[k];
                        buf[len] = '\0';
                    }
                }
            }
        } else {
            if (len < MAX_STR_LEN) {
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *tmp = realloc(buf, cap);
                    if (!tmp) {
                        free(buf);
                        display_error("ERROR: ", "memory allocation failed");
                        return NULL;
                    }
                    buf = tmp;
                }
                buf[len++] = token[i];
                buf[len] = '\0';
            }
            i++;
        }
    }

    return buf;
}

static char *space_operators(const char *line) {
    size_t len = strlen(line);
    size_t cap = (len * 3) + 1;
    char *out = malloc(cap);
    if (out == NULL) {
        display_error("ERROR: ", "memory allocation failed");
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '|' || line[i] == '&') {
            out[j++] = ' ';
            out[j++] = line[i];
            out[j++] = ' ';
        } else {
            out[j++] = line[i];
        }
    }
    out[j] = '\0';
    return out;
}

static int try_assignment(char *line, VarStore *store) {
    if (strchr(line, '|') != NULL || strchr(line, '&') != NULL) {
        return 0;
    }

    char *eq = strchr(line, '=');
    if (eq == NULL) return 0;

    for (char *p = line; p < eq; p++) {
        if (*p == ' ' || *p == '\t') {
            return 0;
        }
    }

    *eq = '\0';
    char *name = line;
    char *value = eq + 1;

    char *expanded = expand_token(value, store);
    if (expanded == NULL) return -1;

    if (store_set(store, name, expanded) < 0) {
        free(expanded);
        return -1;
    }

    free(expanded);
    return 1;
}

static int execute_line(char *line, VarStore *store) {
    if (line[0] == '\0') {
        return 0;
    }

    int assign_result = try_assignment(line, store);
    if (assign_result == 1) {
        return 0;
    }
    if (assign_result == -1) {
        return 0;
    }

    char *spaced = space_operators(line);
    if (spaced == NULL) {
        return 0;
    }

    char *raw_tokens[MAX_STR_LEN] = {NULL};
    size_t raw_count = tokenize_input(spaced, raw_tokens);
    if (raw_count == 0) {
        free(spaced);
        return 0;
    }

    char *expanded_tokens[MAX_STR_LEN] = {NULL};
    for (size_t i = 0; i < raw_count; i++) {
        expanded_tokens[i] = expand_token(raw_tokens[i], store);
        if (expanded_tokens[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                free(expanded_tokens[j]);
            }
            free(spaced);
            return 0;
        }
    }

    int handled = 0;
    int net_rc = try_network_command(expanded_tokens, raw_count, store, &handled);
    if (handled) {
        for (size_t i = 0; i < raw_count; i++) {
            free(expanded_tokens[i]);
        }
        free(spaced);
        (void)net_rc;
        return 0;
    }

    Job job;
    if (parse_job_from_tokens(expanded_tokens, raw_count, &job) < 0) {
        for (size_t i = 0; i < raw_count; i++) {
            free(expanded_tokens[i]);
        }
        free(spaced);
        return 0;
    }

    for (size_t i = 0; i < raw_count; i++) {
        free(expanded_tokens[i]);
    }
    free(spaced);

    if (job.num_cmds == 1 &&
        job.cmds[0].argc == 1 &&
        strcmp(job.cmds[0].argv[0], "exit") == 0 &&
        !job.background) {
        free_job(&job);
        return 1;
    }

    run_job(&job);
    free_job(&job);
    return 0;
}

int main(void) {
    VarStore store;
    init_store(&store);
    init_shell_signals();

    char pending[PENDING_CAP];
    size_t pending_len = 0;
    int saw_eof = 0;

    while (1) {
        if (saw_eof && pending_len == 0) {
            break;
        }

        reap_background_jobs();
        clear_shell_sigint_flag();
        set_shell_signal_mode(SIGMODE_IDLE);
        display_message("mysh$ ");

        while (memchr(pending, '\n', pending_len) == NULL && !saw_eof) {
            char chunk[MAX_STR_LEN + 1];
            ssize_t n = get_input(chunk);

            if (n == 0) {
                saw_eof = 1;
                break;
            }

            if (n < 0) {
                if (errno == EINTR && shell_sigint_flag_is_set()) {
                    clear_shell_sigint_flag();
                    pending_len = 0;
                    break;
                }

                saw_eof = 1;
                pending_len = 0;
                break;
            }

            if (pending_len + (size_t)n > PENDING_CAP) {
                display_error("ERROR: ", "internal input buffer overflow");
                pending_len = 0;
                break;
            }

            memcpy(pending + pending_len, chunk, (size_t)n);
            pending_len += (size_t)n;
        }

        set_shell_signal_mode(SIGMODE_NONE);

        if (pending_len == 0) {
            continue;
        }

        char line[MAX_STR_LEN + 1];
        char *newline_ptr = memchr(pending, '\n', pending_len);

        if (newline_ptr != NULL) {
            size_t line_len = (size_t)(newline_ptr - pending);

            if (line_len > MAX_STR_LEN) {
                display_error("ERROR: ", "input line too long");

                size_t drop = line_len + 1;
                memmove(pending, pending + drop, pending_len - drop);
                pending_len -= drop;
                continue;
            }

            memcpy(line, pending, line_len);
            line[line_len] = '\0';

            size_t drop = line_len + 1;
            memmove(pending, pending + drop, pending_len - drop);
            pending_len -= drop;
        } else {
            if (!saw_eof) {
                continue;
            }

            if (pending_len > MAX_STR_LEN) {
                display_error("ERROR: ", "input line too long");
                pending_len = 0;
                continue;
            }

            memcpy(line, pending, pending_len);
            line[pending_len] = '\0';
            pending_len = 0;
        }

        if (execute_line(line, &store)) {
            break;
        }
    }

    cleanup_all_children();
    store_destroy(&store);
    return 0;
}
