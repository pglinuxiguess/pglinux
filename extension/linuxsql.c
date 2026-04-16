/*
 * linuxsql.c — PostgreSQL extension that boots a Linux kernel
 *
 * This extension loads the Linux 6.1 kernel compiled for ARCH=postgres
 * and executes start_kernel() inside the PostgreSQL backend process.
 * Kernel printk output is routed to PostgreSQL NOTICE messages.
 *
 * IMPORTANT: This file includes ONLY PostgreSQL headers.
 * Kernel headers have conflicting types (bool, int64_t, fd_set, etc.)
 * and must never be included here. All kernel interaction goes through
 * the clean C function boundary declared below.
 *
 * Usage:
 *   CREATE EXTENSION linuxsql;
 *   SELECT linux.boot();
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "miscadmin.h"

#include <sys/mman.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <execinfo.h>
#include <unistd.h>

static sigjmp_buf crash_jmp;
static volatile int in_kernel_boot = 0;

/*
 * Crash handler — catches SIGSEGV during kernel boot.
 * Instead of killing the PG backend, we longjmp back to the
 * boot function and report what happened.
 */
static void crash_handler(int sig, siginfo_t *info, void *ucontext)
{
	if (in_kernel_boot) {
		char msg[512];
		snprintf(msg, sizeof(msg),
			"CRASH: signal %d at addr %p, PC %p",
			sig, info->si_addr,
			ucontext ? (void *)((ucontext_t *)ucontext)->uc_mcontext->__ss.__pc : NULL);
		write(STDERR_FILENO, msg, strlen(msg));
		write(STDERR_FILENO, "\n", 1);

		/* Print backtrace */
		void *bt[32];
		int nframes = backtrace(bt, 32);
		backtrace_symbols_fd(bt, nframes, STDERR_FILENO);

		siglongjmp(crash_jmp, sig);
	}
}

PG_MODULE_MAGIC;

/*
 * Forward declarations — these are defined in the kernel (vmlinux.a).
 * We declare them here with plain C types to avoid including kernel headers.
 */
extern void start_kernel(void);

/*
 * The kernel's 'current' macro dereferences pg_current_task.
 * It's in BSS (NULL) at load time, so we must point it at init_task
 * before calling start_kernel() or the first printk will SIGSEGV.
 *
 * init_task is the static boot task_struct defined in init/init_task.c.
 * pg_current_task is defined in arch/postgres/kernel/setup.c.
 */
struct task_struct;  /* opaque — we don't include kernel headers */
extern struct task_struct init_task;
extern struct task_struct *pg_current_task;

/*
 * Guest physical memory pool.
 * We mmap a contiguous anonymous region that the kernel uses as "RAM".
 */
#define GUEST_MEM_SIZE (64UL * 1024 * 1024)  /* 64 MB */
static void *guest_memory = NULL;

/*
 * Boot log capture.
 * The kernel console driver (console.c) calls pg_console_write()
 * which appends to this buffer so we can return the boot log.
 */
#define BOOT_LOG_SIZE (256 * 1024)  /* 256 KB */
char *pg_boot_log = NULL;
int   pg_boot_log_pos = 0;

/*
 * pg_console_write — called by the kernel's console driver.
 * NOT static — the kernel's console.c references this symbol.
 */
void pg_console_write(const char *buf, unsigned int len)
{
	/* Write directly to stderr for immediate visibility in PG log */
	write(STDERR_FILENO, buf, len);

	/* Append to boot log buffer */
	if (pg_boot_log && pg_boot_log_pos + (int)len < BOOT_LOG_SIZE) {
		memcpy(pg_boot_log + pg_boot_log_pos, buf, len);
		pg_boot_log_pos += len;
		pg_boot_log[pg_boot_log_pos] = '\0';
	}
}

/*
 * pg_ext_console_write — same as pg_console_write, but with a unique
 * symbol name so pg_process.c can call it without the linker resolving
 * to the kernel's weak no-op pg_console_write in vmlinux.a.
 */
