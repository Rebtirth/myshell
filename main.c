#include <stdio.h>
#include <string.h>

int main(void) {
    char line[256];
    while (1) {
        printf("myshell> ");
        if (fgets(line, sizeof(line), stdin) == NULL) break;
         char *token = strtok(line, " \t\n");
         int i = 0;
         while (token != NULL) {
         printf("token[%d] = %s\n", i, token);
         i++;
    token = strtok(NULL, " \t\n");
}
    }
    return 0;
}

