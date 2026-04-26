#include <unistd.h>
#include <string.h>
#include <pwd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "commands.h"
#include "io_helpers.h"
#include "builtins.h"

static volatile sig_atomic_t shell_sigint_flag = 0;
static volatile sig_atomic_t shell_signal_mode = SIGMODE_NONE;
static pid_t fg_pids[256];
static volatile sig_atomic_t fg_pid_count = 0;
static pid_t server_pid = -1;

static void shell_sigint_handler(int signo) {
    (void)signo;
    shell_sigint_flag = 1;
    write(STDOUT_FILENO, "\n", 1);

    if (shell_signal_mode == SIGMODE_WAITFG) {
        sig_atomic_t count = fg_pid_count;
        for (sig_atomic_t i = 0; i < count; i++) {
            if (fg_pids[i] > 0) {
                kill(fg_pids[i], SIGINT);
            }
        }
    }
}

void init_shell_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shell_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

void set_shell_signal_mode(int mode) {
    shell_signal_mode = mode;
}

void clear_shell_sigint_flag(void) {
    shell_sigint_flag = 0;
}

int shell_sigint_flag_is_set(void) {
    return shell_sigint_flag ? 1 : 0;
}

static void set_foreground_pids(pid_t *pids, int count) {
    if (count > 256) {
        count = 256;
    }

    for (int i = 0; i < count; i++) {
        fg_pids[i] = pids[i];
    }

    fg_pid_count = count;
}

static void clear_foreground_pids(void) {
    for (int i = 0; i < 256; i++) {
        fg_pids[i] = 0;
    }

    fg_pid_count = 0;
}

static int token_count(char **tokens) {
    int count = 0;

    while (tokens[count] != NULL) {
        count++;
    }

    return count;
}

static int append_component(char *out, size_t out_sz, size_t *o, const char *s) {
    size_t L = strlen(s);

    if (*o > 0 && out[*o - 1] != '/') {
        if (*o + 1 >= out_sz) {
            return -1;
        }
        out[(*o)++] = '/';
    }

    if (*o + L >= out_sz) {
        return -1;
    }

    memcpy(out + *o, s, L);
    *o += L;
    return 0;
}

static int expand_dot_components(const char *in, char *out, size_t out_sz) {
    size_t o = 0;
    size_t i = 0;

    if (in[0] == '/') {
        if (o + 1 >= out_sz) {
            return -1;
        }
        out[o++] = '/';
        i = 1;
    }

    while (in[i] != '\0') {
        while (in[i] == '/') {
            i++;
        }

        size_t start = i;
        while (in[i] != '\0' && in[i] != '/') {
            i++;
        }

        size_t len = i - start;
        if (len == 0) {
            break;
        }

        char comp[256];
        if (len >= sizeof(comp)) {
            return -1;
        }

        memcpy(comp, in + start, len);
        comp[len] = '\0';

        int all_dots = 1;
        for (size_t k = 0; k < len; k++) {
            if (comp[k] != '.') {
                all_dots = 0;
                break;
            }
        }

        if (all_dots && len >= 2) {
            for (size_t rep = 0; rep < len - 1; rep++) {
                if (append_component(out, out_sz, &o, "..") < 0) {
                    return -1;
                }
            }
        } else {
            if (append_component(out, out_sz, &o, comp) < 0) {
                return -1;
            }
        }
    }

    if (o >= out_sz) {
        return -1;
    }

    out[o] = '\0';

    if (o == 0) {
        if (out_sz < 2) {
            return -1;
        }
        out[0] = '.';
        out[1] = '\0';
    }

    return 0;
}

static char *xstrdup_local(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, s, len + 1);
    return copy;
}

typedef struct ProcNode {
    pid_t pid;
    char *name;
    int active;
    int job_id;
    struct ProcNode *next;
} ProcNode;

typedef struct JobNode {
    int job_id;
    char *cmdline;
    int remaining;
    int active;
    struct JobNode *next;
} JobNode;

static ProcNode *proc_head = NULL;
static JobNode *job_head = NULL;

static ProcNode *find_proc(pid_t pid) {
    ProcNode *curr = proc_head;

    while (curr != NULL) {
        if (curr->pid == pid) {
            return curr;
        }
        curr = curr->next;
    }

    return NULL;
}

static JobNode *find_job(int job_id) {
    JobNode *curr = job_head;

    while (curr != NULL) {
        if (curr->job_id == job_id) {
            return curr;
        }
        curr = curr->next;
    }

    return NULL;
}

static void add_proc(pid_t pid, const char *name) {
    ProcNode *node = malloc(sizeof(ProcNode));

    if (node == NULL) {
        display_error("ERROR: ", "memory allocation failed");
        return;
    }

    node->name = xstrdup_local(name);
    if (node->name == NULL) {
        free(node);
        display_error("ERROR: ", "memory allocation failed");
        return;
    }

    node->pid = pid;
    node->active = 1;
    node->job_id = 0;
    node->next = proc_head;
    proc_head = node;
}

static void remove_proc(pid_t pid) {
    ProcNode *prev = NULL;
    ProcNode *curr = proc_head;

    while (curr != NULL) {
        if (curr->pid == pid) {
            if (prev == NULL) {
                proc_head = curr->next;
            } else {
                prev->next = curr->next;
            }

            free(curr->name);
            free(curr);
            return;
        }

        prev = curr;
        curr = curr->next;
    }
}

static int highest_active_job_id(void) {
    int max_id = 0;
    JobNode *curr = job_head;

    while (curr != NULL) {
        if (curr->active && curr->job_id > max_id) {
            max_id = curr->job_id;
        }
        curr = curr->next;
    }

    return max_id;
}

static int next_job_id(void) {
    return highest_active_job_id() + 1;
}

