/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rv64_cpu.h — RISC-V RV64IMA CPU emulator for LinuxSQL
 *
 * PostgreSQL IS the computer. This emulator provides:
 *   - RV64I base integer instructions
 *   - M extension (multiply/divide)
 *   - A extension (atomics: LR/SC, AMO)
 *   - Privileged modes: M, S, U
 *   - Sv39 virtual memory
 *   - Trap/interrupt handling
 *
 * The emulator runs a standard Linux kernel compiled for RISC-V.
 */
#ifndef RV64_CPU_H
#define RV64_CPU_H

/* Forward declaration for JIT state */
struct jit_state;

// #define DISABLE_JIT 1

#include <stdint.h>
#include "rv64_virtio.h"
#include <stddef.h>

/* ---- Privilege levels ---- */
#define PRIV_U  0   /* User */
#define PRIV_S  1   /* Supervisor (kernel) */
#define PRIV_M  3   /* Machine (firmware/SBI) */

/* ---- CSR addresses ---- */

/* Machine-level CSRs */
#define CSR_MSTATUS     0x300
#define CSR_MISA        0x301
#define CSR_MEDELEG     0x302
#define CSR_MIDELEG     0x303
#define CSR_MIE         0x304
#define CSR_MTVEC       0x305
#define CSR_MCOUNTEREN  0x306
#define CSR_MSCRATCH    0x340
#define CSR_MEPC        0x341
#define CSR_MCAUSE      0x342
#define CSR_MTVAL       0x343
#define CSR_MIP         0x344
#define CSR_MHARTID     0xF14
#define CSR_MVENDORID   0xF11
#define CSR_MARCHID     0xF12
#define CSR_MIMPID      0xF13

/* Machine memory protection (stubbed) */
#define CSR_PMPCFG0     0x3A0
#define CSR_PMPADDR0    0x3B0

/* Supervisor-level CSRs */
#define CSR_SSTATUS     0x100
#define CSR_SIE         0x104
#define CSR_STVEC       0x105
#define CSR_SCOUNTEREN  0x106
#define CSR_SSCRATCH    0x140
#define CSR_SEPC        0x141
#define CSR_SCAUSE      0x142
#define CSR_STVAL       0x143
#define CSR_SIP         0x144
#define CSR_SATP        0x180

/* Counter/timer CSRs */
#define CSR_CYCLE       0xC00
#define CSR_TIME        0xC01
#define CSR_INSTRET     0xC02

/* ---- mstatus / sstatus bit fields ---- */
#define MSTATUS_SIE     (1ULL << 1)
#define MSTATUS_MIE     (1ULL << 3)
#define MSTATUS_SPIE    (1ULL << 5)
#define MSTATUS_MPIE    (1ULL << 7)
#define MSTATUS_SPP     (1ULL << 8)
#define MSTATUS_MPP     (3ULL << 11)
#define MSTATUS_FS      (3ULL << 13)
#define MSTATUS_SUM     (1ULL << 18)
#define MSTATUS_MXR     (1ULL << 19)
#define MSTATUS_TVM     (1ULL << 20)
#define MSTATUS_TW      (1ULL << 21)
#define MSTATUS_TSR     (1ULL << 22)
#define MSTATUS_UXL     (3ULL << 32)
#define MSTATUS_SXL     (3ULL << 34)
#define MSTATUS_SD      (1ULL << 63)

/* sstatus is a view of mstatus with only S-mode visible bits */
#define SSTATUS_MASK    (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | \
                         MSTATUS_FS | MSTATUS_SUM | MSTATUS_MXR | \
                         MSTATUS_UXL | MSTATUS_SD)

/* ---- Interrupt and exception cause codes ---- */

/* Interrupts (bit 63 set) */
#define IRQ_S_SOFT      (1ULL | (1ULL << 63))
#define IRQ_M_SOFT      (3ULL | (1ULL << 63))
#define IRQ_S_TIMER     (5ULL | (1ULL << 63))
#define IRQ_M_TIMER     (7ULL | (1ULL << 63))
#define IRQ_S_EXT       (9ULL | (1ULL << 63))
#define IRQ_M_EXT       (11ULL | (1ULL << 63))

