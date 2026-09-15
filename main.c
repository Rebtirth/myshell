#include <stdio.h>
#include <string.h>

int main(void) {
    char line[256];
    while (1) {
        printf("myshell> ");
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        printf("你输入了: %s", line);
    }
    return 0;
}