static char *build_job_string(Job *job) {
    size_t total = 1;

    for (int i = 0; i < job->num_cmds; i++) {
        for (int j = 0; j < job->cmds[i].argc; j++) {
            total += strlen(job->cmds[i].argv[j]) + 1;
        }
        if (i < job->num_cmds - 1) {
            total += 3;
        }
    }

    char *buf = malloc(total);
    if (buf == NULL) {
        return NULL;
    }

    buf[0] = '\0';

    for (int i = 0; i < job->num_cmds; i++) {
        for (int j = 0; j < job->cmds[i].argc; j++) {
            strcat(buf, job->cmds[i].argv[j]);
            if (j < job->cmds[i].argc - 1) {
                strcat(buf, " ");
            }
        }
        if (i < job->num_cmds - 1) {
            strcat(buf, " | ");
        }
    }

    return buf;
}

static void add_background_job(int job_id, const char *cmdline, int remaining, pid_t *pids, int count) {
    JobNode *job = malloc(sizeof(JobNode));

    if (job == NULL) {
        display_error("ERROR: ", "memory allocation failed");
        return;
    }

    job->cmdline = xstrdup_local(cmdline);
    if (job->cmdline == NULL) {
        free(job);
        display_error("ERROR: ", "memory allocation failed");
        return;
    }

    job->job_id = job_id;
    job->remaining = remaining;
    job->active = 1;
    job->next = job_head;
    job_head = job;

    for (int i = 0; i < count; i++) {
        ProcNode *p = find_proc(pids[i]);
        if (p != NULL) {
            p->job_id = job_id;
        }
    }
}

static void remove_job(int job_id) {
    JobNode *prev = NULL;
    JobNode *curr = job_head;

    while (curr != NULL) {
        if (curr->job_id == job_id) {
            if (prev == NULL) {
                job_head = curr->next;
            } else {
                prev->next = curr->next;
            }

            free(curr->cmdline);
            free(curr);
            return;
        }

        prev = curr;
        curr = curr->next;
    }
}

void reap_background_jobs(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == server_pid) {
            server_pid = -1;
            continue;
        }

        ProcNode *proc = find_proc(pid);
        int job_id = 0;

        if (proc != NULL) {
            job_id = proc->job_id;
        }

        if (job_id != 0) {
            JobNode *job = find_job(job_id);
            if (job != NULL && job->active) {
                job->remaining--;
                if (job->remaining <= 0) {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "[%d]+ Done %s\n", job->job_id, job->cmdline);
                    display_message(msg);
                    job->active = 0;
                    remove_job(job->job_id);
                }
            }
        }

        remove_proc(pid);
    }
}

/* ---------------- M5 networking helpers ---------------- */

enum ClientMode {
    CLIENT_MODE_UNKNOWN = 0,
    CLIENT_MODE_SEND = 1,
    CLIENT_MODE_INTERACTIVE = 2
};

typedef struct ServerClient {
    int fd;
    int id;
    int mode;
    char buf[4096];
    int inbuf;
    struct ServerClient *next;
} ServerClient;

static int write_all_fd(int fd, const char *buf, size_t len) {
    size_t written = 0;

    while (written < len) {
        ssize_t rc = write(fd, buf + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)rc;
    }

    return 0;
}

static int send_line_fd(int fd, const char *line) {
    size_t len = strlen(line);
    return write_all_fd(fd, line, len);
}

static int buffer_get_line(char *buf, int *inbuf, char **out_line) {
    for (int i = 0; i < *inbuf; i++) {
        if (buf[i] == '\n') {
            int line_len = i;
            if (line_len > 0 && buf[line_len - 1] == '\r') {
                line_len--;
            }

            char *line = malloc((size_t)line_len + 1);
            if (line == NULL) {
                return -1;
            }

            memcpy(line, buf, (size_t)line_len);
            line[line_len] = '\0';

            int remaining = *inbuf - (i + 1);
            memmove(buf, buf + i + 1, (size_t)remaining);
            *inbuf = remaining;
            *out_line = line;
            return 1;
        }
    }

    return 0;
}

static void remove_server_client(ServerClient **head, ServerClient *target) {
    ServerClient *prev = NULL;
    ServerClient *curr = *head;

    while (curr != NULL) {
        if (curr == target) {
            if (prev == NULL) {
                *head = curr->next;
            } else {
                prev->next = curr->next;
            }

            close(curr->fd);
            free(curr);
            return;
        }

        prev = curr;
        curr = curr->next;
    }
}

static void broadcast_interactive(ServerClient *clients, const char *line) {
    ServerClient *curr = clients;

    while (curr != NULL) {
        if (curr->mode == CLIENT_MODE_INTERACTIVE) {
            if (send_line_fd(curr->fd, line) < 0) {
                /* ignore write failure; cleanup happens on next read */
            }
        }
        curr = curr->next;
    }
}

