// SPDX-License-Identifier: GPL-2.0
/*
 * pg_process.c — Host-side process management for LinuxSQL
 *
 * Implements the fork()+PTRACE_SYSCALL process execution model.
 * Each kernel "process" is a real host child managed via ptrace.
 *
 * This file includes HOST headers (glibc, ptrace, etc.) — NOT kernel
 * headers. Communication with the kernel side is through pg_console_write().
 *
 * Syscall interception strategy:
 *   We use PTRACE_SYSCALL which stops at both entry and exit.
 *   On ENTRY: decide if we handle it or let the host kernel do it.
 *     - Handle ourselves: set x8=-1 (nullify), kernel skips it.
 *     - Pass through: don't touch registers, kernel executes it.
 *   On EXIT: if we handled it, write our return value to x0.
 *
 * This is more reliable than PTRACE_SYSEMU because SYSEMU doesn't
 * support passthrough — once it intercepts, the syscall is dead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <elf.h>
#include <linux/ptrace.h>

/*
 * Console output — defined in the PG extension (linuxsql.c).
 * We use a distinct symbol name because the kernel's vmlinux.a also
 * exports a weak pg_console_write that the linker would prefer.
 */
extern void pg_ext_console_write(const char *buf, unsigned int len);

static void pg_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		pg_ext_console_write(buf, n);
}

/* ---- Child memory access ---- */

static ssize_t read_child_mem(pid_t pid, void *dst,
			      unsigned long src_addr, size_t len)
{
	struct iovec local = { .iov_base = dst, .iov_len = len };
	struct iovec remote = { .iov_base = (void *)src_addr, .iov_len = len };
	return process_vm_readv(pid, &local, 1, &remote, 1, 0);
}

static ssize_t write_child_mem(pid_t pid, unsigned long dst_addr,
			       const void *src, size_t len)
{
	struct iovec local = { .iov_base = (void *)src, .iov_len = len };
	struct iovec remote = { .iov_base = (void *)dst_addr, .iov_len = len };
	return process_vm_writev(pid, &local, 1, &remote, 1, 0);
}

/* ---- aarch64 register access ---- */

/*
 * aarch64 user registers from PTRACE_GETREGSET(NT_PRSTATUS).
 * 34 × 8 bytes = 272 bytes: x0-x30, sp, pc, pstate.
 */
struct aarch64_regs {
	unsigned long long x[31];
	unsigned long long sp;
	unsigned long long pc;
	unsigned long long pstate;
};

static int get_regs(pid_t pid, struct aarch64_regs *regs)
{
	struct iovec iov = { regs, sizeof(*regs) };
	return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
}

static int set_regs(pid_t pid, struct aarch64_regs *regs)
{
	struct iovec iov = { regs, sizeof(*regs) };
	return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
}

/* ---- Syscall numbers (aarch64, asm-generic/unistd.h) ---- */
#define SYS_getcwd          17
#define SYS_fcntl           25
#define SYS_ioctl           29
#define SYS_openat          56
#define SYS_close           57
#define SYS_read            63
#define SYS_write           64
#define SYS_writev          66
#define SYS_readlinkat      78
#define SYS_newfstatat      79
#define SYS_fstat           80
#define SYS_exit            93
#define SYS_exit_group      94
#define SYS_set_tid_address 96
#define SYS_set_robust_list 99
#define SYS_clock_gettime   113
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_uname           160
#define SYS_getpid          172
#define SYS_getppid         173
#define SYS_getuid          174
#define SYS_geteuid         175
#define SYS_getgid          176
#define SYS_getegid         177
#define SYS_gettid          178
#define SYS_brk             214
#define SYS_munmap          215
#define SYS_mmap            222
#define SYS_mprotect        226
#define SYS_madvise         233
#define SYS_prlimit64       261
#define SYS_getrandom       278
#define SYS_rseq            293

/* Multi-process and fd management */
#define SYS_clone           220
#define SYS_execve          221
#define SYS_wait4           260
#define SYS_pipe2           59
#define SYS_dup3            24
#define SYS_ppoll           73
#define SYS_rt_sigreturn    139
#define SYS_sigaltstack     132
#define SYS_sched_getaffinity 123
#define SYS_sched_yield     124

/* ---- Syscall classification ---- */