/* Interrupt pending/enable bit positions */
#define MIP_SSIP        (1ULL << 1)
#define MIP_MSIP        (1ULL << 3)
#define MIP_STIP        (1ULL << 5)
#define MIP_MTIP        (1ULL << 7)
#define MIP_SEIP        (1ULL << 9)
#define MIP_MEIP        (1ULL << 11)

/* Exceptions (bit 63 clear) */
#define EXC_INSN_MISALIGN    0
#define EXC_INSN_ACCESS      1
#define EXC_ILLEGAL_INSN     2
#define EXC_BREAKPOINT       3
#define EXC_LOAD_MISALIGN    4
#define EXC_LOAD_ACCESS      5
#define EXC_STORE_MISALIGN   6
#define EXC_STORE_ACCESS     7
#define EXC_ECALL_U          8
#define EXC_ECALL_S          9
#define EXC_ECALL_M          11
#define EXC_INSN_PAGE_FAULT  12
#define EXC_LOAD_PAGE_FAULT  13
#define EXC_STORE_PAGE_FAULT 15

/* ---- Memory access types (for MMU) ---- */
#define ACCESS_READ     0
#define ACCESS_WRITE    1
#define ACCESS_EXEC     2

/* ---- Sv39 page table ---- */
#define SATP_MODE_BARE  0ULL
#define SATP_MODE_SV39  8ULL
#define SATP_MODE_SHIFT 60

#define PTE_V   (1ULL << 0)  /* Valid */
#define PTE_R   (1ULL << 1)  /* Readable */
#define PTE_W   (1ULL << 2)  /* Writable */
#define PTE_X   (1ULL << 3)  /* Executable */
#define PTE_U   (1ULL << 4)  /* User-accessible */
#define PTE_G   (1ULL << 5)  /* Global */
#define PTE_A   (1ULL << 6)  /* Accessed */
#define PTE_D   (1ULL << 7)  /* Dirty */

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1ULL << PAGE_SHIFT)
#define PAGE_MASK   (PAGE_SIZE - 1)

/* ---- Memory map (QEMU virt compatible) ---- */
#define CLINT_BASE      0x02000000ULL
#define CLINT_SIZE      0x00010000ULL
#define CLINT_MTIMECMP  0x4000  /* offset within CLINT */
#define CLINT_MTIME     0xBFF8  /* offset within CLINT */

#define PLIC_BASE       0x0C000000ULL
#define PLIC_SIZE       0x04000000ULL

#define UART0_BASE      0x10000000ULL
#define UART0_SIZE      0x00001000ULL
#define UART0_IRQ       10

/* Virtio MMIO devices (QEMU virt layout: 0x10001000+) */
#define VIRTIO_MMIO_BASE  0x10001000ULL
#define VIRTIO_MMIO_SIZE  0x00001000ULL  /* per device */
#define VIRTIO_MMIO_COUNT 2              /* blk, net */
#define VIRTIO_BLK_IRQ    8             /* PLIC IRQ for virtio-blk */
#define VIRTIO_NET_IRQ    9             /* PLIC IRQ for virtio-net */

#define RAM_BASE        0x80000000ULL
#define RAM_SIZE_DEFAULT (256ULL * 1024 * 1024)  /* 256 MB */

/* ---- Device structures ---- */

struct rv64_uart {
	uint8_t  rbr;           /* receive buffer */
	uint8_t  thr;           /* transmit holding (write-only) */
	uint8_t  ier;           /* interrupt enable */
	uint8_t  iir;           /* interrupt ident (read-only) */
	uint8_t  lcr;           /* line control */
	uint8_t  mcr;           /* modem control */
	uint8_t  lsr;           /* line status */
	uint8_t  msr;           /* modem status */
	uint8_t  scr;           /* scratch */
	uint8_t  dll;           /* divisor latch low */
	uint8_t  dlm;           /* divisor latch high */
	int      rx_ready;      /* nonzero if rbr has data */
	int      thre_pending;  /* TX holding register empty interrupt pending */

	/* Output callback */
	void (*tx_callback)(uint8_t ch, void *opaque);
	void *tx_opaque;

	/* Input buffer (ring) */
	uint8_t  rx_buf[256];
	int      rx_head;
	int      rx_tail;
};

