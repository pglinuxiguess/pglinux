/*
 * extension/lds_symbols.c — Linker section boundary symbols
 *
 * When vmlinux is built as a standalone binary, vmlinux.lds defines these.
 * When building as a shared library (PG extension), we provide them as
 * plain C symbols. On macOS (Mach-O), section attributes with dots in
 * the name are invalid, so we just define these as regular zero-sized
 * arrays. They serve as address markers for the kernel's iteration macros.
 */
#include <stddef.h>

/* Type aliases to avoid kernel header dependencies */
typedef void (*initcall_t)(void);
typedef unsigned long ulong;

/*
 * Section boundary markers.
 * Each pair defines an empty range — the RISC-V kernel emulator references
 * these symbols but they're unused on the host side.
 */

/* Text section markers */
char _stext[0];
char _etext[0];
char _sinittext[0];
char _einittext[0];
char _sdata[0];
char _edata[0];

/* BSS */
char __bss_start[0];
char __bss_stop[0];

/* Read-only data */
char __start_rodata[0];
char __end_rodata[0];

/* Init region */
char __init_begin[0];
char __init_end[0];

/* Special text sections */
char __cpuidle_text_start[0];
char __cpuidle_text_end[0];
char __irqentry_text_start[0];
char __irqentry_text_end[0];
char __softirqentry_text_start[0];
char __softirqentry_text_end[0];
char __sched_text_start[0];
char __sched_text_end[0];

/*
 * Initcall levels — the kernel walks these during boot.
 * Objects with __initcall(fn, level) go into .initcallN.init sections.
 * We define start markers for each level + the overall range.
 */
initcall_t __initcall_start[0];
initcall_t __initcall0_start[0];
initcall_t __initcall1_start[0];
initcall_t __initcall2_start[0];
initcall_t __initcall3_start[0];
initcall_t __initcall4_start[0];
initcall_t __initcall5_start[0];
initcall_t __initcall6_start[0];
initcall_t __initcall7_start[0];
initcall_t __initcall_end[0];

/* Console initcalls */
initcall_t __con_initcall_start[0];
initcall_t __con_initcall_end[0];

/* Exception table */
char __start___ex_table[0];
char __stop___ex_table[0];

/* Module version table */
char __start___modver[0];
char __stop___modver[0];

/* Kernel parameters */
char __start___param[0];
char __stop___param[0];

/* Setup data */
char __setup_start[0];
char __setup_end[0];

/* Firmware */
char __start_builtin_fw[0];
char __end_builtin_fw[0];

/*
 * Scheduler class ordering is handled by linuxsql.lds.
 * __sched_class_highest and __sched_class_lowest are defined there.
 */

/*
 * jiffies — defined by kernel/time/timer.o as jiffies_64.
 * We don't define it here — the kernel provides it.
 * The vmlinux.lds alias (jiffies = jiffies_64) doesn't work in .so,
 * so we define jiffies as a weak symbol pointing into jiffies_64.
 */

/* Init stack */
char init_stack[8192] __attribute__((aligned(8192)));
