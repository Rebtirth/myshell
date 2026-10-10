# myshell

用于学习 Linux 系统编程的简易 Shell：读取一行输入，解析命令与重定向，再通过 `fork`、`execvp`、`pipe`、`dup2` 和 `waitpid` 执行。目标是让代码和系统调用的关系容易理解，当前没有宣称兼容 POSIX Shell 或 Bash。

## 构建与运行

在 WSL/Ubuntu 中进入项目的 `myshell` 目录，例如：

```sh
cd /home/rebtirth/project/myshell
gcc -std=c11 -Wall -Wextra -Wpedantic -g main.c parser.c executor.c -o myshell
./myshell
```

只需要 GCC 和 Python 3，无需安装第三方 Python 包。有 GNU Make 时，也可以使用可选的构建入口：

```sh
make
make test
make clean
```

`make clean` 只删除可执行文件 `myshell` 和三个 `.o` 文件。

## 已支持的功能

- 根据 `PATH` 查找外部命令，也支持 `/bin/ls`、`./program` 这样的路径。
- 动态读取长行、动态保存参数；单引号、双引号和反斜杠转义保留带空格的参数及空参数。
- 多阶段管道 `|`，以及输入 `<`、覆盖输出 `>`、追加输出 `>>`。重定向按出现顺序应用，位于管道连接之后。
- 内建命令 `cd [path]`、`pwd`、`exit [N]`。`cd` 无参数时使用 `HOME`；`pwd` 不接受选项；`exit` 无参数时使用上次命令状态，数字状态取低八位。
- 单个内建命令在父进程运行，重定向后恢复标准输入和输出。管道中的内建命令在子进程运行，不会切换父 Shell 的目录，也不会使父 Shell 退出。
- 命令流输入和 EOF 退出；非交互模式不输出提示符。命令自身读取标准输入时，可以消耗命令流中剩下的内容。
- 基本 Ctrl-C：交互 Shell 保持运行，前台子命令恢复默认中断行为。子程序自行修改信号处理时，行为由子程序决定。

示例，在 `myshell>` 后输入：

```sh
echo "a|b"
printf "hello\\n" | wc -c
printf "first\\n" > demo.txt
printf "second\\n" >> demo.txt
cat < demo.txt | wc -l
pwd > directory.txt
cd /tmp
pwd
exit
```

第一条输出 `a|b`，第二条计数为 `6`，读取 `demo.txt` 的管道计数为 `2`。引号内的 `|`、`<`、`>` 是普通参数内容。

输入输出重定向可以覆盖管道：`echo hello > demo.txt | wc -c` 将文字写入文件，右侧读取到 EOF，输出 `0`。`pwd > first.txt > second.txt` 按顺序创建或截断两个文件，最终输出进入 `second.txt`。

Shell 返回最后一条命令的状态；管道返回最后一个阶段的状态，并等待全部阶段结束。语法错误返回 `2`，未找到命令返回 `127`，无法执行返回 `126`，被信号终止的子命令返回 `128 + 信号编号`。例如 `false | true` 返回 `0`，`true | false` 返回 `1`。

## 当前限制

尚未实现变量展开（例如 `$HOME`）、通配符展开、`~` 展开、命令替换、多行命令、`;`、`&&`、`||`、后台执行 `&`、`jobs`、`fg`、`bg`。需要实际路径时直接写路径。

不支持数字文件描述符重定向，例如 `2>`、`2>&1`；只有重定向而没有命令的输入也会被拒绝。本版本只解析单行，不支持引号跨行或反斜杠续行。

Ctrl-Z 暂不支持：交互 Shell 和它启动的子进程忽略 SIGTSTP，避免命令停止后父进程一直等待。完整作业控制需要另外实现进程组和终端控制；子程序可以自行覆盖此信号设置。

## 项目结构

```text
myshell/
├── main.c                # 读行、提示符、交互信号与主循环
├── parser.c / parser.h   # 分词、语法检查、管道和重定向数据结构
├── executor.c / executor.h # 内建命令、进程、文件描述符与等待
├── tests/test_shell.py   # Python 标准库 unittest 回归测试
├── Makefile              # 可选构建入口
├── README.md
└── .gitignore
```

## 测试

先编译，再从项目的 `myshell` 目录执行：

```sh
python3 tests/test_shell.py "$PWD/myshell" -v
```

测试使用 Python 标准库 `unittest`，在临时目录中验证命令、引号、长输入、管道、重定向、内建命令和错误恢复；交互测试使用伪终端验证信号行为。也可用 `-k` 选择部分测试。

可选：用 GCC 的 AddressSanitizer 和 UndefinedBehaviorSanitizer 检查内存及未定义行为，然后重新运行测试：

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer main.c parser.c executor.c -o myshell
python3 tests/test_shell.py "$PWD/myshell" -v
```

## 后续学习路线

1. 先读 `main.c`，跟踪一条命令从输入到退出状态的完整过程，再读解析器的引号状态和内存释放。
2. 在执行器中画出 `printf hello | wc -c` 的文件描述符关系，理解为什么必须先启动全部阶段、关闭无用管道端，再等待。
3. 每次选一个功能补测试再实现，例如环境变量展开或通配符；展开规则需要保留引号上下文，不能只做字符串替换。
4. 最后再加入作业控制：学习 `setpgid`、`tcsetpgrp`、停止与继续状态，维护作业表，然后实现 Ctrl-Z、`jobs`、`fg`、`bg`。
