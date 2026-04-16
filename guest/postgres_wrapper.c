#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/resource.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--check") == 0) return 0;

    struct rlimit rl;
    rl.rlim_cur = 8 * 1024 * 1024;
    rl.rlim_max = 8 * 1024 * 1024;
    if (setrlimit(RLIMIT_STACK, &rl) != 0) {
        perror("postgres_wrapper: setrlimit");
    }

    char **newargv = malloc((argc + 1) * sizeof(char*));
    newargv[0] = "/usr/lib/postgresql/18/bin/postgres.real";
    for (int i=1; i<argc; i++) newargv[i] = argv[i];
    newargv[argc] = NULL;
    
    execv(newargv[0], newargv);
    return 1;
}