static int setup_listen_socket(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }

    int on = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on)) < 0) {
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 16) < 0) {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

static void server_child_loop(int port, int status_fd) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_DFL);

    int listen_fd = setup_listen_socket(port);
    if (listen_fd < 0) {
        if (status_fd >= 0) {
            char ch = '0';
            write_all_fd(status_fd, &ch, 1);
            close(status_fd);
        }
        _exit(1);
    }

    if (status_fd >= 0) {
        char ch = '1';
        write_all_fd(status_fd, &ch, 1);
        close(status_fd);
    }

    ServerClient *clients = NULL;
    int next_client_id = 1;
    int connected_count = 0;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;

        for (ServerClient *c = clients; c != NULL; c = c->next) {
            FD_SET(c->fd, &readfds);
            if (c->fd > max_fd) {
                max_fd = c->fd;
            }
        }

        int rc = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);

            if (client_fd >= 0) {
                ServerClient *node = malloc(sizeof(ServerClient));
                if (node != NULL) {
                    node->fd = client_fd;
                    node->id = next_client_id++;
                    node->mode = CLIENT_MODE_UNKNOWN;
                    node->inbuf = 0;
                    node->next = clients;
                    clients = node;
                    connected_count++;
                } else {
                    close(client_fd);
                }
            }
        }

        ServerClient *curr = clients;
        while (curr != NULL) {
            ServerClient *next = curr->next;

            if (!FD_ISSET(curr->fd, &readfds)) {
                curr = next;
                continue;
            }

            ssize_t nr = read(
                curr->fd,
                curr->buf + curr->inbuf,
                sizeof(curr->buf) - 1U - (size_t)curr->inbuf
            );

            if (nr <= 0) {
                connected_count--;
                remove_server_client(&clients, curr);
                curr = next;
                continue;
            }

            curr->inbuf += (int)nr;
            curr->buf[curr->inbuf] = '\0';

            if (curr->inbuf >= (int)sizeof(curr->buf) - 1) {
                connected_count--;
                remove_server_client(&clients, curr);
                curr = next;
                continue;
            }

            char *line = NULL;
            int got_line;

            while ((got_line = buffer_get_line(curr->buf, &curr->inbuf, &line)) == 1) {
                if (curr->mode == CLIENT_MODE_UNKNOWN) {
                    if (strcmp(line, "__MYSH_INTERACTIVE__") == 0) {
                        curr->mode = CLIENT_MODE_INTERACTIVE;
                        char idbuf[64];
                        snprintf(idbuf, sizeof(idbuf), "ID %d\n", curr->id);

                        if (send_line_fd(curr->fd, idbuf) < 0) {
                            free(line);
                            connected_count--;
                            remove_server_client(&clients, curr);
                            break;
                        }
                    } else {
                        curr->mode = CLIENT_MODE_SEND;
                        char msgbuf[512];
                        snprintf(msgbuf, sizeof(msgbuf), "%s\n", line);
                        display_message(msgbuf);
                        broadcast_interactive(clients, msgbuf);
                        free(line);
                        connected_count--;
                        remove_server_client(&clients, curr);
                        line = NULL;
                        break;
                    }
                } else if (curr->mode == CLIENT_MODE_INTERACTIVE) {
                    if (strcmp(line, "\\connected") == 0) {
                        char countbuf[64];
                        snprintf(countbuf, sizeof(countbuf), "%d\n", connected_count);

                        if (send_line_fd(curr->fd, countbuf) < 0) {
                            free(line);
                            connected_count--;
                            remove_server_client(&clients, curr);
                            break;
                        }
                    } else {
                        char msgbuf[512];
                        snprintf(msgbuf, sizeof(msgbuf), "client%d: %s\n", curr->id, line);
                        display_message(msgbuf);
                        broadcast_interactive(clients, msgbuf);
                    }
                }

                free(line);
                line = NULL;
            }

            if (got_line < 0) {
                connected_count--;
                remove_server_client(&clients, curr);
            }

            curr = next;
        }
    }

    while (clients != NULL) {
        ServerClient *next = clients->next;
        close(clients->fd);
        free(clients);
        clients = next;
    }

    close(listen_fd);
    _exit(0);
}

static int parse_port_number(const char *token, int *port_out) {
    char *endptr = NULL;
    long val = strtol(token, &endptr, 10);

    if (token[0] == '\0' || *endptr != '\0' || val <= 0 || val > 65535) {
        return -1;
    }

    *port_out = (int)val;
    return 0;
}

static int connect_to_host(const char *hostname, int port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    char port_str[32];
    int sock_fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(hostname, port_str, &hints, &result) != 0) {
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) {
            continue;
        }

        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(sock_fd);
        sock_fd = -1;
    }

    freeaddrinfo(result);
    return sock_fd;
}

static char *join_message_tokens(char **tokens, int start_index) {
    size_t total = 1;

    for (int i = start_index; tokens[i] != NULL; i++) {
        total += strlen(tokens[i]) + 1;
    }

    char *msg = malloc(total);
    if (msg == NULL) {
        return NULL;
    }

    msg[0] = '\0';

    for (int i = start_index; tokens[i] != NULL; i++) {
        strcat(msg, tokens[i]);
        if (tokens[i + 1] != NULL) {
            strcat(msg, " ");
        }
    }

    return msg;
}

static char *expand_client_line(const char *line, VarStore *store) {
    char *copy = xstrdup_local(line);
    if (copy == NULL) {
        return NULL;
    }

    char *tokens[256];
    size_t count = tokenize_input(copy, tokens);
    size_t total = 1;

    for (size_t i = 0; i < count; i++) {
        size_t cap = 64;
        size_t len = 0;
        char *expanded = malloc(cap);
        if (expanded == NULL) {
            free(copy);
            return NULL;
        }

        expanded[0] = '\0';

        size_t p = 0;
        while (tokens[i][p] != '\0') {
            if (tokens[i][p] == '$') {
                p++;
                char name[129];
                size_t n = 0;

                while (tokens[i][p] != '\0' &&
                       tokens[i][p] != '$' &&
                       tokens[i][p] != ' ' &&
                       tokens[i][p] != '\t' &&
                       tokens[i][p] != '\n' &&
                       n < 128) {
                    name[n++] = tokens[i][p++];
                }

                name[n] = '\0';
                const char *val = store_get(store, name);

                if (val != NULL) {
                    for (size_t k = 0; val[k] != '\0'; k++) {
                        if (len + 1 >= cap) {
                            cap *= 2;
                            char *tmp = realloc(expanded, cap);
                            if (tmp == NULL) {
                                free(expanded);
                                free(copy);
                                return NULL;
                            }
                            expanded = tmp;
                        }

                        expanded[len++] = val[k];
                    }
                    expanded[len] = '\0';
                }
            } else {
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *tmp = realloc(expanded, cap);
                    if (tmp == NULL) {
                        free(expanded);
                        free(copy);
                        return NULL;
                    }
                    expanded = tmp;
                }

                expanded[len++] = tokens[i][p++];
                expanded[len] = '\0';
            }
        }

        total += strlen(expanded) + 1;
        free(expanded);
    }

    char *result = malloc(total);
    if (result == NULL) {
        free(copy);
        return NULL;
    }

    result[0] = '\0';

    for (size_t i = 0; i < count; i++) {
        size_t cap = 64;
        size_t len = 0;
        char *expanded = malloc(cap);
        if (expanded == NULL) {
            free(result);
            free(copy);
            return NULL;
        }

        expanded[0] = '\0';

        size_t p = 0;
        while (tokens[i][p] != '\0') {
            if (tokens[i][p] == '$') {
                p++;
                char name[129];
                size_t n = 0;

                while (tokens[i][p] != '\0' &&
                       tokens[i][p] != '$' &&
                       tokens[i][p] != ' ' &&
                       tokens[i][p] != '\t' &&
                       tokens[i][p] != '\n' &&
                       n < 128) {
                    name[n++] = tokens[i][p++];
                }

                name[n] = '\0';
                const char *val = store_get(store, name);

                if (val != NULL) {
                    for (size_t k = 0; val[k] != '\0'; k++) {
                        if (len + 1 >= cap) {
                            cap *= 2;
                            char *tmp = realloc(expanded, cap);
                            if (tmp == NULL) {
                                free(expanded);
                                free(result);
                                free(copy);
                                return NULL;
                            }
                            expanded = tmp;
                        }

                        expanded[len++] = val[k];
                    }
                    expanded[len] = '\0';
                }
            } else {
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *tmp = realloc(expanded, cap);
                    if (tmp == NULL) {
                        free(expanded);
                        free(result);
                        free(copy);
                        return NULL;
                    }
                    expanded = tmp;
                }

                expanded[len++] = tokens[i][p++];
                expanded[len] = '\0';
            }
        }

        strcat(result, expanded);
        if (i + 1 < count) {
            strcat(result, " ");
        }

        free(expanded);
    }

    free(copy);
    return result;
}