struct rv64_clint {
	uint64_t mtimecmp;
	uint64_t mtime;
};

struct rv64_plic {
	/*
	 * PLIC for single hart, 2 contexts (M=0, S=1).
	 * Per QEMU virt layout:
	 *   Enable ctx 0: 0x002000, ctx 1: 0x002080
	 *   Threshold/claim ctx 0: 0x200000, ctx 1: 0x201000
	 */
	uint32_t priority[64];     /* source priority */
	uint32_t pending[2];       /* pending bits (64 sources) */
	uint32_t enable[2][2];     /* [ctx][word] enable bits */
	uint32_t threshold[2];     /* [ctx] priority threshold */
	uint32_t claim[2];         /* [ctx] last claimed IRQ */
};

/* ---- LR/SC reservation ---- */
struct rv64_reservation {
	int      valid;
	uint64_t addr;
};

/* ---- TLB (Translation Lookaside Buffer) ---- */
#define TLB_SIZE       4096
#define TLB_INDEX_MASK (TLB_SIZE - 1)

struct tlb_entry {
	uint64_t vpn;      /* virtual page number (vaddr >> 12) — full tag */
	uint64_t ppn;      /* physical page number (pa >> 12) */
	uint8_t *host_base; /* host pointer: ram + (ppn<<12) - RAM_BASE, or NULL for MMIO */
	uint8_t  perm;     /* permission bits: PTE_R | PTE_W | PTE_X | PTE_U */
	uint8_t  level;    /* page level: 0=4K, 1=2M, 2=1G */
	uint8_t  dirty;    /* PTE_D already set in page table */
	uint8_t  valid;    /* entry is populated */
};

/* ---- CPU state ---- */

struct rv64_cpu {
	/* General-purpose registers (x0 is hardwired to 0) */
	uint64_t x[32];
	uint64_t pc;

	/* Floating-point registers (F/D extension) */
	uint64_t f[32];     /* stored as raw bits: f32 in low 32, f64 in all 64 */
	uint32_t fcsr;      /* FP control/status: fflags[4:0] | frm[7:5] */

	/* Current privilege level */
	uint8_t priv;

	/* Machine-level CSRs */
	uint64_t mstatus;
	uint64_t misa;
	uint64_t medeleg;
	uint64_t mideleg;
	uint64_t mie;
	uint64_t mtvec;
	uint64_t mcounteren;
	uint64_t mscratch;
	uint64_t mepc;
	uint64_t mcause;
	uint64_t mtval;
	uint64_t mip;

	/* Supervisor-level CSRs */
	uint64_t satp;
	uint64_t sscratch;
	uint64_t sepc;
	uint64_t scause;
	uint64_t stval;
	uint64_t stvec;
	uint64_t scounteren;
	uint64_t stimecmp;    /* SSTC extension: S-mode timer compare */
	uint64_t menvcfg;     /* Machine environment config (controls SSTC) */

	/* Counters */
	uint64_t cycle;
	uint64_t instret;

	/* PMP (stubbed — just store values to not crash) */
	uint64_t pmpcfg[4];
	uint64_t pmpaddr[16];

	/* Atomics: LR/SC reservation */
	struct rv64_reservation reservation;

	/* TLB cache — indexed by VPN[0] */
	struct tlb_entry tlb[TLB_SIZE];

	/* Devices */
	struct rv64_uart  uart;
	struct rv64_clint clint;
	struct rv64_plic  plic;

	/* MMIO Devices */
	struct virtio_mmio_dev virtio_blk;   /* Device 0 */
	struct virtio_mmio_dev virtio_net;   /* Device 1 */
	struct virtio_mmio_dev *virtio_devs[VIRTIO_MMIO_COUNT];

	/* VM Identity */
	uint64_t id;

	/* Debug / stats */
	uint64_t ram_size;

	/* JIT compiler state (NULL if JIT disabled) */
	struct jit_state *jit;

	/* Execution state */
	uint8_t pending_irq_check;
	int halted;    /* WFI or fatal */
	int error;     /* nonzero on fatal error */

