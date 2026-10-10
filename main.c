#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "executor.h"
#include "parser.h"

static volatile sig_atomic_t interrupted;

static void handle_interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static int configure_signals(void)
{
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = handle_interrupt;
    /* 不设置 SA_RESTART，让空闲时的 getline 能被 Ctrl+C 中断。 */
    if (sigaction(SIGINT, &action, NULL) == -1) {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGQUIT, &action, NULL) == -1 ||
        sigaction(SIGTSTP, &action, NULL) == -1) {
        return -1;
    }
    return 0;
}

int main(void)
{
    bool interactive = isatty(STDIN_FILENO) != 0;
    bool exit_requested = false;
    char *line = NULL;
    size_t capacity = 0;
    int status = 0;

    /* 不预读后续命令行，避免抢走 cat 等子命令需要的标准输入。 */
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        fputs("myshell: cannot configure stdin buffering\n", stderr);
        return 1;
    }
    if (interactive && configure_signals() == -1) {
        perror("myshell: sigaction");
        return 1;
    }

    while (!exit_requested) {
        struct pipeline pipeline = {0};
        const char *error = NULL;
        ssize_t length;
        int parse_status;

        interrupted = 0;
        if (interactive) {
            fputs("myshell> ", stderr);
            fflush(stderr);
        }
        errno = 0;
        length = getline(&line, &capacity, stdin);
        if (length == -1) {
            if (errno == EINTR) {
                /* 中断后的 FILE 错误标志也要清除，才能继续读输入。 */
                clearerr(stdin);
                status = 128 + SIGINT;
                if (interactive) {
                    fputc('\n', stderr);
                }
                continue;
            }
            if (feof(stdin)) {
                if (interactive) {
                    fputc('\n', stderr);
                }
                break;
            }
            perror("myshell: getline");
            status = 1;
            break;
        }
        if (interrupted) {
            status = 128 + SIGINT;
            if (interactive) {
                fputc('\n', stderr);
            }
            continue;
        }

        /* 输入中的 NUL 不能静默截断成另一条命令。 */
        if (memchr(line, '\0', (size_t)length) != NULL) {
            fprintf(stderr, "myshell: NUL bytes are not supported\n");
            status = 2;
            continue;
        }

        parse_status = parse_pipeline(line, &pipeline, &error);
        if (parse_status != 0) {
            fprintf(stderr, "myshell: %s\n", error ? error : "parse failed");
            status = parse_status < 0 ? 1 : 2;
        } else if (pipeline.count != 0) {
            status = execute_pipeline(&pipeline, interactive, status,
                                      &exit_requested);
            if (interactive && interrupted) {
                fputc('\n', stderr);
            }
        }
        free_pipeline(&pipeline);
    }

    free(line);
    return status;
}