static int start_server_command(char **tokens) {
    if (tokens[1] == NULL) {
        display_error("ERROR: ", "No port provided");
        return -1;
    }

    if (server_pid > 0) {
        display_error("ERROR: Builtin failed: ", "start-server");
        return -1;
    }

    int port;
    if (parse_port_number(tokens[1], &port) < 0) {
        display_error("ERROR: Builtin failed: ", "start-server");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        display_error("ERROR: Builtin failed: ", "start-server");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        display_error("ERROR: Builtin failed: ", "start-server");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        proc_head = NULL;
        job_head = NULL;
        clear_foreground_pids();
        set_shell_signal_mode(SIGMODE_NONE);
        clear_shell_sigint_flag();
        server_child_loop(port, pipefd[1]);
    }

    close(pipefd[1]);

    char status = '0';
    ssize_t nr = read(pipefd[0], &status, 1);
    close(pipefd[0]);

    if (nr != 1 || status != '1') {
        waitpid(pid, NULL, 0);
        display_error("ERROR: Builtin failed: ", "start-server");
        return -1;
    }

    server_pid = pid;
    return 0;
}

static int close_server_command(void) {
    if (server_pid > 0) {
        kill(server_pid, SIGTERM);
        waitpid(server_pid, NULL, 0);
        server_pid = -1;
    }

    return 0;
}

static int send_command(char **tokens) {
    if (tokens[1] == NULL) {
        display_error("ERROR: ", "No port provided");
        return -1;
    }

    if (tokens[2] == NULL) {
        display_error("ERROR: ", "No hostname provided");
        return -1;
    }

    int port;
    if (parse_port_number(tokens[1], &port) < 0) {
        display_error("ERROR: Builtin failed: ", "send");
        return -1;
    }

    char *message = join_message_tokens(tokens, 3);
    if (message == NULL) {
        display_error("ERROR: ", "memory allocation failed");
        return -1;
    }

    if (strlen(message) >= MAX_STR_LEN) {
        free(message);
        display_error("ERROR: Builtin failed: ", "send");
        return -1;
    }

    int sock_fd = connect_to_host(tokens[2], port);
    if (sock_fd < 0) {
        free(message);
        display_error("ERROR: Builtin failed: ", "send");
        return -1;
    }

    char line[512];
    snprintf(line, sizeof(line), "%s\n", message);
    free(message);

    if (send_line_fd(sock_fd, line) < 0) {
        close(sock_fd);
        display_error("ERROR: Builtin failed: ", "send");
        return -1;
    }

    close(sock_fd);
    return 0;
}

static int start_client_session_child(int port, const char *hostname, VarStore *store) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_DFL);

    int sock_fd = connect_to_host(hostname, port);
    if (sock_fd < 0) {
        display_error("ERROR: Builtin failed: ", "start-client");
        return 1;
    }

    if (send_line_fd(sock_fd, "__MYSH_INTERACTIVE__\n") < 0) {
        close(sock_fd);
        display_error("ERROR: Builtin failed: ", "start-client");
        return 1;
    }

    char idbuf[128];
    int idlen = 0;

    while (idlen < (int)sizeof(idbuf) - 1) {
        ssize_t nr = read(sock_fd, idbuf + idlen, sizeof(idbuf) - 1U - (size_t)idlen);
        if (nr <= 0) {
            close(sock_fd);
            display_error("ERROR: Builtin failed: ", "start-client");
            return 1;
        }

        idlen += (int)nr;
        idbuf[idlen] = '\0';

        if (strchr(idbuf, '\n') != NULL) {
            break;
        }
    }

    char stdin_buf[1024];
    char socket_buf[4096];
    int socket_inbuf = 0;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock_fd, &readfds);

        int max_fd = (sock_fd > STDIN_FILENO) ? sock_fd : STDIN_FILENO;

        int rc = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (FD_ISSET(sock_fd, &readfds)) {
            ssize_t nr = read(
                sock_fd,
                socket_buf + socket_inbuf,
                sizeof(socket_buf) - 1U - (size_t)socket_inbuf
            );

            if (nr <= 0) {
                break;
            }

            socket_inbuf += (int)nr;
            socket_buf[socket_inbuf] = '\0';

            if (write_all_fd(STDOUT_FILENO, socket_buf, (size_t)socket_inbuf) < 0) {
                break;
            }

            socket_inbuf = 0;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t nr = read(STDIN_FILENO, stdin_buf, sizeof(stdin_buf) - 1);
            if (nr <= 0) {
                break;
            }

            stdin_buf[nr] = '\0';

            char *newline = strchr(stdin_buf, '\n');
            if (newline != NULL) {
                *newline = '\0';
            }

            newline = strchr(stdin_buf, '\r');
            if (newline != NULL) {
                *newline = '\0';
            }

            if (stdin_buf[0] == '\0') {
                break;
            }

            char *expanded = NULL;
            if (strcmp(stdin_buf, "\\connected") == 0) {
                expanded = xstrdup_local("\\connected");
            } else {
                expanded = expand_client_line(stdin_buf, store);
            }

            if (expanded == NULL) {
                display_error("ERROR: ", "memory allocation failed");
                continue;
            }

            if (strlen(expanded) >= MAX_STR_LEN) {
                display_error("ERROR: Builtin failed: ", "start-client");
                free(expanded);
                continue;
            }

            char line[512];
            snprintf(line, sizeof(line), "%s\n", expanded);
            free(expanded);

            if (send_line_fd(sock_fd, line) < 0) {
                break;
            }
        }
    }

    close(sock_fd);
    return 0;
}

