#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/random.h>
#include <sys/resource.h>

#define PG_DATA "/data/pgdata"
#define PG_RUN "/run/postgresql"
#define PG_BIN "/usr/lib/postgresql/18/bin"
#define PG_UID 100
#define PG_GID 102

void append_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, content, strlen(content));
    close(fd);
}

int main() {
    write(1, "init:dev\n", 9);
    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, "");
    
    write(1, "init:proc\n", 10);
    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", 0, "");

    write(1, "init:sysfs\n", 11);
    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, "");

    /* Fix entropy for PostgreSQL getrandom() */
    int rfd = open("/dev/urandom", O_RDWR);
    if (rfd >= 0) {
        struct {
            int ent_count;
            int size;
            unsigned char data[32];
        } entropy = {
            .ent_count = 256,
            .size = 32,
            .data = "linuxsqllinuxsqllinuxsqllinuxsql" 
        };
        ioctl(rfd, RNDADDENTROPY, &entropy);
        close(rfd);
        write(1, "init:entropy OK\n", 16);
    }

    /* Mount persistent ext4 filesystem */
    write(1, "init:mount vda\n", 15);
    mkdir("/mnt", 0755);
    if (mount("/dev/vda", "/mnt", "ext4", 0, "") != 0) {
        perror("mount /dev/vda");
        write(1, "Falling back to tmpfs for /mnt...\n", 34);
        mount("tmpfs", "/mnt", "tmpfs", 0, "");
    }

    /* Create essentials */
    mkdir("/mnt/dev", 0755);
    mkdir("/mnt/proc", 0755);
    mkdir("/mnt/sys", 0755);

    /* Enforce 8MB stack limit via GLIBC ABI which inherits down correctly */
    struct rlimit rl;
    rl.rlim_cur = 8 * 1024 * 1024;
    rl.rlim_max = 8 * 1024 * 1024;
    if (setrlimit(RLIMIT_STACK, &rl) == 0) {
        write(1, "init:stack lim OK\n", 18);
    }

    /* chroot into the Debian rootfs */
    write(1, "init:chroot\n", 12);
    if (chroot("/mnt") != 0 || chdir("/") != 0) {
        write(1, "init:chroot FAIL\n", 17);
        perror("chroot");
        goto setup_env;
    }

    /* Mount essential filesystems inside the new root */
    mount("udev", "/dev", "devtmpfs", 0, "");
    mkdir("/dev/shm", 0777);
    mount("tmpfs", "/dev/shm", "tmpfs", 0, "");
    
    mount("proc", "/proc", "proc", 0, "");
    mount("sysfs", "/sys", "sysfs", 0, "");
    mkdir("/run", 0755);
    mount("tmpfs", "/run", "tmpfs", 0, "");
    write(1, "init:rootfs OK\n", 15);

setup_env:
    sethostname("linuxsql-vm", 12);
    write(1, "init:host\n", 10);

    /* Fix SHMMAX */
    int fd = open("/proc/sys/kernel/shmmax", O_WRONLY);
    if (fd >= 0) {
        write(fd, "268435456\n", 10); // 256MB
        close(fd);
        write(1, "init:shmmax OK\n", 15);
    }

    mkdir(PG_RUN, 0755);
    chown(PG_RUN, PG_UID, PG_GID);

    if (access("/data", F_OK) != 0) mkdir("/data", 0755);
    chown("/data", PG_UID, PG_GID);

    if (access("/data/pglog", F_OK) != 0) mkdir("/data/pglog", 0755);
    chown("/data/pglog", PG_UID, PG_GID);

    if (access(PG_DATA "/PG_VERSION", F_OK) != 0) {
        printf("=== First boot: initializing PostgreSQL ===\n");
        pid_t pid = fork();
        if (pid == 0) {
            syscall(144, PG_GID);
            syscall(146, PG_UID);
            fprintf(stderr, "initdb child: uid=%d gid=%d\n", getuid(), getgid());
            setenv("TZ", "UTC", 1);
            execl(PG_BIN "/initdb", PG_BIN "/initdb", "-D", PG_DATA, "--no-locale", "--auth=trust", "--username=postgres", NULL);
            _exit(1);
        }
        int st;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            printf("=== initdb complete ===\n");
            append_file(PG_DATA "/postgresql.conf",
                "listen_addresses = '*'\n"
                "port = 5432\n"
                "unix_socket_directories = '" PG_RUN "'\n"
                "shared_buffers = 64MB\n"
                "max_connections = 100\n"
                "log_destination = 'stderr'\n"
                "logging_collector = off\n"
            );
        } else {
            printf("=== initdb FAILED (exit %d) ===\n", WEXITSTATUS(st));
        }
    } else {
        printf("=== PG_VERSION found. Skipping initdb ===\n");
    }

    /* Start PostgreSQL */
    printf("=== Starting PostgreSQL ===\n");
    pid_t pid = fork();
    if (pid == 0) {
        syscall(144, PG_GID);
        syscall(146, PG_UID);
        setenv("TZ", "UTC", 1);
        execl(PG_BIN "/pg_ctl", PG_BIN "/pg_ctl", "start", "-D", PG_DATA, "-l", "/data/pglog/pg.log", NULL);
        _exit(1);
    }
    int st;
    waitpid(pid, &st, 0);

    printf("\n=====================================\n");
    printf("  LinuxSQL RISC-V VM - PG 18.3 Ready\n");
    printf("=====================================\n\n");

    setenv("PS1", "linuxsql# ", 1);
    setenv("PATH", "/usr/lib/postgresql/18/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
    setenv("PAGER", "cat", 1);
    setenv("PGDATA", "/data/pgdata", 1);
    execl("/bin/dash", "-dash", NULL); /* argv[0] starts with '-' = login shell */
    perror("exec dash");
    while(1) sleep(100);
    return 0;
}