	/* Run loop exit reason — set on every rv64_run return */
	enum {
		RUN_EXIT_NONE = 0,       /* hasn't run yet */
		RUN_EXIT_BUDGET = 1,     /* cycle budget exhausted normally */
		RUN_EXIT_ERROR = 2,      /* rv64_step returned -1 */
		RUN_EXIT_DEBUG_HALT = 3, /* dbg.debug_halt was set */
		RUN_EXIT_WFI_TIMEOUT = 4,/* halted (WFI) and budget ran out */
	} run_exit_reason;

	/* ---- Debug instrumentation ---- */
	struct {
		/* Memory watchpoint: log stores that touch [addr, addr+size) */
		uint64_t watch_addr;   /* guest virtual address (0 = disabled) */
		uint32_t watch_size;   /* byte range to watch */
		int      watch_active; /* nonzero when armed */

		/* PC breakpoint: log register state when PC hits target */
		uint64_t break_pc;     /* guest PC (0 = disabled) */
		int      break_active; /* nonzero when armed */
		int      break_halt;   /* if set, halt VM on breakpoint hit */

		/* Debug halt: set by breakpoint or vm_step_until.
		 * Causes rv64_run to return immediately. Cleared by vm_resume(). */
		int      debug_halt;

		/* Store-hit ring buffer: captures stores into watched region */
#define DBG_STORE_LOG_SIZE 64
		struct {
			uint64_t pc;       /* PC of the store instruction */
			uint64_t addr;     /* target address */
			uint64_t value;    /* value written */
			uint32_t size;     /* 1/2/4/8 bytes */
			uint64_t instret;  /* instruction count at time of store */
		} store_log[DBG_STORE_LOG_SIZE];
		int store_log_count;   /* total hits (wraps ring) */

		/* Breakpoint-hit snapshot: register dump at break */
#define DBG_BREAK_LOG_SIZE 16
		struct {
			uint64_t pc;
			uint64_t x[32];    /* integer registers */
			uint64_t f[32];    /* FP registers (raw bits) */
			uint64_t sp;       /* x2 for convenience */
			uint64_t instret;
		} break_log[DBG_BREAK_LOG_SIZE];
		int break_log_count;

		/* Console-instret log: records instret at each newline output */
#define DBG_CONSOLE_LOG_SIZE 256
		struct {
			uint64_t instret;  /* instret when this newline was written */
			int      console_pos; /* position in console buffer */
		} console_log[DBG_CONSOLE_LOG_SIZE];
		int console_log_count;

		/* Trap exception log: ring buffer of recent exception traps.
		 * Only exceptions (bit 63 clear) — interrupts are excluded to
		 * avoid flooding with timer ticks. For ECALL traps, a7/a0-a2
		 * capture the syscall number and first three arguments. */
#define DBG_TRAP_LOG_SIZE 4096
		struct {
			uint64_t cause;    /* exception cause code */
			uint64_t tval;     /* trap value (faulting addr for page faults) */
			uint64_t pc;       /* PC at time of trap */
			uint64_t instret;  /* instruction count */
			uint8_t  priv;     /* privilege level before trap */
			/* Syscall/register context (useful for ECALL traps) */
			uint64_t a7;       /* x[17] — syscall number for ECALL */
			uint64_t a0;       /* x[10] — arg0 / return value */
			uint64_t a1;       /* x[11] — arg1 */
			uint64_t a2;       /* x[12] — arg2 */
			/* Filled at SRET: return value + return PC.
			 * If ret_pc != pc+4, kernel redirected to signal handler. */
			uint64_t ret_a0;   /* a0 at SRET (syscall return value) */
			uint64_t ret_pc;   /* sepc at SRET (where kernel returns to) */
		} trap_log[DBG_TRAP_LOG_SIZE];
		int trap_log_count;
		int trap_log_last_slot;  /* slot of most recent entry, for SRET backfill */

		/* Instruction log: circular buffer of last N instructions executed */
#define DBG_INST_LOG_SIZE 4096
		struct {
			uint64_t pc;
			uint32_t insn;
			uint64_t instret;
		} inst_log[DBG_INST_LOG_SIZE];
		int inst_log_idx;
	} dbg;

	/* ---- Performance profiling counters ---- */
	struct {
		/* Time breakdown (cycle counts) */
		uint64_t exec_cycles;     /* cycles in instruction execution */
		uint64_t wfi_cycles;      /* cycles spent idle in WFI */