static int start_client_command(char **tokens, VarStore *store) {
    if (tokens[1] == NULL) {
        display_error("ERROR: ", "No port provided");
        return -1;
    }

    if (tokens[2] == NULL) {
        display_error("ERROR: ", "No hostname provided");
        return -1;
    }

    int port;
    if (parse_port_number(tokens[1], &port) < 0) {
        display_error("ERROR: Builtin failed: ", "start-client");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        display_error("ERROR: Builtin failed: ", "start-client");
        return -1;
    }

    if (pid == 0) {
        proc_head = NULL;
        job_head = NULL;
        clear_foreground_pids();
        set_shell_signal_mode(SIGMODE_NONE);
        clear_shell_sigint_flag();
        _exit(start_client_session_child(port, tokens[2], store));
    }

    clear_shell_sigint_flag();
    set_foreground_pids(&pid, 1);
    set_shell_signal_mode(SIGMODE_WAITFG);

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }

    clear_foreground_pids();
    set_shell_signal_mode(SIGMODE_NONE);
    return 0;
}

int try_network_command(char **tokens, size_t count, VarStore *store, int *handled) {
    *handled = 0;

    if (count == 0 || tokens[0] == NULL) {
        return 0;
    }

    if (strcmp(tokens[0], "start-server") != 0 &&
        strcmp(tokens[0], "close-server") != 0 &&
        strcmp(tokens[0], "send") != 0 &&
        strcmp(tokens[0], "start-client") != 0) {
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(tokens[i], "|") == 0 || strcmp(tokens[i], "&") == 0) {
            display_error("ERROR: Builtin failed: ", tokens[0]);
            *handled = 1;
            return -1;
        }
    }

    *handled = 1;

    if (strcmp(tokens[0], "start-server") == 0) {
        return start_server_command(tokens);
    }
    if (strcmp(tokens[0], "close-server") == 0) {
        return close_server_command();
    }
    if (strcmp(tokens[0], "send") == 0) {
        return send_command(tokens);
    }
    if (strcmp(tokens[0], "start-client") == 0) {
        return start_client_command(tokens, store);
    }

    return 0;
}

void cleanup_all_children(void) {
    ProcNode *curr = proc_head;

    while (curr != NULL) {
        kill(curr->pid, SIGTERM);
        curr = curr->next;
    }

    if (server_pid > 0) {
        kill(server_pid, SIGTERM);
    }

    while (waitpid(-1, NULL, 0) > 0) {
    }

    server_pid = -1;

    while (proc_head != NULL) {
        ProcNode *next = proc_head->next;
        free(proc_head->name);
        free(proc_head);
        proc_head = next;
    }

    while (job_head != NULL) {
        JobNode *next = job_head->next;
        free(job_head->cmdline);
        free(job_head);
        job_head = next;
    }
}

/* ---------------- Existing builtin / job logic ---------------- */

ssize_t bn_ps(char **tokens) {
    if (token_count(tokens) > 1) {
        display_error("ERROR: Builtin failed: ", "ps");
        return -1;
    }

    ProcNode *curr = proc_head;
    while (curr != NULL) {
        char line[256];
        snprintf(line, sizeof(line), "%s %d\n", curr->name, (int)curr->pid);
        display_message(line);
        curr = curr->next;
    }

    return 0;
}

ssize_t bn_kill(char **tokens) {
    int argc = token_count(tokens);

    if (argc < 2 || argc > 3) {
        display_error("ERROR: Builtin failed: ", "kill");
        return -1;
    }

    char *endptr = NULL;
    long pid_long = strtol(tokens[1], &endptr, 10);
    if (*tokens[1] == '\0' || *endptr != '\0' || pid_long <= 0) {
        display_error("ERROR: ", "The process does not exist");
        return -1;
    }

    int signum = SIGTERM;
    if (argc == 3) {
        endptr = NULL;
        long sig_long = strtol(tokens[2], &endptr, 10);
        if (*tokens[2] == '\0' || *endptr != '\0' || sig_long <= 0 || sig_long >= NSIG) {
            display_error("ERROR: ", "Invalid signal specified");
            return -1;
        }
        signum = (int)sig_long;
    }

    if (kill((pid_t)pid_long, signum) != 0) {
        if (errno == ESRCH) {
            display_error("ERROR: ", "The process does not exist");
        } else if (errno == EINVAL) {
            display_error("ERROR: ", "Invalid signal specified");
        } else {
            display_error("ERROR: Builtin failed: ", "kill");
        }
        return -1;
    }

    return 0;
}

