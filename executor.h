#ifndef MYSHELL_EXECUTOR_H
#define MYSHELL_EXECUTOR_H

#include <stdbool.h>

#include "parser.h"

/* 单个内建命令在父进程中执行；管道内的内建命令在子进程中执行。 */
int execute_pipeline(const struct pipeline *pipeline, bool interactive,
                     int previous_status, bool *exit_requested);

#endif