		/* Interrupt stats */
		uint64_t irq_checks;      /* times check_interrupts called */
		uint64_t irq_taken;       /* times an interrupt was delivered */

		/* MMU stats */
		uint64_t mmu_walks;       /* full page table walks (TLB misses) */
		uint64_t mmu_walk_steps;  /* individual PTE reads (3 per walk) */
		uint64_t tlb_hits;        /* TLB cache hits */
		uint64_t tlb_misses;      /* TLB cache misses (== mmu_walks) */

		/* Instruction class distribution */
		uint64_t insn_alu;        /* R-type / I-type arithmetic */
		uint64_t insn_load;       /* load instructions */
		uint64_t insn_store;      /* store instructions */
		uint64_t insn_branch;     /* conditional branches */
		uint64_t insn_jump;       /* JAL, JALR */
		uint64_t insn_csr;        /* CSR read/write */
		uint64_t insn_fpu;        /* floating point */
		uint64_t insn_atomic;     /* AMO, LR, SC */
		uint64_t insn_system;     /* ECALL, EBREAK, WFI, xRET */
		uint64_t insn_other;      /* everything else */

		/* Virtio I/O stats */
		uint64_t blk_reads;       /* read requests */
		uint64_t blk_writes;      /* write requests */
		uint64_t blk_bytes_read;
		uint64_t blk_bytes_written;

		/* JIT Performance Sampling (Flamegraphs) */
#define DBG_PERF_LOG_SIZE 65536
		struct {
			uint64_t pc;
			uint64_t ra;
			uint64_t fp;
		} perf_samples[DBG_PERF_LOG_SIZE];
		uint32_t perf_sample_idx;
		uint32_t perf_sample_tick;
	} prof;
};

/* ---- Public API ---- */

/* Initialize CPU to reset state */
void rv64_init(struct rv64_cpu *cpu, uint8_t *ram, uint64_t ram_size);

/* Deliver a trap from external code (e.g., FPU handler, JIT thunks) */
void rv64_trap(struct rv64_cpu *cpu, uint64_t cause, uint64_t tval);

/* Execute one instruction. Returns 0 normally, -1 on fatal error. */
int rv64_step(struct rv64_cpu *cpu);

/* Run for up to `max_cycles` cycles. Returns cycles executed. */
uint64_t rv64_run(struct rv64_cpu *cpu, uint64_t max_cycles);

/* Push a byte into the UART receive buffer */
void rv64_uart_rx(struct rv64_cpu *cpu, uint8_t ch);

/* Flush the TLB (call on satp write or sfence.vma) */
void rv64_tlb_flush(struct rv64_cpu *cpu);

/* ---- Bus interface (used by CPU, implemented in rv64_bus.c) ---- */

/* Returns 0 on success, sets *val. Returns -1 on fault. */
int bus_load(struct rv64_cpu *cpu, uint64_t addr, int size, uint64_t *val);
int bus_store(struct rv64_cpu *cpu, uint64_t addr, int size, uint64_t val);

/* ---- MMU (implemented in rv64_mmu.c) ---- */

/*
 * Translate virtual address to physical address via Sv39 page tables.
 * Returns 0 on success (phys written to *pa).
 * Returns the exception code on fault.
 */
int rv64_translate(struct rv64_cpu *cpu, uint64_t vaddr, int access_type,
                   uint64_t *pa);

/* ---- FPU (implemented in rv64_fpu.c) ---- */

/*
 * Execute a floating-point instruction (opcodes 0x07, 0x27, 0x43-0x4F, 0x53).
 * Returns 0 on success, -1 if unimplemented.
 */
int rv64_fp_exec(struct rv64_cpu *cpu, uint32_t insn);

/* ---- JIT helpers (called by JIT-generated code) ---- */
int rv64_jit_load(struct rv64_cpu *cpu, uint64_t vaddr, int size,
                  uint64_t *val);
int rv64_jit_store(struct rv64_cpu *cpu, uint64_t vaddr, int size,
                   uint64_t val);

int rv64_cpu_load(struct rv64_cpu *cpu, uint64_t vaddr, int size, uint64_t *val, int access);

#endif /* RV64_CPU_H */