ssize_t bn_cd(char **tokens) {
    int argc = token_count(tokens);

    if (argc > 2) {
        display_error("ERROR: Too many arguments: ", "cd takes a single path");
        return -1;
    }

    const char *path = NULL;

    if (argc == 1) {
        struct passwd *pw = getpwuid(getuid());
        if (pw == NULL) {
            display_error("ERROR: Builtin failed: ", "cd");
            return -1;
        }

        path = pw->pw_dir;
        if (chdir(path) != 0) {
            display_error("ERROR: ", "Invalid path");
            return -1;
        }

        return 0;
    }

    char path_buf[4096];
    if (expand_dot_components(tokens[1], path_buf, sizeof(path_buf)) < 0) {
        display_error("ERROR: Builtin failed: ", "cd");
        return -1;
    }

    if (chdir(path_buf) != 0) {
        display_error("ERROR: ", "Invalid path");
        return -1;
    }

    return 0;
}

static int cmp_strptrs(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

static int ls_recursive(
    const char *path,
    int show_all,
    int filter_enabled,
    char *filter_substring,
    int current_depth,
    int max_depth
) {
    struct stat st;

    if (stat(path, &st) != 0) {
        display_error("ERROR: ", "Invalid path");
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        const char *base = strrchr(path, '/');
        base = (base == NULL) ? path : base + 1;

        int should_print = 1;
        if (filter_enabled && strstr(base, filter_substring) == NULL) {
            should_print = 0;
        }

        if (should_print) {
            display_message((char *)base);
            display_message("\n");
        }

        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        display_error("ERROR: ", "Invalid path");
        return -1;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        display_error("ERROR: ", "Invalid path");
        return -1;
    }

    char **names = NULL;
    size_t n_names = 0;
    size_t cap = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        char *copy = malloc(strlen(name) + 1);

        if (copy == NULL) {
            display_error("ERROR: Builtin failed: ", "ls");
            for (size_t j = 0; j < n_names; j++) {
                free(names[j]);
            }
            free(names);
            closedir(dir);
            return -1;
        }

        strcpy(copy, name);

        if (n_names == cap) {
            size_t new_cap = (cap == 0) ? 16 : cap * 2;
            char **tmp = realloc(names, new_cap * sizeof(char *));

            if (tmp == NULL) {
                display_error("ERROR: Builtin failed: ", "ls");
                free(copy);
                for (size_t j = 0; j < n_names; j++) {
                    free(names[j]);
                }
                free(names);
                closedir(dir);
                return -1;
            }

            names = tmp;
            cap = new_cap;
        }

        names[n_names++] = copy;
    }

    closedir(dir);
    qsort(names, n_names, sizeof(char *), cmp_strptrs);

    for (size_t i = 0; i < n_names; i++) {
        const char *name = names[i];
        int is_dot = (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);

        if (is_dot) {
            int should_print = 1;
            if (filter_enabled && strstr(name, filter_substring) == NULL) {
                should_print = 0;
            }

            if (should_print) {
                display_message((char *)name);
                display_message("\n");
            }

            continue;
        }

        if (name[0] == '.' && !show_all) {
            continue;
        }

        char new_path[4096];
        snprintf(new_path, sizeof(new_path), "%s/%s", path, name);

        struct stat child_st;
        int child_is_dir = (stat(new_path, &child_st) == 0 && S_ISDIR(child_st.st_mode));

        int should_print = 1;
        if (filter_enabled) {
            should_print = (strstr(name, filter_substring) != NULL);
        }

        if (should_print) {
            display_message((char *)name);
            display_message("\n");
        }

        if (child_is_dir && (max_depth == -1 || current_depth < max_depth)) {
            if (ls_recursive(
                    new_path,
                    show_all,
                    filter_enabled,
                    filter_substring,
                    current_depth + 1,
                    max_depth
                ) < 0) {
                for (size_t j = 0; j < n_names; j++) {
                    free(names[j]);
                }
                free(names);
                return -1;
            }
        }
    }

    for (size_t i = 0; i < n_names; i++) {
        free(names[i]);
    }

    free(names);
    return 0;
}

ssize_t bn_ls(char ** tokens) {
    int show_all = 0, filter_enabled = 0, recursive = 0, depth_provided = 0, max_depth = -1, path_provided = 0;
    char * filter_substring = NULL;
    const char * path = ".";

    for (int i = 1; tokens[i] != NULL; i++) {
        if (strcmp(tokens[i], "--a") == 0) show_all = 1;
        else if (strcmp(tokens[i], "--f") == 0) {
            if (tokens[i + 1] == NULL) {
                display_error("ERROR: Builtin failed: ", "ls");
                return -1;
            }
            filter_enabled = 1;
            filter_substring = tokens[++i];
        } else if (strcmp(tokens[i], "--rec") == 0) recursive = 1;
        else if (strcmp(tokens[i], "--d") == 0) {
            if (tokens[i + 1] == NULL) {
                display_error("ERROR: Builtin failed: ", "ls");
                return -1;
            }
            char * endptr;
            long val = strtol(tokens[++i], & endptr, 10);
            if ( * endptr != '\0' || val < 0) {
                display_error("ERROR: Builtin failed: ", "ls");
                return -1;
            }
            max_depth = (int) val;
            depth_provided = 1;
        } else if (tokens[i][0] == '-') {
            display_error("ERROR: Builtin failed: ", "ls");
            return -1;
        } else {
            if (path_provided) {
                display_error("ERROR: Too many arguments: ", "ls takes a single path");
                return -1;
            }
            path = tokens[i];
            path_provided = 1;
        }
    }

    if (depth_provided && !recursive) {
        display_error("ERROR: Builtin failed: ", "ls");
        return -1;
    }
    if (!recursive) max_depth = 0;
    char path_buf[4096];
    if (expand_dot_components(path, path_buf, sizeof(path_buf)) < 0) {
        display_error("ERROR: Builtin failed: ", "ls");
        return -1;
    }
    return ls_recursive(path_buf, show_all, filter_enabled, filter_substring, 0, max_depth);
}

