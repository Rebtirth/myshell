#include "parser.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum token_type {
    TOKEN_END,
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_INPUT,
    TOKEN_OUTPUT,
    TOKEN_APPEND
};

struct token {
    enum token_type type;
    char *word;
};

struct lexer {
    const char *cursor;
};

/* 倍增容量，避免长命令不断复制；所有乘法都先检查溢出。 */
static void *grow_array(void *array, size_t *capacity, size_t needed,
                        size_t element_size)
{
    size_t new_capacity;
    void *new_array;

    if (needed <= *capacity) {
        return array;
    }
    if (needed > SIZE_MAX / element_size) {
        return NULL;
    }
    new_capacity = *capacity == 0 ? 8 : *capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / element_size) {
        new_capacity = needed;
    }
    new_array = realloc(array, new_capacity * element_size);
    if (new_array == NULL) {
        return NULL;
    }
    *capacity = new_capacity;
    return new_array;
}

static int append_character(char **word, size_t *length, size_t *capacity,
                            char character)
{
    char *new_word;

    if (*length > SIZE_MAX - 2) {
        return -1;
    }
    new_word = grow_array(*word, capacity, *length + 2, sizeof(**word));
    if (new_word == NULL) {
        return -1;
    }
    *word = new_word;
    (*word)[(*length)++] = character;
    (*word)[*length] = '\0';
    return 0;
}

static int is_unsupported_operator(char character)
{
    return character == '&' || character == ';' || character == '(' ||
           character == ')' || character == '`';
}

/* 词法分析只处理引号和转义，不执行变量、通配符或命令替换。 */
static int next_token(struct lexer *lexer, struct token *token,
                      const char **error)
{
    const char *cursor = lexer->cursor;
    char *word = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int plain_digits = 1;

    token->word = NULL;
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0') {
        token->type = TOKEN_END;
        lexer->cursor = cursor;
        return 0;
    }
    if (*cursor == '|') {
        token->type = TOKEN_PIPE;
        lexer->cursor = cursor + 1;
        return 0;
    }
    if (*cursor == '<') {
        if (cursor[1] == '<') {
            *error = "here-documents (<<) are not supported";
            return 1;
        }
        token->type = TOKEN_INPUT;
        lexer->cursor = cursor + 1;
        return 0;
    }
    if (*cursor == '>') {
        token->type = cursor[1] == '>' ? TOKEN_APPEND : TOKEN_OUTPUT;
        lexer->cursor = cursor + (cursor[1] == '>' ? 2 : 1);
        return 0;
    }

    while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
           *cursor != '|' && *cursor != '<' && *cursor != '>') {
        if (is_unsupported_operator(*cursor)) {
            *error = "unsupported control operator; quote it to use it literally";
            goto syntax_error;
        }
        if (*cursor == '\'') {
            plain_digits = 0;
            ++cursor;
            while (*cursor != '\0' && *cursor != '\'') {
                if (append_character(&word, &length, &capacity, *cursor++) < 0) {
                    goto memory_error;
                }
            }
            if (*cursor == '\0') {
                *error = "unterminated single quote";
                goto syntax_error;
            }
            ++cursor;
        } else if (*cursor == '"') {
            plain_digits = 0;
            ++cursor;
            while (*cursor != '\0' && *cursor != '"') {
                if (*cursor == '\\') {
                    char escaped = cursor[1];

                    if (escaped == '\0') {
                        *error = "unterminated double quote";
                        goto syntax_error;
                    }
                    if (escaped == '\n') {
                        cursor += 2;
                        continue;
                    }
                    if (escaped == '$' || escaped == '`' || escaped == '"' ||
                        escaped == '\\') {
                        ++cursor;
                    }
                    /* 在双引号里，\q 等普通字符前的反斜杠需要保留。 */
                }
                if (append_character(&word, &length, &capacity, *cursor++) < 0) {
                    goto memory_error;
                }
            }
            if (*cursor == '\0') {
                *error = "unterminated double quote";
                goto syntax_error;
            }
            ++cursor;
        } else if (*cursor == '\\') {
            plain_digits = 0;
            ++cursor;
            if (*cursor == '\0') {
                *error = "trailing backslash";
                goto syntax_error;
            }
            if (*cursor == '\n') {
                *error = "line continuations are not supported";
                goto syntax_error;
            }
            if (append_character(&word, &length, &capacity, *cursor++) < 0) {
                goto memory_error;
            }
        } else {
            if (!isdigit((unsigned char)*cursor)) {
                plain_digits = 0;
            }
            if (append_character(&word, &length, &capacity, *cursor++) < 0) {
                goto memory_error;
            }
        }
    }

    /* 2>file 是文件描述符语法，不能误当成参数 "2" 加普通重定向。 */
    if (plain_digits && length != 0 && (*cursor == '<' || *cursor == '>')) {
        *error = "file descriptor redirections (such as 2>) are not supported";
        goto syntax_error;
    }
    if (word == NULL) {
        word = malloc(1);
        if (word == NULL) {
            goto memory_error;
        }
        word[0] = '\0';
    }
    token->type = TOKEN_WORD;
    token->word = word;
    lexer->cursor = cursor;
    return 0;