void pg_ext_console_write(const char *buf, unsigned int len)
{
	write(STDERR_FILENO, buf, len);
	if (pg_boot_log && pg_boot_log_pos + (int)len < BOOT_LOG_SIZE) {
		memcpy(pg_boot_log + pg_boot_log_pos, buf, len);
		pg_boot_log_pos += len;
		pg_boot_log[pg_boot_log_pos] = '\0';
	}
}

/*
 * Exported symbols that the kernel's arch/postgres code calls.
 * Defined here — the kernel has weak defaults that we override.
 */
void *linuxsql_guest_memory(void) { return guest_memory; }
unsigned long linuxsql_guest_mem_size(void) { return GUEST_MEM_SIZE; }

/*
 * linux_boot — SQL-callable function
 *
 * Allocates guest memory, boots the kernel, returns boot log.
 */
PG_FUNCTION_INFO_V1(linux_boot);
Datum
linux_boot(PG_FUNCTION_ARGS)
{
	/* Allocate guest physical memory */
	if (!guest_memory) {
		guest_memory = mmap(NULL, GUEST_MEM_SIZE,
				    PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (guest_memory == MAP_FAILED)
			elog(ERROR, "linuxsql: failed to mmap %lu bytes for guest RAM",
			     GUEST_MEM_SIZE);
		elog(NOTICE, "linuxsql: allocated %lu MB guest RAM at %p",
		     GUEST_MEM_SIZE / (1024*1024), guest_memory);
	}

	/* Point 'current' at the static boot task before the kernel touches it */
	pg_current_task = &init_task;

	/* Allocate boot log buffer */
	if (!pg_boot_log) {
		pg_boot_log = palloc0(BOOT_LOG_SIZE);
		pg_boot_log_pos = 0;
	}

	/* Boot the kernel — with crash protection */
	elog(NOTICE, "linuxsql: calling start_kernel()...");

	/* Install crash handler */
	struct sigaction sa, old_sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, &old_sa);
	sigaction(SIGBUS, &sa, NULL);

	int crash_sig = sigsetjmp(crash_jmp, 1);
	if (crash_sig == 0) {
		in_kernel_boot = 1;
		start_kernel();
		in_kernel_boot = 0;
		/* Restore original handler */
		sigaction(SIGSEGV, &old_sa, NULL);
	} else {
		in_kernel_boot = 0;
		sigaction(SIGSEGV, &old_sa, NULL);

		/* Append crash info to boot log */
		if (pg_boot_log) {
			int n = snprintf(pg_boot_log + pg_boot_log_pos,
				BOOT_LOG_SIZE - pg_boot_log_pos,
				"\n*** KERNEL CRASH: signal %d ***\n", crash_sig);
			pg_boot_log_pos += n;
		}
		elog(WARNING, "linuxsql: kernel crashed with signal %d", crash_sig);
	}

	/* Return the captured boot log */
	PG_RETURN_TEXT_P(cstring_to_text(pg_boot_log));
}

/*
 * linux_exec — SQL-callable function: linux.exec(cmd TEXT)
 *
 * Runs a shell command via busybox inside the ptrace sandbox.
 * Does NOT require a full kernel boot — just forks and execs.
 * Returns the command's stdout/stderr output as TEXT.
 */
extern int pg_run_cmd(const char *cmd);

PG_FUNCTION_INFO_V1(linux_exec);
Datum
linux_exec(PG_FUNCTION_ARGS)
{
	text *cmd_text = PG_GETARG_TEXT_PP(0);
	char *cmd = text_to_cstring(cmd_text);

	/* Allocate output capture buffer (reuse boot log infrastructure) */
	if (!pg_boot_log) {
		pg_boot_log = palloc0(BOOT_LOG_SIZE);
	}
	pg_boot_log_pos = 0;
	pg_boot_log[0] = '\0';

	int exit_code = pg_run_cmd(cmd);

	/*
	 * Append exit code to output if non-zero.
	 * This lets the caller see if the command failed.
	 */
	if (exit_code != 0) {
		int n = snprintf(pg_boot_log + pg_boot_log_pos,
			BOOT_LOG_SIZE - pg_boot_log_pos,
			"\n[exit code: %d]\n", exit_code);
		if (n > 0) pg_boot_log_pos += n;
	}

	pfree(cmd);
	PG_RETURN_TEXT_P(cstring_to_text(pg_boot_log));
}
