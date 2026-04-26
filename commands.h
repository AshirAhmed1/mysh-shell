#ifndef COMMANDS_H
#define COMMANDS_H

#include <sys/types.h>
#include <stddef.h>
#include "variables.h"

typedef struct {
    char **argv;
    int argc;
} Command;

typedef struct {
    Command *cmds;
    int num_cmds;
    int background;
} Job;

#define SIGMODE_NONE 0
#define SIGMODE_IDLE 1
#define SIGMODE_BUILTIN 2
#define SIGMODE_WAITFG 3

ssize_t bn_cd(char **tokens);
ssize_t bn_ls(char **tokens);
ssize_t bn_cat(char **tokens);
ssize_t bn_wc(char **tokens);
ssize_t bn_ps(char **tokens);
ssize_t bn_kill(char **tokens);

int parse_job_from_tokens(char **tokens, size_t count, Job *job);
void free_job(Job *job);
ssize_t run_job(Job *job);

void reap_background_jobs(void);
void cleanup_all_children(void);

void init_shell_signals(void);
void set_shell_signal_mode(int mode);
void clear_shell_sigint_flag(void);
int shell_sigint_flag_is_set(void);

int try_network_command(char **tokens, size_t count, VarStore *store, int *handled);

#endif