syntax_error:
    free(word);
    return 1;

memory_error:
    free(word);
    *error = "out of memory";
    return -1;
}

static void free_command(struct command *command)
{
    size_t i;

    for (i = 0; i < command->argc; ++i) {
        free(command->argv[i]);
    }
    for (i = 0; i < command->redirection_count; ++i) {
        free(command->redirections[i].path);
    }
    free(command->argv);
    free(command->redirections);
    memset(command, 0, sizeof(*command));
}

void free_pipeline(struct pipeline *pipeline)
{
    size_t i;

    if (pipeline == NULL) {
        return;
    }
    for (i = 0; i < pipeline->count; ++i) {
        free_command(&pipeline->commands[i]);
    }
    free(pipeline->commands);
    pipeline->commands = NULL;
    pipeline->count = 0;
}

static int append_argument(struct command *command, size_t *capacity, char *word)
{
    char **new_argv;

    if (command->argc > SIZE_MAX - 2) {
        return -1;
    }
    new_argv = grow_array(command->argv, capacity, command->argc + 2,
                         sizeof(*command->argv));
    if (new_argv == NULL) {
        return -1;
    }
    command->argv = new_argv;
    command->argv[command->argc++] = word;
    command->argv[command->argc] = NULL;
    return 0;
}

static int append_redirection(struct command *command, size_t *capacity,
                              enum redirection_type type, char *path)
{
    struct redirection *new_redirections;

    if (command->redirection_count == SIZE_MAX) {
        return -1;
    }
    new_redirections = grow_array(command->redirections, capacity,
                                 command->redirection_count + 1,
                                 sizeof(*command->redirections));
    if (new_redirections == NULL) {
        return -1;
    }
    command->redirections = new_redirections;
    command->redirections[command->redirection_count].type = type;
    command->redirections[command->redirection_count].path = path;
    ++command->redirection_count;
    return 0;
}

static int append_command(struct pipeline *pipeline, size_t *capacity,
                          struct command *command)
{
    struct command *new_commands;

    if (pipeline->count == SIZE_MAX) {
        return -1;
    }
    new_commands = grow_array(pipeline->commands, capacity, pipeline->count + 1,
                             sizeof(*pipeline->commands));
    if (new_commands == NULL) {
        return -1;
    }
    pipeline->commands = new_commands;
    pipeline->commands[pipeline->count++] = *command;
    memset(command, 0, sizeof(*command));
    return 0;
}

int parse_pipeline(const char *line, struct pipeline *result, const char **error)
{
    struct lexer lexer;
    struct token token = {TOKEN_END, NULL};
    struct command command = {NULL, 0, NULL, 0};
    size_t argument_capacity = 0;
    size_t redirection_capacity = 0;
    size_t command_capacity = 0;
    const char *message = NULL;
    int status = 0;

    if (error != NULL) {
        *error = NULL;
    }
    if (result == NULL || line == NULL) {
        if (error != NULL) {
            *error = "invalid parser argument";
        }
        return 1;
    }
    result->commands = NULL;
    result->count = 0;
    lexer.cursor = line;

    for (;;) {
        status = next_token(&lexer, &token, &message);
        if (status != 0) {
            goto failure;
        }
        if (token.type == TOKEN_WORD) {
            if (append_argument(&command, &argument_capacity, token.word) < 0) {
                goto memory_error;
            }
            token.word = NULL;
        } else if (token.type == TOKEN_INPUT || token.type == TOKEN_OUTPUT ||
                   token.type == TOKEN_APPEND) {
            enum redirection_type type = token.type == TOKEN_INPUT ? REDIR_INPUT :
                token.type == TOKEN_OUTPUT ? REDIR_OUTPUT : REDIR_APPEND;

            status = next_token(&lexer, &token, &message);
            if (status != 0) {
                goto failure;
            }
            if (token.type != TOKEN_WORD) {
                message = "redirection requires a filename";
                status = 1;
                goto failure;
            }
            if (append_redirection(&command, &redirection_capacity,
                                   type, token.word) < 0) {
                goto memory_error;
            }
            token.word = NULL;
        } else if (token.type == TOKEN_PIPE || token.type == TOKEN_END) {
            if (command.argc == 0) {
                if (token.type == TOKEN_END && result->count == 0 &&
                    command.redirection_count == 0) {
                    return 0;
                }
                message = "each pipeline stage requires a command";
                status = 1;
                goto failure;
            }
            if (append_command(result, &command_capacity, &command) < 0) {
                goto memory_error;
            }
            argument_capacity = 0;
            redirection_capacity = 0;
            if (token.type == TOKEN_END) {
                return 0;
            }
        }
    }

memory_error:
    status = -1;
    message = "out of memory";

failure:
    free(token.word);
    free_command(&command);
    free_pipeline(result);
    if (error != NULL) {
        *error = message;
    }
    return status;
}