/*
 * Classify a syscall at ENTRY time:
 *   PASSTHROUGH — let the host kernel execute it (brk, mmap, mprotect, etc.)
 *   HANDLED     — we handle it ourselves (write, uname, getpid, etc.)
 *   EXIT        — child wants to exit
 */
#define SC_PASSTHROUGH 0
#define SC_HANDLED     1
#define SC_EXIT        2

/* Saved state from entry for use at exit */
static int entry_action;
static long entry_nr;
static long handled_retval;
/* Saved x0-x2 from entry (args may be clobbered by kernel at exit) */
static unsigned long long saved_x0, saved_x1, saved_x2;

/*
 * handle_entry — called at syscall-entry stop.
 *
 * Returns the action: PASSTHROUGH, HANDLED, or EXIT.
 * For HANDLED, sets x8=-1 to nullify the kernel's execution
 * and saves the return value in handled_retval.
 */
static int handle_entry(pid_t pid, struct aarch64_regs *r)
{
	long nr = (long)r->x[8];
	entry_nr = nr;

	/* Save args — kernel may clobber them during passthrough */
	saved_x0 = r->x[0];
	saved_x1 = r->x[1];
	saved_x2 = r->x[2];

	switch (nr) {
	/*
	 * PASSTHROUGH: let the host kernel handle these.
	 * Don't touch registers — the kernel will execute the real syscall
	 * and we'll read the result at exit.
	 */
	case SYS_brk:
	case SYS_mmap:
	case SYS_munmap:
	case SYS_mprotect:
	case SYS_madvise:
	case SYS_set_tid_address:
	case SYS_set_robust_list:
	case SYS_rt_sigaction:
	case SYS_rt_sigprocmask:
	case SYS_prlimit64:
	case SYS_rseq:
	case SYS_getrandom:
	case SYS_clock_gettime:
	case SYS_close:
	case SYS_read:         /* stdin EOF handled differently if needed */
	case SYS_readlinkat:
	case SYS_newfstatat:
	case SYS_fstat:
	case SYS_ioctl:
	case SYS_fcntl:
	case SYS_getcwd:
	case SYS_openat:
	case SYS_clone:
	case SYS_execve:
	case SYS_wait4:
	case SYS_pipe2:
	case SYS_dup3:
	case SYS_ppoll:
	case SYS_rt_sigreturn:
	case SYS_sigaltstack:
	case SYS_sched_getaffinity:
	case SYS_sched_yield:
		return SC_PASSTHROUGH;

	/* EXIT: child wants to quit */
	case SYS_exit_group:
	case SYS_exit:
		handled_retval = (long)r->x[0];
		return SC_EXIT;

	/*
	 * HANDLED: we intercept these. Nullify the kernel's execution
	 * by setting x8=-1, then we'll set x0 at exit.
	 */
	case SYS_write: {
		int fd = (int)r->x[0];
		unsigned long buf_addr = r->x[1];
		size_t count = (size_t)r->x[2];

		if (fd == 1 || fd == 2) {
			/* Console output — read from child, write to boot log */
			if (count > 4096) count = 4096;
			char buf[4096];
			ssize_t n = read_child_mem(pid, buf, buf_addr, count);
			if (n > 0) {
				pg_ext_console_write(buf, n);
				handled_retval = n;
			} else {
				handled_retval = -14; /* -EFAULT */
			}
		} else {
			/* Other fds — pass through to host */
			return SC_PASSTHROUGH;
		}
		/* Nullify so kernel doesn't execute */
		r->x[8] = (unsigned long long)-1;
		set_regs(pid, r);
		return SC_HANDLED;
	}

	case SYS_writev: {
		int fd = (int)r->x[0];
		if (fd == 1 || fd == 2) {
			unsigned long iov_addr = r->x[1];
			int iovcnt = (int)r->x[2];
			long total = 0;
			for (int i = 0; i < iovcnt && i < 16; i++) {
				unsigned long iov_base, iov_len;
				read_child_mem(pid, &iov_base,
					       iov_addr + i * 16, 8);
				read_child_mem(pid, &iov_len,
					       iov_addr + i * 16 + 8, 8);
				if (iov_len > 0 && iov_len <= 4096) {
					char buf[4096];
					read_child_mem(pid, buf, iov_base, iov_len);
					pg_ext_console_write(buf, iov_len);
					total += iov_len;
				}
			}
			handled_retval = total;
		} else {
			return SC_PASSTHROUGH;
		}
		r->x[8] = (unsigned long long)-1;
		set_regs(pid, r);
		return SC_HANDLED;
	}

	case SYS_uname: {
		/* Write our custom utsname into child memory */
		struct {
			char sysname[65];
			char nodename[65];
			char release[65];
			char version[65];
			char machine[65];
			char domainname[65];
		} uts;
		memset(&uts, 0, sizeof(uts));
		strncpy(uts.sysname, "Linux", 64);
		strncpy(uts.nodename, "postgresql", 64);
		strncpy(uts.release, "6.1.0-postgres", 64);
		strncpy(uts.version, "LinuxSQL on PostgreSQL", 64);
		strncpy(uts.machine, "aarch64", 64);
		strncpy(uts.domainname, "(none)", 64);
		write_child_mem(pid, r->x[0], &uts, sizeof(uts));
		handled_retval = 0;
		r->x[8] = (unsigned long long)-1;
		set_regs(pid, r);
		return SC_HANDLED;
	}

	/* ID syscalls — return our custom values */
	case SYS_getpid:  handled_retval = 1; break;
	case SYS_getppid: handled_retval = 0; break;
	case SYS_gettid:  handled_retval = 1; break;
	case SYS_getuid:  handled_retval = 0; break;
	case SYS_geteuid: handled_retval = 0; break;
	case SYS_getgid:  handled_retval = 0; break;
	case SYS_getegid: handled_retval = 0; break;

	default:
		/* Unknown syscall — let kernel try it */
		return SC_PASSTHROUGH;
	}

	/* Common path for simple HANDLED syscalls */
	r->x[8] = (unsigned long long)-1;
	set_regs(pid, r);
	return SC_HANDLED;
}

