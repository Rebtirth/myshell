#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "executor.h"

static bool is_builtin(const struct command *command)
{
    return strcmp(command->argv[0], "cd") == 0 ||
           strcmp(command->argv[0], "pwd") == 0 ||
           strcmp(command->argv[0], "exit") == 0;
}

static int print_directory(void)
{
    size_t capacity = 256;
    char *directory;

    for (;;) {
        directory = malloc(capacity);
        if (directory == NULL) {
            perror("myshell: pwd");
            return 1;
        }
        if (getcwd(directory, capacity) != NULL) {
            break;
        }
        free(directory);
        if (errno != ERANGE || capacity > SIZE_MAX / 2) {
            perror("myshell: pwd");
            return 1;
        }
        capacity *= 2;
    }
    int result = puts(directory) == EOF ? 1 : 0;
    free(directory);
    return result;
}

static int run_builtin(const struct command *command, int previous_status,
                       bool *exit_requested)
{
    if (strcmp(command->argv[0], "cd") == 0) {
        const char *path;
        if (command->argc > 2) {
            fprintf(stderr, "myshell: cd: too many arguments\n");
            return 2;
        }
        path = command->argc == 2 ? command->argv[1] : getenv("HOME");
        if (path == NULL) {
            fprintf(stderr, "myshell: cd: HOME is not set\n");
            return 1;
        }
        if (chdir(path) == -1) {
            fprintf(stderr, "myshell: cd: %s: %s\n", path, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (strcmp(command->argv[0], "pwd") == 0) {
        if (command->argc != 1) {
            fprintf(stderr, "myshell: pwd: arguments are not supported\n");
            return 2;
        }
        return print_directory();
    }

    int status = previous_status;
    if (command->argc >= 2) {
        const char *text = command->argv[1];
        const char *digit = text;
        char *end;
        long value;
        if (*digit == '+' || *digit == '-') {
            ++digit;
        }
        while (*digit >= '0' && *digit <= '9') {
            ++digit;
        }
        errno = 0;
        value = strtol(text, &end, 10);
        if (*text == '\0' || end == text || *digit != '\0' ||
            *end != '\0' || errno == ERANGE) {
            fprintf(stderr, "myshell: exit: %s: numeric argument required\n", text);
            *exit_requested = true;
            return 2;
        }
        status = (unsigned char)value;
    }
    if (command->argc > 2) {
        fprintf(stderr, "myshell: exit: too many arguments\n");
        return 2;
    }
    *exit_requested = true;
    return status;
}

static int duplicate_to(int source, int destination)
{
    int result;
    do {
        result = dup2(source, destination);
    } while (result == -1 && errno == EINTR);
    return result;
}

static int apply_redirections(const struct command *command)
{
    for (size_t i = 0; i < command->redirection_count; ++i) {
        const struct redirection *redirection = &command->redirections[i];
        int target = redirection->type == REDIR_INPUT ? STDIN_FILENO : STDOUT_FILENO;
        int flags = redirection->type == REDIR_INPUT ? O_RDONLY :
                    O_WRONLY | O_CREAT |
                    (redirection->type == REDIR_APPEND ? O_APPEND : O_TRUNC);
        int descriptor = open(redirection->path, flags, 0666);
        if (descriptor == -1) {
            /* FIFO 的 open 可能阻塞；父进程内建命令收到 Ctrl+C 后要返回。 */
            if (errno == EINTR) {
                return 128 + SIGINT;
            }
            fprintf(stderr, "myshell: %s: %s\n", redirection->path, strerror(errno));
            return 1;
        }
        if (duplicate_to(descriptor, target) == -1) {
            perror("myshell: dup2");
            if (descriptor != target) {
                close(descriptor);
            }
            return 1;
        }
        if (descriptor != target) {
            close(descriptor);
        }
    }
    return 0;
}

static int run_parent_builtin(const struct command *command, int previous_status,
                              bool *exit_requested)
{
    if (command->redirection_count == 0) {
        int status = run_builtin(command, previous_status, exit_requested);
        if (fflush(stdout) == EOF) {
            perror("myshell: stdout");
            clearerr(stdout);
            return 1;
        }
        return status;
    }

    /* 切换 fd 前后都刷新 stdio，避免 pwd 的缓冲输出跑到错误的文件。 */
    if (fflush(stdout) == EOF) {
        perror("myshell: stdout");
        clearerr(stdout);
        return 1;
    }
    int saved_input = dup(STDIN_FILENO);
    int saved_output = dup(STDOUT_FILENO);
    if (saved_input == -1 || saved_output == -1) {
        perror("myshell: dup");
        if (saved_input != -1) {
            close(saved_input);
        }
        if (saved_output != -1) {
            close(saved_output);
        }
        return 1;
    }
    int status = apply_redirections(command);
    if (status == 0) {
        status = run_builtin(command, previous_status, exit_requested);
    }
    if (fflush(stdout) == EOF) {
        perror("myshell: stdout");
        status = 1;
    }
    int restored_input = duplicate_to(saved_input, STDIN_FILENO);
    int restored_output = duplicate_to(saved_output, STDOUT_FILENO);
    if (restored_input == -1 || restored_output == -1) {
        perror("myshell: restore redirection");
        *exit_requested = true;
        status = 1;
    }
    close(saved_input);
    close(saved_output);
    clearerr(stdout);
    return status;
}

static int restore_child_signals(bool interactive)
{
    if (!interactive) {
        return 0;
    }
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    /* 子命令恢复 Ctrl+C/Ctrl+\\，但本阶段尚不支持 Ctrl+Z 作业控制。 */
    return sigaction(SIGINT, &action, NULL) == -1 ||
           sigaction(SIGQUIT, &action, NULL) == -1 ? -1 : 0;
}

static int wait_for_child(pid_t pid)
{
    int wait_status;
    pid_t result;
    do {
        result = waitpid(pid, &wait_status, 0);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        perror("myshell: waitpid");
        return 1;
    }
    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        return 128 + WTERMSIG(wait_status);
    }
    return 1;
}

int execute_pipeline(const struct pipeline *pipeline, bool interactive,
                     int previous_status, bool *exit_requested)
{
    if (pipeline->count == 1 && is_builtin(&pipeline->commands[0])) {
        return run_parent_builtin(&pipeline->commands[0], previous_status,
                                  exit_requested);
    }

    pid_t *pids = calloc(pipeline->count, sizeof(*pids));
    if (pids == NULL) {
        perror("myshell: pipeline");
        return 1;
    }
    size_t started = 0;
    int previous_input = -1;
    int status = 1;
    bool failed = false;
    fflush(NULL);

    /* 先启动全部子进程再等待；否则超过管道缓冲区的输出会死锁。 */
    for (size_t i = 0; i < pipeline->count; ++i) {
        int next_pipe[2] = {-1, -1};
        if (i + 1 < pipeline->count && pipe(next_pipe) == -1) {
            perror("myshell: pipe");
            failed = true;
            break;
        }
        pid_t pid = fork();
        if (pid == -1) {
            perror("myshell: fork");
            if (next_pipe[0] != -1) {
                close(next_pipe[0]);
                close(next_pipe[1]);
            }
            failed = true;
            break;
        }
        if (pid == 0) {
            const struct command *command = &pipeline->commands[i];
            if (restore_child_signals(interactive) == -1) {
                perror("myshell: sigaction");
                _exit(1);
            }
            if ((previous_input != -1 &&
                 duplicate_to(previous_input, STDIN_FILENO) == -1) ||
                (next_pipe[1] != -1 &&
                 duplicate_to(next_pipe[1], STDOUT_FILENO) == -1)) {
                perror("myshell: dup2");
                _exit(1);
            }
            if (previous_input != -1) {
                close(previous_input);
            }
            if (next_pipe[0] != -1) {
                close(next_pipe[0]);
                close(next_pipe[1]);
            }
            /* 重定向在接好管道之后应用，可覆盖管道的 stdin/stdout。 */
            int redirect_status = apply_redirections(command);
            if (redirect_status != 0) {
                _exit(redirect_status);
            }
            if (is_builtin(command)) {
                bool child_exit = false;
                int child_status = run_builtin(command, previous_status, &child_exit);
                if (fflush(stdout) == EOF) {
                    child_status = 1;
                }
                _exit(child_status);
            }
            execvp(command->argv[0], command->argv);
            int saved_errno = errno;
            fprintf(stderr, "myshell: %s: %s\n", command->argv[0],
                    strerror(saved_errno));
            /* _exit 不会再次刷新从父进程继承的 stdio 缓冲区。 */
            _exit(saved_errno == ENOENT || saved_errno == ENOTDIR ? 127 : 126);
        }
        pids[started++] = pid;
        if (previous_input != -1) {
            close(previous_input);
        }
        if (next_pipe[1] != -1) {
            close(next_pipe[1]);
        }
        previous_input = next_pipe[0];
    }
    if (previous_input != -1) {
        close(previous_input);
    }
    if (failed) {
        /* 部分创建失败时终止已启动的直接子进程，避免留下阻塞的管道。 */
        for (size_t i = 0; i < started; ++i) {
            kill(pids[i], SIGKILL);
        }
    }
    for (size_t i = 0; i < started; ++i) {
        int child_status = wait_for_child(pids[i]);
        if (i + 1 == pipeline->count) {
            status = child_status;
        }
    }
    free(pids);
    return failed ? 1 : status;
}