static int open_input_fd(char ** tokens,
  const char * cmd_name, int * fd_out, int * close_when_done) {
    int argc = token_count(tokens);
    if (argc > 2) {
        if (strcmp(cmd_name, "cat") == 0) display_error("ERROR: Too many arguments: ", "cat takes a single file");
        else display_error("ERROR: Too many arguments: ", "wc takes a single file");
        return -1;
    }
    if (argc == 1) {
        * fd_out = STDIN_FILENO;* close_when_done = 0;
        return 0;
    }
    char path_buf[4096];
    if (expand_dot_components(tokens[1], path_buf, sizeof(path_buf)) < 0) {
        display_error("ERROR: Builtin failed: ", (char * ) cmd_name);
        return -1;
    }
    struct stat st;
    if (stat(path_buf, & st) != 0 || !S_ISREG(st.st_mode)) {
        display_error("ERROR: ", "Cannot open file");
        return -1;
    }
    int fd = open(path_buf, O_RDONLY);
    if (fd < 0) {
        display_error("ERROR: ", "Cannot open file");
        return -1;
    }
    * fd_out = fd;* close_when_done = 1;
    return 0;
}

static int read_first_chunk_or_report_no_input(int fd, char * buffer, ssize_t * bytes_read) {
    * bytes_read = read(fd, buffer, 4096);
    if ( * bytes_read < 0) {
        if (errno == EINTR && shell_sigint_flag_is_set()) return 1;
        if (fd == STDIN_FILENO && (errno == EIO || errno == EBADF)) {
            display_error("ERROR: ", "No input source provided");
            return -1;
        }
        return -2;
    }
    return 0;
}

ssize_t bn_cat(char ** tokens) {
    int fd, close_when_done;
    if (open_input_fd(tokens, "cat", & fd, & close_when_done) < 0) return -1;
    char buffer[4096];
    ssize_t bytes_read;
    int first_status = read_first_chunk_or_report_no_input(fd, buffer, & bytes_read);
    if (first_status == 1 || first_status == -1) {
        if (close_when_done) close(fd);
        return -1;
    }
    if (first_status == -2) {
        if (close_when_done) close(fd);
        display_error("ERROR: Builtin failed: ", "cat");
        return -1;
    }
    while (bytes_read > 0) {
        if (write(STDOUT_FILENO, buffer, (size_t) bytes_read) < 0) {
            if (close_when_done) close(fd);
            display_error("ERROR: Builtin failed: ", "cat");
            return -1;
        }
        bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            if (errno == EINTR && shell_sigint_flag_is_set()) {
                if (close_when_done) close(fd);
                return -1;
            }
            if (close_when_done) close(fd);
            display_error("ERROR: Builtin failed: ", "cat");
            return -1;
        }
    }
    if (close_when_done) close(fd);
    return 0;
}

ssize_t bn_wc(char ** tokens) {
    int fd, close_when_done;
    if (open_input_fd(tokens, "wc", & fd, & close_when_done) < 0) return -1;
    long word_count = 0, char_count = 0, newline_count = 0;
    int in_word = 0;
    char buffer[4096];
    ssize_t bytes_read;
    int first_status = read_first_chunk_or_report_no_input(fd, buffer, & bytes_read);
    if (first_status == 1 || first_status == -1) {
        if (close_when_done) close(fd);
        return -1;
    }
    if (first_status == -2) {
        if (close_when_done) close(fd);
        display_error("ERROR: Builtin failed: ", "wc");
        return -1;
    }
    while (bytes_read > 0) {
        for (ssize_t i = 0; i < bytes_read; i++) {
            char c = buffer[i];
            char_count++;
            if (c == '\n') newline_count++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (in_word) {
                    word_count++;
                    in_word = 0;
                }
            } else in_word = 1;
        }
        bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            if (errno == EINTR && shell_sigint_flag_is_set()) {
                if (close_when_done) close(fd);
                return -1;
            }
            if (close_when_done) close(fd);
            display_error("ERROR: Builtin failed: ", "wc");
            return -1;
        }
    }
    if (in_word) word_count++;
    if (close_when_done) close(fd);
    char output[128];
    snprintf(output, sizeof(output), "word count %ld\n", word_count);
    display_message(output);
    snprintf(output, sizeof(output), "character count %ld\n", char_count);
    display_message(output);
    snprintf(output, sizeof(output), "newline count %ld\n", newline_count);
    display_message(output);
    return 0;
}

int parse_job_from_tokens(char ** tokens, size_t count, Job * job) {
  job -> cmds = NULL;
  job -> num_cmds = 0;
  job -> background = 0;
  if (count == 0) return -1;
  if (strcmp(tokens[count - 1], "&") == 0) {
    job -> background = 1;
    count--;
  }
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tokens[i], "&") == 0) {
      display_error("ERROR: ", "Invalid syntax");
      return -1;
    }
  }
  if (count == 0) {
    display_error("ERROR: ", "Invalid syntax");
    return -1;
  }
  int num_cmds = 1;
  for (size_t i = 0; i < count; i++)
    if (strcmp(tokens[i], "|") == 0) num_cmds++;
  Command * cmds = calloc((size_t) num_cmds, sizeof(Command));
  if (cmds == NULL) {
    display_error("ERROR: ", "memory allocation failed");
    return -1;
  }
  int cmd_index = 0;
  size_t start = 0;
  for (size_t i = 0; i <= count; i++) {
    int at_split = (i == count) || (strcmp(tokens[i], "|") == 0);
    if (!at_split) continue;
    if (i == start) {
      free(cmds);
      display_error("ERROR: ", "Invalid syntax");
      return -1;
    }
    int argc = (int)(i - start);
    cmds[cmd_index].argc = argc;
    cmds[cmd_index].argv = calloc((size_t) argc + 1, sizeof(char * ));
    if (cmds[cmd_index].argv == NULL) {
      for (int j = 0; j < cmd_index; j++) {
        for (int k = 0; k < cmds[j].argc; k++) free(cmds[j].argv[k]);
        free(cmds[j].argv);
      }
      free(cmds);
      display_error("ERROR: ", "memory allocation failed");
      return -1;
    }
    for (int a = 0; a < argc; a++) {
      cmds[cmd_index].argv[a] = xstrdup_local(tokens[start + (size_t) a]);
      if (cmds[cmd_index].argv[a] == NULL) {
        for (int j = 0; j <= cmd_index; j++) {
          if (cmds[j].argv != NULL) {
            for (int k = 0; k < cmds[j].argc; k++) free(cmds[j].argv[k]);
            free(cmds[j].argv);
          }
        }
        free(cmds);
        display_error("ERROR: ", "memory allocation failed");
        return -1;
      }
    }
    cmds[cmd_index].argv[argc] = NULL;
    cmd_index++;
    start = i + 1;
  }
  job -> cmds = cmds;
  job -> num_cmds = num_cmds;
  return 0;
}