/*
 * handle_exit — called at syscall-exit stop.
 *
 * For HANDLED syscalls, writes our return value into x0.
 * For PASSTHROUGH, the kernel already set x0 — nothing to do.
 */
static void handle_exit(pid_t pid, struct aarch64_regs *r)
{
	if (entry_action == SC_HANDLED) {
		r->x[0] = (unsigned long long)handled_retval;
		set_regs(pid, r);
	}
	/* For PASSTHROUGH, x0 already has the kernel's result */
}

/* ---- Main syscall loop ---- */

static int pg_syscall_loop(pid_t root_pid)
{
	struct aarch64_regs regs;
	int status;
	int exit_code = -1;


	/*
	 * Resume the root process to start it running.
	 * All subsequent resumes happen at the bottom of the loop.
	 */
	if (ptrace(PTRACE_SYSCALL, root_pid, 0, 0) < 0) {
		pg_log("[ptrace] initial SYSCALL failed: %s\n", strerror(errno));
		return -1;
	}

	while (1) {
		pid_t pid = waitpid(-1, &status, __WALL);
		if (pid < 0) {
			if (errno == ECHILD)
				break;
			pg_log("[ptrace] waitpid failed: %s\n", strerror(errno));
			break;
		}

		if (WIFEXITED(status)) {
			if (pid == root_pid) {
				exit_code = WEXITSTATUS(status);
				break;
			}
			continue; /* non-root child exited, ignore */
		}

		if (WIFSIGNALED(status)) {
			if (pid == root_pid) {
				exit_code = 128 + WTERMSIG(status);
				break;
			}
			continue;
		}

		if (!WIFSTOPPED(status))
			continue;

		int sig = WSTOPSIG(status);
		int event = (status >> 16) & 0xff;

		/* Handle ptrace events from clone/fork/vfork/exec */
		if (event) {
			if (event == PTRACE_EVENT_CLONE ||
			    event == PTRACE_EVENT_FORK ||
			    event == PTRACE_EVENT_VFORK) {
				/*
				 * New child auto-attached by ptrace.
				 * Get its pid and resume both parent and child.
				 */
				unsigned long new_pid;
				ptrace(PTRACE_GETEVENTMSG, pid, 0, &new_pid);


				/* Resume the new child */
				ptrace(PTRACE_SYSCALL, (pid_t)new_pid, 0, 0);
			}
			/* Resume the parent (or exec'd process) */
			ptrace(PTRACE_SYSCALL, pid, 0, 0);
			continue;
		}

		/* Syscall stop has bit 7 set (PTRACE_O_TRACESYSGOOD) */
		if (sig == (SIGTRAP | 0x80)) {
			struct ptrace_syscall_info sci;
			if (ptrace(PTRACE_GET_SYSCALL_INFO,
				   pid, sizeof(sci), &sci) < 0) {
				/* Process may have exited between wait and here */
				continue;
			}

			get_regs(pid, &regs);

			if (sci.op == PTRACE_SYSCALL_INFO_ENTRY) {
				entry_action = handle_entry(pid, &regs);

				if (entry_action == SC_EXIT) {
					if (pid == root_pid) {
						exit_code = (int)handled_retval;
						kill(pid, SIGKILL);
						waitpid(pid, &status, 0);
						break;
					}
					kill(pid, SIGKILL);
					waitpid(pid, &status, 0);
					continue;
				}
			} else if (sci.op == PTRACE_SYSCALL_INFO_EXIT) {
				handle_exit(pid, &regs);
			}

		} else if (sig == SIGSEGV || sig == SIGBUS) {
			get_regs(pid, &regs);
			pg_log("[crash] pid %d signal %d at PC=0x%llx\n",
			       pid, sig, regs.pc);
			if (pid == root_pid) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				exit_code = 128 + sig;
				break;
			}
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			continue;

		} else {
			/*
			 * Other signal — deliver to child and continue.
			 * Use PTRACE_SYSCALL with the signal number to inject it.
			 */
			ptrace(PTRACE_SYSCALL, pid, 0, sig);
			continue;
		}

		/* Resume this process */
		ptrace(PTRACE_SYSCALL, pid, 0, 0);
	}

	return exit_code;
}

