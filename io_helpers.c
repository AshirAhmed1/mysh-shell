#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include "io_helpers.h"

// ===== Internal write helper =====

static void write_all(int fd, const char *buf, size_t len) {
    size_t written = 0;

    while (written < len) {
        ssize_t rc = write(fd, buf + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        written += (size_t)rc;
    }
}


// ===== Output helpers =====

/* Prereq: str is a NULL terminated string
 */
void display_message(char *str) {
    write_all(STDOUT_FILENO, str, strlen(str));
}


/* Prereq: pre_str, str are NULL terminated strings
 */
void display_error(char *pre_str, char *str) {
    write_all(STDERR_FILENO, pre_str, strlen(pre_str));
    write_all(STDERR_FILENO, str, strlen(str));
    write_all(STDERR_FILENO, "\n", 1);
}


// ===== Input tokenizing =====

/* Prereq: in_ptr points to a character buffer of size > MAX_STR_LEN
 * Return: number of bytes read
 */
ssize_t get_input(char *in_ptr) {
    ssize_t n = read(STDIN_FILENO, in_ptr, MAX_STR_LEN);

    if (n < 0) {
        if (errno == EIO) {
            in_ptr[0] = '\0';
            return 0;
        }
        in_ptr[0] = '\0';
        return -1;
    }

    if (n == 0) {
        in_ptr[0] = '\0';
        return 0;
    }

    in_ptr[n] = '\0';
    return n;
}


/* Prereq: in_ptr is a string, tokens is of size >= len(in_ptr)
 * Warning: in_ptr is modified
 * Return: number of tokens.
 */
size_t tokenize_input(char *in_ptr, char **tokens) {
    char *curr_ptr = strtok(in_ptr, DELIMITERS);
    size_t token_count = 0;

    while (curr_ptr != NULL) {
        tokens[token_count] = curr_ptr;
        token_count += 1;
        curr_ptr = strtok(NULL, DELIMITERS);
    }

    tokens[token_count] = NULL;
    return token_count;
}