#ifndef MYSHELL_PARSER_H
#define MYSHELL_PARSER_H

#include <stddef.h>

enum redirection_type {
    REDIR_INPUT,
    REDIR_OUTPUT,
    REDIR_APPEND
};

struct redirection {
    enum redirection_type type;
    char *path;
};

struct command {
    char **argv;
    size_t argc;
    struct redirection *redirections;
    size_t redirection_count;
};

struct pipeline {
    struct command *commands;
    size_t count;
};

/* argv 始终以 NULL 结尾；重定向按输入顺序保存。 */
/* 返回 0：成功；1：语法错误；-1：内存不足。error 指向静态字符串。 */
int parse_pipeline(const char *line, struct pipeline *result, const char **error);
void free_pipeline(struct pipeline *pipeline);

#endif