/* ---- Entry point ---- */

/*
 * pg_run_cmd — fork a child, exec busybox with the given command,
 * and run the syscall interception loop.
 *
 * Returns the child's exit code.
 */
int pg_run_cmd(const char *cmd)
{

	pid_t pid = fork();
	if (pid < 0) {
		pg_log("arch/postgres: fork failed: %s\n", strerror(errno));
		return -1;
	}

	if (pid == 0) {
		/* ---- Child process ---- */
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);

		/*
		 * Use the host's /bin/sh for full access to installed
		 * packages (X11, coreutils, etc.)
		 */
		char *argv[] = {
			"/bin/sh", "-c",
			(char *)cmd,
			NULL
		};
		char *envp[] = {
			"HOME=/",
			"PATH=/usr/local/bin:/usr/bin:/bin:/work/initramfs/bin",
			"TERM=linux",
			"DISPLAY=host.docker.internal:0",
			NULL
		};
		execve("/bin/sh", argv, envp);
		_exit(127);
	}

	/* ---- Parent process ---- */
	int status;

	waitpid(pid, &status, 0);
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
		pg_log("arch/postgres: child didn't stop (status=0x%x)\n", status);
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return -1;
	}

	ptrace(PTRACE_SETOPTIONS, pid, 0,
	       PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL |
	       PTRACE_O_TRACEEXEC | PTRACE_O_TRACECLONE |
	       PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK);

	ptrace(PTRACE_CONT, pid, 0, 0);
	waitpid(pid, &status, 0);

	if (WIFEXITED(status)) {
		pg_log("arch/postgres: child exited immediately (code %d)\n",
		       WEXITSTATUS(status));
		return WEXITSTATUS(status);
	}

	if (WIFSTOPPED(status)) {
		int event = (status >> 16) & 0xff;
		if (event != PTRACE_EVENT_EXEC) {
			pg_log("arch/postgres: unexpected stop (sig=%d event=%d)\n",
			       WSTOPSIG(status), event);
		}
	}

	int ret = pg_syscall_loop(pid);
	return ret;
}

/*
 * pg_run_init — called from arch_call_rest_init() during kernel boot.
 * Runs the default init sequence.
 */
int pg_run_init(void)
{
	return pg_run_cmd(
		"echo '=== LinuxSQL init ==='; "
		"echo 'Hello from inside PostgreSQL!'; "
		"echo 'PID 1 running as busybox ash'; "
		"echo '=== init done ==='"
	);
}

