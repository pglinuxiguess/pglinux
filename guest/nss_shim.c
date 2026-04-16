/*
 * nss_shim.c — NSS bypass for static glibc binaries.
 *
 * glibc's getpwuid()/getpwnam() try to dlopen("libnss_files.so")
 * which fails in static binaries, causing NULL returns.  This shim
 * overrides those functions and reads /etc/passwd directly.
 *
 * Link this BEFORE libc (-lc) so our symbols take precedence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

static struct passwd pw_result;
static char pw_buf[1024];

static struct passwd *parse_passwd_line(char *line) {
    char *fields[7];
    int i;
    char *p;

    /* Split on ':' — format: name:passwd:uid:gid:gecos:home:shell */
    fields[0] = line;
    p = line;
    for (i = 1; i < 7 && p; i++) {
        p = strchr(p, ':');
        if (p) {
            *p++ = '\0';
            fields[i] = p;
        } else {
            fields[i] = "";
        }
    }
    if (i < 7) return NULL;

    pw_result.pw_name   = fields[0];
    pw_result.pw_passwd = fields[1];
    pw_result.pw_uid    = (uid_t)atoi(fields[2]);
    pw_result.pw_gid    = (gid_t)atoi(fields[3]);
    pw_result.pw_gecos  = fields[4];
    pw_result.pw_dir    = fields[5];
    pw_result.pw_shell  = fields[6];

    /* Strip trailing newline from shell */
    char *nl = strchr(pw_result.pw_shell, '\n');
    if (nl) *nl = '\0';

    return &pw_result;
}

struct passwd *getpwuid(uid_t uid) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return NULL;

    while (fgets(pw_buf, sizeof(pw_buf), f)) {
        char tmp[1024];
        strncpy(tmp, pw_buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        /* Quick check: find 3rd field (uid) */
        char *p = strchr(tmp, ':');
        if (!p) continue;
        p = strchr(p + 1, ':');
        if (!p) continue;
        uid_t this_uid = (uid_t)atoi(p + 1);
        if (this_uid == uid) {
            fclose(f);
            return parse_passwd_line(pw_buf);
        }
    }
    fclose(f);
    return NULL;
}

struct passwd *getpwnam(const char *name) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return NULL;

    while (fgets(pw_buf, sizeof(pw_buf), f)) {
        size_t nlen = strlen(name);
        if (strncmp(pw_buf, name, nlen) == 0 && pw_buf[nlen] == ':') {
            fclose(f);
            return parse_passwd_line(pw_buf);
        }
    }
    fclose(f);
    return NULL;
}

/* Reentrant versions — used by some PG code paths */
int getpwuid_r(uid_t uid, struct passwd *pwd,
               char *buf, size_t buflen, struct passwd **result) {
    struct passwd *p = getpwuid(uid);
    if (!p) { *result = NULL; return 0; }
    memcpy(pwd, p, sizeof(*pwd));
    *result = pwd;
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd,
               char *buf, size_t buflen, struct passwd **result) {
    struct passwd *p = getpwnam(name);
    if (!p) { *result = NULL; return 0; }
    memcpy(pwd, p, sizeof(*pwd));
    *result = pwd;
    return 0;
}