void free_job(Job * job) {
  if (job == NULL || job -> cmds == NULL) return;
  for (int i = 0; i < job -> num_cmds; i++) {
    if (job -> cmds[i].argv != NULL) {
      for (int j = 0; j < job -> cmds[i].argc; j++) free(job -> cmds[i].argv[j]);
      free(job -> cmds[i].argv);
    }
  }
  free(job -> cmds);
  job -> cmds = NULL;
  job -> num_cmds = 0;
  job -> background = 0;
}

static void exec_command_in_child(Command * cmd) {
  if (cmd -> argc == 0 || cmd -> argv == NULL || cmd -> argv[0] == NULL) _exit(1);
  signal(SIGINT, SIG_DFL);
  if (strcmp(cmd -> argv[0], "exit") == 0) _exit(0);
  bn_ptr builtin = check_builtin(cmd -> argv[0]);
  if (builtin != NULL) {
    ssize_t rc = builtin(cmd -> argv);
    _exit(rc == 0 ? 0 : 1);
  }
  execvp(cmd -> argv[0], cmd -> argv);
  if (errno == ENOENT) display_error("ERROR: Unknown command: ", cmd -> argv[0]);
  else display_error("ERROR: Builtin failed: ", cmd -> argv[0]);
  _exit(1);
}

ssize_t run_job(Job * job) {
  if (job == NULL || job -> num_cmds <= 0) return -1;
  if (job -> num_cmds == 1 && !job -> background) {
    Command * cmd = & job -> cmds[0];
    if (strcmp(cmd -> argv[0], "exit") == 0) return 0;
    if (strcmp(cmd -> argv[0], "cd") == 0) return bn_cd(cmd -> argv);
    bn_ptr builtin = check_builtin(cmd -> argv[0]);
    if (builtin != NULL) {
      clear_shell_sigint_flag();
      set_shell_signal_mode(SIGMODE_BUILTIN);
      ssize_t rc = builtin(cmd -> argv);
      set_shell_signal_mode(SIGMODE_NONE);
      return rc;
    }
  }

  int num_pipes = job -> num_cmds - 1;
  int * pipefds = NULL;
  if (num_pipes > 0) {
    pipefds = malloc((size_t)(2 * num_pipes) * sizeof(int));
    if (pipefds == NULL) {
      display_error("ERROR: ", "memory allocation failed");
      return -1;
    }
    for (int i = 0; i < num_pipes; i++) {
      if (pipe(pipefds + (2 * i)) < 0) {
        free(pipefds);
        display_error("ERROR: ", "Builtin failed: pipe");
        return -1;
      }
    }
  }

  pid_t * pids = malloc((size_t) job -> num_cmds * sizeof(pid_t));
  if (pids == NULL) {
    if (pipefds != NULL) {
      for (int i = 0; i < 2 * num_pipes; i++) close(pipefds[i]);
      free(pipefds);
    }
    display_error("ERROR: ", "memory allocation failed");
    return -1;
  }

  for (int i = 0; i < job -> num_cmds; i++) {
    pid_t pid = fork();
    if (pid < 0) {
      if (pipefds != NULL) {
        for (int j = 0; j < 2 * num_pipes; j++) close(pipefds[j]);
        free(pipefds);
      }
      free(pids);
      display_error("ERROR: ", "Builtin failed: fork");
      return -1;
    }
    if (pid == 0) {
      if (i > 0) {
        if (dup2(pipefds[2 * (i - 1)], STDIN_FILENO) < 0) _exit(1);
      } else if (job -> background) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
          dup2(devnull, STDIN_FILENO);
          close(devnull);
        }
      }
      if (i < job -> num_cmds - 1) {
        if (dup2(pipefds[2 * i + 1], STDOUT_FILENO) < 0) _exit(1);
      }
      if (pipefds != NULL) {
        for (int j = 0; j < 2 * num_pipes; j++) close(pipefds[j]);
      }
      proc_head = NULL;
      job_head = NULL;
      clear_foreground_pids();
      set_shell_signal_mode(SIGMODE_NONE);
      clear_shell_sigint_flag();
      exec_command_in_child( & job -> cmds[i]);
    }
    pids[i] = pid;
    add_proc(pid, job -> cmds[i].argv[0]);
  }

  if (pipefds != NULL) {
    for (int i = 0; i < 2 * num_pipes; i++) close(pipefds[i]);
    free(pipefds);
  }

  if (job -> background) {
    int job_id = next_job_id();
    char * job_str = build_job_string(job);
    if (job_str == NULL) {
      free(pids);
      display_error("ERROR: ", "memory allocation failed");
      return -1;
    }
    add_background_job(job_id, job_str, job -> num_cmds, pids, job -> num_cmds);
    char msg[128];
    snprintf(msg, sizeof(msg), "[%d] %d\n", job_id, (int) pids[job -> num_cmds - 1]);
    display_message(msg);
    free(job_str);
    free(pids);
    return 0;
  }

  clear_shell_sigint_flag();
  set_foreground_pids(pids, job -> num_cmds);
  set_shell_signal_mode(SIGMODE_WAITFG);

  for (int i = 0; i < job -> num_cmds; i++) {
    int status;
    while (waitpid(pids[i], & status, 0) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    remove_proc(pids[i]);
  }

  clear_foreground_pids();
  set_shell_signal_mode(SIGMODE_NONE);
  free(pids);
  return 0;
}