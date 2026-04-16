/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rv64_cpu.c — RISC-V RV64IMA CPU emulator for LinuxSQL
 *
 * This file IS the CPU. PostgreSQL IS the computer.
 *
 * Implements:
 *   - RV64I base integer instruction set
 *   - M extension (multiply/divide)
 *   - A extension (atomics: LR.W/D, SC.W/D, AMO*.W/D)
 *   - Privileged architecture: M/S/U modes, CSRs, traps, interrupts
 *   - Sv39 virtual memory (via rv64_mmu.c)
 *
 * The main entry point is rv64_run(), which executes instructions
 * in a tight loop, checking for interrupts periodically.
 */
#include "rv64_cpu.h"
#include "rv64_jit.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

/*
 * Real-time boot anchor. Set by rv64_pgext.c at vm_boot() / vm_resume().
 * We drive clint.mtime from CLOCK_MONOTONIC so that guest sleep durations
 * track real wall-clock time, not emulated instruction throughput.
 */
extern uint64_t vm_boot_time_ns;

/* ---- Helpers ---- */

/* Sign-extend a value from bit position `b` */
static inline int64_t sign_extend(uint64_t val, int b)
{
	uint64_t m = 1ULL << (b - 1);
	return (int64_t)((val ^ m) - m);
}

/* x0 is always zero — enforce after every write */
#define WR(rd, val) do { cpu->x[rd] = (val); cpu->x[0] = 0; } while(0)

/* ---- Memory access through MMU ---- */

/*
 * Fast-path load: check TLB for host pointer, read directly from RAM.
 * Falls back to full translate + bus_load on miss or MMIO.
 */
int rv64_cpu_load(struct rv64_cpu *cpu, uint64_t vaddr, int size,
                    uint64_t *val, int access)
{
	uint64_t vpn = vaddr >> PAGE_SHIFT;
	unsigned int idx = vpn & TLB_INDEX_MASK;
	struct tlb_entry *te = &cpu->tlb[idx];

	if (__builtin_expect(te->valid && te->vpn == vpn && te->host_base, 1)) {
		/* TLB hit on RAM page — check read/exec permission */
		int ok;
		if (access == ACCESS_EXEC)
			ok = (te->perm & PTE_X);
		else
			ok = (te->perm & PTE_R) ||
			     ((cpu->mstatus & MSTATUS_MXR) && (te->perm & PTE_X));

		if (__builtin_expect(ok, 1)) {
			/* Privilege check */
			if (cpu->priv == PRIV_U && !(te->perm & PTE_U))
				goto slow_path;
			if (cpu->priv == PRIV_S && (te->perm & PTE_U) &&
			    !(cpu->mstatus & MSTATUS_SUM))
				goto slow_path;

			uint64_t offset = vaddr & PAGE_MASK;
			/* Bail to slow path if access crosses page boundary */
			if (__builtin_expect(offset + size > PAGE_SIZE, 0))
				goto slow_path;
			
			/* Reconstruct physical address from cached PPN + page offset */
			cpu->prof.tlb_hits++;
			const uint8_t *p = te->host_base + offset;
			switch (size) {
			case 1: *val = *p; return 0;
			case 2: { uint16_t v; __builtin_memcpy(&v, p, 2); *val = v; return 0; }
			case 4: { uint32_t v; __builtin_memcpy(&v, p, 4); *val = v; return 0; }
			case 8: { uint64_t v; __builtin_memcpy(&v, p, 8); *val = v; return 0; }
			}
		}
	}

slow_path:;
	/* If this access crosses a page boundary, do it byte-by-byte
	 * so each byte gets its own translation */
	if (__builtin_expect(((vaddr & PAGE_MASK) + size) > PAGE_SIZE, 0)) {
		uint64_t result = 0;
		for (int i = 0; i < size; i++) {
			uint64_t pa, byte_val;
			int exc = rv64_translate(cpu, vaddr + i, access, &pa);
			if (exc) return exc;
			if (bus_load(cpu, pa, 1, &byte_val) < 0)
				return (access == ACCESS_EXEC) ? EXC_INSN_ACCESS : EXC_LOAD_ACCESS;
			result |= (byte_val & 0xFF) << (i * 8);
		}
		*val = result;
		return 0;
	}
	uint64_t pa;
	int exc = rv64_translate(cpu, vaddr, access, &pa);
	if (exc)
		return exc;
	if (bus_load(cpu, pa, size, val) < 0)
		return (access == ACCESS_EXEC) ? EXC_INSN_ACCESS : EXC_LOAD_ACCESS;
	return 0;
}

/*
 * Fast-path store: TLB hit on RAM page → direct write.
 * Falls back for MMIO, TLB miss, or dirty-bit propagation.
 */
static inline int cpu_store(struct rv64_cpu *cpu, uint64_t vaddr, int size,
                     uint64_t val)
{
	/* ---- Watchpoint check ---- */
	if (__builtin_expect(cpu->dbg.watch_active, 0)) {
		uint64_t wa = cpu->dbg.watch_addr;
		uint32_t ws = cpu->dbg.watch_size;
		/* Check overlap: [vaddr, vaddr+size) ∩ [wa, wa+ws) */
		if (vaddr < wa + ws && vaddr + size > wa) {
			int slot = cpu->dbg.store_log_count % DBG_STORE_LOG_SIZE;
			cpu->dbg.store_log[slot].pc = cpu->pc;
			cpu->dbg.store_log[slot].addr = vaddr;
			cpu->dbg.store_log[slot].value = val;
			cpu->dbg.store_log[slot].size = size;
			cpu->dbg.store_log[slot].instret = cpu->instret;
			cpu->dbg.store_log_count++;
		}
	}

	uint64_t vpn = vaddr >> PAGE_SHIFT;
	unsigned int idx = vpn & TLB_INDEX_MASK;
	struct tlb_entry *te = &cpu->tlb[idx];

	if (__builtin_expect(te->valid && te->vpn == vpn && te->host_base, 1)) {
		if (__builtin_expect((te->perm & PTE_W) && te->dirty, 1)) {
			/* Privilege check */
			if (cpu->priv == PRIV_U && !(te->perm & PTE_U))
				goto store_slow;
			if (cpu->priv == PRIV_S && (te->perm & PTE_U) &&
			    !(cpu->mstatus & MSTATUS_SUM))
				goto store_slow;

			uint64_t offset = vaddr & PAGE_MASK;
			/* Bail to slow path if access crosses page boundary */
			if (__builtin_expect(offset + size > PAGE_SIZE, 0))
				goto store_slow;
			cpu->prof.tlb_hits++;
			uint8_t *p = te->host_base + offset;
			switch (size) {
			case 1: *p = (uint8_t)val; return 0;
			case 2: { uint16_t v = (uint16_t)val; __builtin_memcpy(p, &v, 2); return 0; }
			case 4: { uint32_t v = (uint32_t)val; __builtin_memcpy(p, &v, 4); return 0; }
			case 8: { __builtin_memcpy(p, &val, 8); return 0; }
			}
		}
	}

store_slow:;
	/* If this access crosses a page boundary, do it byte-by-byte
	 * so each byte gets its own translation */
	if (__builtin_expect(((vaddr & PAGE_MASK) + size) > PAGE_SIZE, 0)) {
		for (int i = 0; i < size; i++) {
			uint64_t pa;
			int exc = rv64_translate(cpu, vaddr + i, ACCESS_WRITE, &pa);
			if (exc) return exc;
			if (bus_store(cpu, pa, 1, (val >> (i * 8)) & 0xFF) < 0)
				return EXC_STORE_ACCESS;
		}
		return 0;
	}
	uint64_t pa;
	int exc = rv64_translate(cpu, vaddr, ACCESS_WRITE, &pa);
	if (exc)
		return exc;
	if (bus_store(cpu, pa, size, val) < 0)
		return EXC_STORE_ACCESS;
	return 0;
}

/*
 * JIT helper: load a value at vaddr with fault handling.
 * Returns 0 on success, exception code on fault.
 * Called by JIT-generated code via function pointer.
 *
 * noinline: address is captured at JIT init and embedded in
 * generated code — must remain stable through LTO.
 */
__attribute__((noinline, used))
int rv64_jit_load(struct rv64_cpu *cpu, uint64_t vaddr, int size, uint64_t *val)
{
	uint64_t vpn = vaddr >> 12;
	return rv64_cpu_load(cpu, vaddr, size, val, ACCESS_READ);
}


/*
 * JIT helper: store a value at vaddr with fault handling.
 * Returns 0 on success, exception code on fault.
 * Called by JIT-generated code via function pointer.
 */
__attribute__((noinline, used))
int rv64_jit_store(struct rv64_cpu *cpu, uint64_t vaddr, int size, uint64_t val)
{
	uint64_t vpn = vaddr >> 12;
	return cpu_store(cpu, vaddr, size, val);
}

/* ---- CSR access ---- */

/*
 * Read a CSR. S-mode CSRs return the appropriate view of M-mode
 * registers (sstatus is a subset of mstatus, etc.)
 */
static int csr_read(struct rv64_cpu *cpu, uint32_t csr, uint64_t *val)
{
	switch (csr) {
	/* Machine-level */
	case CSR_MSTATUS:    *val = cpu->mstatus; return 0;
	case CSR_MISA:       *val = cpu->misa; return 0;
	case CSR_MEDELEG:    *val = cpu->medeleg; return 0;
	case CSR_MIDELEG:    *val = cpu->mideleg; return 0;
	case CSR_MIE:        *val = cpu->mie; return 0;
	case CSR_MTVEC:      *val = cpu->mtvec; return 0;
	case CSR_MCOUNTEREN: *val = cpu->mcounteren; return 0;
	case CSR_MSCRATCH:   *val = cpu->mscratch; return 0;
	case CSR_MEPC:       *val = cpu->mepc; return 0;
	case CSR_MCAUSE:     *val = cpu->mcause; return 0;
	case CSR_MTVAL:      *val = cpu->mtval; return 0;
	case CSR_MIP:        *val = cpu->mip; return 0;
	case CSR_MHARTID:    *val = 0; return 0;
	case CSR_MVENDORID:  *val = 0; return 0;
	case CSR_MARCHID:    *val = 0; return 0;
	case CSR_MIMPID:     *val = 0; return 0;
	case 0x30A:          *val = cpu->menvcfg; return 0; /* menvcfg */

	/* Entropy source (seed CSR 0x015) — OpenSBI reads this for RNG */
	case 0x015: {
		/*
		 * Return format: [31:30] = ES (entropy status)
		 *   0b11 = fully seeded (ES16), bits [15:0] = entropy
		 * Use instret as a cheap PRNG.
		 */
		uint64_t ent = cpu->instret * 6364136223846793005ULL + 1442695040888963407ULL;
		*val = (3ULL << 30) | (ent & 0xFFFF);
		return 0;
	}

	/* PMP (return stored values, not enforced) */
	case CSR_PMPCFG0:    *val = cpu->pmpcfg[0]; return 0;
	case CSR_PMPCFG0+2:  *val = cpu->pmpcfg[1]; return 0;
	case CSR_PMPADDR0: case CSR_PMPADDR0+1: case CSR_PMPADDR0+2:
	case CSR_PMPADDR0+3: case CSR_PMPADDR0+4: case CSR_PMPADDR0+5:
	case CSR_PMPADDR0+6: case CSR_PMPADDR0+7: case CSR_PMPADDR0+8:
	case CSR_PMPADDR0+9: case CSR_PMPADDR0+10: case CSR_PMPADDR0+11:
	case CSR_PMPADDR0+12: case CSR_PMPADDR0+13: case CSR_PMPADDR0+14:
	case CSR_PMPADDR0+15:
		*val = cpu->pmpaddr[csr - CSR_PMPADDR0];
		return 0;

	/* Supervisor-level (views of M-mode registers) */
	case CSR_SSTATUS:    *val = cpu->mstatus & SSTATUS_MASK; return 0;
	case CSR_SIE:        *val = cpu->mie & cpu->mideleg; return 0;
	case CSR_STVEC:      *val = cpu->stvec; return 0;
	case CSR_SCOUNTEREN: *val = cpu->scounteren; return 0;
	case CSR_SSCRATCH:   *val = cpu->sscratch; return 0;
	case CSR_SEPC:       *val = cpu->sepc; return 0;
	case CSR_SCAUSE:     *val = cpu->scause; return 0;
	case CSR_STVAL:      *val = cpu->stval; return 0;
	case CSR_SIP:        *val = cpu->mip & cpu->mideleg; return 0;
	case CSR_SATP:       *val = cpu->satp; return 0;

	/* SSTC: stimecmp */
	case 0x14D:          *val = cpu->stimecmp; return 0;

	/* FP CSRs */
	case 0x001: *val = cpu->fcsr & 0x1F; return 0;       /* fflags */
	case 0x002: *val = (cpu->fcsr >> 5) & 0x7; return 0;  /* frm */
	case 0x003: *val = cpu->fcsr & 0xFF; return 0;        /* fcsr */

	/* Counters */
	case CSR_CYCLE:
	case CSR_INSTRET:    *val = cpu->instret; return 0;
	case CSR_TIME:       *val = cpu->clint.mtime; return 0;

	default:
		/* Unknown CSR — MUST trap */
		return -1;
	}
}

static int csr_write(struct rv64_cpu *cpu, uint32_t csr, uint64_t val)
{
	switch (csr) {
	case CSR_MSTATUS:    cpu->mstatus = val; return 0;
	case CSR_MISA:       return 0; /* read-only for us */
	case CSR_MEDELEG:    cpu->medeleg = val; return 0;
	case CSR_MIDELEG:    cpu->mideleg = val; return 0;
	case CSR_MIE:        cpu->mie = val; return 0;
	case CSR_MTVEC:      cpu->mtvec = val; return 0;
	case CSR_MCOUNTEREN: cpu->mcounteren = val; return 0;
	case CSR_MSCRATCH:   cpu->mscratch = val; return 0;
	case CSR_MEPC:       cpu->mepc = val; return 0;
	case CSR_MCAUSE:     cpu->mcause = val; return 0;
	case CSR_MTVAL:      cpu->mtval = val; return 0;
	case CSR_MIP:        cpu->mip = val; return 0;

	case CSR_PMPCFG0:    cpu->pmpcfg[0] = val; return 0;
	case CSR_PMPCFG0+2:  cpu->pmpcfg[1] = val; return 0;
	case CSR_PMPADDR0: case CSR_PMPADDR0+1: case CSR_PMPADDR0+2:
	case CSR_PMPADDR0+3: case CSR_PMPADDR0+4: case CSR_PMPADDR0+5:
	case CSR_PMPADDR0+6: case CSR_PMPADDR0+7: case CSR_PMPADDR0+8:
	case CSR_PMPADDR0+9: case CSR_PMPADDR0+10: case CSR_PMPADDR0+11:
	case CSR_PMPADDR0+12: case CSR_PMPADDR0+13: case CSR_PMPADDR0+14:
	case CSR_PMPADDR0+15:
		cpu->pmpaddr[csr - CSR_PMPADDR0] = val;
		return 0;

	case CSR_SSTATUS:
		cpu->mstatus = (cpu->mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK);
		return 0;
	case CSR_SIE:
		cpu->mie = (cpu->mie & ~cpu->mideleg) | (val & cpu->mideleg);
		return 0;
	case CSR_STVEC:      cpu->stvec = val; return 0;
	case CSR_SCOUNTEREN: cpu->scounteren = val; return 0;
	case CSR_SSCRATCH:   cpu->sscratch = val; return 0;
	case CSR_SEPC:       cpu->sepc = val; return 0;
	case CSR_SCAUSE:     cpu->scause = val; return 0;
	case CSR_STVAL:      cpu->stval = val; return 0;
	case CSR_SIP:
		cpu->mip = (cpu->mip & ~cpu->mideleg) | (val & cpu->mideleg);
		return 0;

	/* SSTC: stimecmp — writing clears STIP */
	case 0x14D:
		cpu->stimecmp = val;
		cpu->mip &= ~MIP_STIP;  /* clear pending timer interrupt */
		return 0;

	/* menvcfg — controls S-mode access to stimecmp etc */
	case 0x30A:
		cpu->menvcfg = val;
		return 0;
	case 0x31A:  /* menvcfgh (RV32 high half, ignored on RV64) */
		return 0;

	case CSR_SATP: {
		/*
		 * We only support Sv39 (mode 8) and Bare (mode 0).
		 * The kernel probes for Sv48/Sv57 by writing and reading
		 * back — reject unsupported modes by storing 0 (Bare).
		 */
		uint64_t mode = val >> SATP_MODE_SHIFT;
		if (mode == SATP_MODE_BARE || mode == SATP_MODE_SV39)
			cpu->satp = val;
		else
			cpu->satp = 0; /* reject: hardware doesn't support this mode */
		rv64_tlb_flush(cpu);
		return 0;
	}

	/* FP CSRs */
	case 0x001: cpu->fcsr = (cpu->fcsr & ~0x1F) | (val & 0x1F); return 0;
	case 0x002: cpu->fcsr = (cpu->fcsr & ~0xE0) | ((val & 0x7) << 5); return 0;
	case 0x003: cpu->fcsr = val & 0xFF; return 0;

	default:
		/* Unknown CSR — MUST trap */
		return -1;
	}
}

/* ---- Trap handling ---- */

/*
 * Take a trap (exception or interrupt).
 * Decides whether to trap to M-mode or S-mode based on delegation.
 */
static void trap(struct rv64_cpu *cpu, uint64_t cause, uint64_t tval)
{
	if (cause == EXC_ILLEGAL_INSN)
		fprintf(stderr, "rv64: TRAP EXC_ILLEGAL_INSN pc=0x%lx tval=0x%08x\n",
			(unsigned long)cpu->pc, (uint32_t)tval);
	int is_interrupt = (cause >> 63) & 1;
	uint64_t code = cause & 0x7FFFFFFFFFFFFFFFULL;
	int trap_to_s;

	/* Log exceptions to the trap ring buffer.
	 * Exclude interrupts (timer noise) and ECALL_S (SBI calls from kernel —
	 * these flood the buffer with ~200K timer set_timer calls). */
	if (!is_interrupt && cause != EXC_ECALL_S) {
		int slot = cpu->dbg.trap_log_count % DBG_TRAP_LOG_SIZE;
		cpu->dbg.trap_log[slot].cause   = cause;
		cpu->dbg.trap_log[slot].tval    = tval;
		cpu->dbg.trap_log[slot].pc      = cpu->pc;
		cpu->dbg.trap_log[slot].instret = cpu->instret;
		cpu->dbg.trap_log[slot].priv    = (uint8_t)cpu->priv;
		cpu->dbg.trap_log[slot].a7      = cpu->x[17];
		cpu->dbg.trap_log[slot].a0      = cpu->x[10];
		cpu->dbg.trap_log[slot].a1      = cpu->x[11];
		cpu->dbg.trap_log[slot].a2      = cpu->x[12];
		cpu->dbg.trap_log[slot].ret_a0  = 0;
		cpu->dbg.trap_log[slot].ret_pc  = 0;
		cpu->dbg.trap_log_last_slot = slot;
		cpu->dbg.trap_log_count++;
	}

	/*
	 * Determine trap target mode.
	 * Traps from M-mode always go to M-mode.
	 * Traps from S/U mode go to S if delegated, else M.
	 */
	if (cpu->priv == PRIV_M) {
		trap_to_s = 0;
	} else if (is_interrupt) {
		trap_to_s = (cpu->mideleg >> code) & 1;
	} else {
		trap_to_s = (cpu->medeleg >> code) & 1;
	}

	if (trap_to_s) {
		/* Trap to S-mode */
		cpu->sepc = cpu->pc;
		cpu->scause = cause;
		cpu->stval = tval;

		/* Save current interrupt enable, then disable */
		uint64_t sie = (cpu->mstatus >> 1) & 1;
		cpu->mstatus = (cpu->mstatus & ~MSTATUS_SPIE) |
		               (sie << 5);                      /* SPIE = old SIE */
		cpu->mstatus &= ~MSTATUS_SIE;                  /* SIE = 0 */

		/* Save previous privilege in SPP */
		cpu->mstatus = (cpu->mstatus & ~MSTATUS_SPP) |
		               ((uint64_t)cpu->priv << 8);

		cpu->priv = PRIV_S;

		/* Jump to stvec */
		uint64_t base = cpu->stvec & ~3ULL;
		uint64_t mode = cpu->stvec & 3;
		if (mode == 1 && is_interrupt)
			cpu->pc = base + code * 4; /* vectored */
		else
			cpu->pc = base; /* direct */
	} else {
		/* Trap to M-mode */
		cpu->mepc = cpu->pc;
		cpu->mcause = cause;
		cpu->mtval = tval;

		uint64_t mie = (cpu->mstatus >> 3) & 1;
		cpu->mstatus = (cpu->mstatus & ~MSTATUS_MPIE) |
		               (mie << 7);
		cpu->mstatus &= ~MSTATUS_MIE;

		cpu->mstatus = (cpu->mstatus & ~MSTATUS_MPP) |
		               ((uint64_t)cpu->priv << 11);

		cpu->priv = PRIV_M;

		uint64_t base = cpu->mtvec & ~3ULL;
		uint64_t mode = cpu->mtvec & 3;
		if (mode == 1 && is_interrupt)
			cpu->pc = base + code * 4;
		else
			cpu->pc = base;
	}
}

/* Public wrapper: allows rv64_fpu.c to deliver memory fault traps */
void rv64_trap(struct rv64_cpu *cpu, uint64_t cause, uint64_t tval)
{
	if (cause == EXC_ILLEGAL_INSN)
		fprintf(stderr, "rv64: ILLEGAL INSN trap pc=0x%lx insn=0x%08x\n",
			(unsigned long)cpu->pc, (uint32_t)tval);
	
	trap(cpu, cause, tval);
}

/* ---- Interrupt checking ---- */

/*
 * Check for pending interrupts and take one if enabled.
 * Returns 1 if an interrupt was taken, 0 otherwise.
 */
static int check_interrupts(struct rv64_cpu *cpu)
{
	cpu->prof.irq_checks++;

	/* Update timer interrupt pending bits */
	if (cpu->clint.mtime >= cpu->clint.mtimecmp)
		cpu->mip |= MIP_MTIP;
	else
		cpu->mip &= ~MIP_MTIP;

	/* SSTC: S-mode timer via stimecmp */
	if (cpu->clint.mtime >= cpu->stimecmp)
		cpu->mip |= MIP_STIP;
	/* Note: STIP is cleared when stimecmp is written */



	/* Check PLIC for external interrupts (S-mode context = 1) */
	int plic_pending = 0;
	for (int i = 0; i < 2; i++) {
		if (cpu->plic.pending[i] & cpu->plic.enable[1][i]) {
			plic_pending = 1;
			break;
		}
	}
	if (plic_pending)
		cpu->mip |= MIP_SEIP;
	else
		cpu->mip &= ~MIP_SEIP;

	/* Run network event loop — throttled to avoid select() syscall overhead.
	 * At ~1B insn/sec, every 100K cycles = ~10K polls/sec = sub-ms latency. */
	if ((cpu->cycle % 100000) == 0)
		virtio_net_poll(&cpu->virtio_net);

	uint64_t pending = cpu->mip & cpu->mie;
	if (pending == 0)
		return 0;

	/*
	 * Check if interrupts are globally enabled for current mode.
	 * M-mode: enabled if mstatus.MIE is set
	 * S-mode: always take M-level interrupts; take S-level if SIE set
	 * U-mode: always take any enabled interrupt
	 */
	uint64_t mie_bit = (cpu->mstatus >> 3) & 1;
	uint64_t sie_bit = (cpu->mstatus >> 1) & 1;

	/* Check interrupt priority: MEI > MSI > MTI > SEI > SSI > STI */
	struct { uint64_t mask; uint64_t cause; int to_m; } irqs[] = {
		{ MIP_MEIP, IRQ_M_EXT,   1 },
		{ MIP_MSIP, IRQ_M_SOFT,  1 },
		{ MIP_MTIP, IRQ_M_TIMER, 1 },
		{ MIP_SEIP, IRQ_S_EXT,   0 },
		{ MIP_SSIP, IRQ_S_SOFT,  0 },
		{ MIP_STIP, IRQ_S_TIMER, 0 },
	};

	for (int i = 0; i < 6; i++) {
		if (!(pending & irqs[i].mask))
			continue;

		int take = 0;
		if (irqs[i].to_m) {
			/* M-level interrupt: not delegated */
			if (!(cpu->mideleg & irqs[i].mask)) {
				if (cpu->priv < PRIV_M ||
				    (cpu->priv == PRIV_M && mie_bit))
					take = 1;
			}
		}
		/* S-level interrupt or delegated M-level */
		if (!take && (cpu->mideleg & irqs[i].mask)) {
			if (cpu->priv < PRIV_S ||
			    (cpu->priv == PRIV_S && sie_bit))
				take = 1;
		}

		if (take) {
			trap(cpu, irqs[i].cause, 0);
			cpu->halted = 0; /* wake from WFI */
			cpu->prof.irq_taken++;
			return 1;
		}
	}

	return 0;
}

/* ---- Initialization ---- */

void rv64_init(struct rv64_cpu *cpu, uint8_t *ram, uint64_t ram_size)
{
	memset(cpu, 0, sizeof(*cpu));
	/* cpu->ram explicit assignment removed to respect process locality */
	cpu->ram_size = ram_size;

	/* Boot in M-mode at the reset vector */
	cpu->priv = PRIV_M;
	cpu->pc = RAM_BASE; /* OpenSBI loads here */

	/*
	 * misa: RV64IMAFDC
	 * MXL = 2 (64-bit) in bits [63:62]
	 */
	cpu->misa = (2ULL << 62) |  /* MXL = 64-bit */
	            (1 << 0)  |     /* A - Atomics */
	            (1 << 2)  |     /* C - Compressed */
	            (1 << 3)  |     /* D - Double-precision FP */
	            (1 << 5)  |     /* F - Single-precision FP */
	            (1 << 8)  |     /* I - Integer base */
	            (1 << 12) |     /* M - Multiply/Divide */
	            (1 << 18) |     /* S - Supervisor mode */
	            (1 << 20);      /* U - User mode */

	/* Set UXL and SXL to 64-bit, FS = Initial (01) in mstatus */
	cpu->mstatus = (2ULL << 32) | (2ULL << 34) | (1ULL << 13);

	/* UART: LSR starts with THR empty */
	cpu->uart.lsr = 0x60;

	/* Timer: don't fire until programmed */
	cpu->clint.mtimecmp = UINT64_MAX;
	cpu->stimecmp = UINT64_MAX;

	/* Initialize JIT compiler */
#ifndef DISABLE_JIT
	cpu->jit = calloc(1, sizeof(struct jit_state));
	if (cpu->jit) {
		if (jit_init(cpu->jit) < 0) {
			free(cpu->jit);
			cpu->jit = NULL;
			fprintf(stderr, "rv64: JIT init failed, running interpreter-only\n");
		} else {
			fprintf(stderr, "rv64: JIT compiler enabled\n");
		}
	}
#else
	cpu->jit = NULL;
	fprintf(stderr, "rv64: JIT disabled\n");
#endif
}

/* ---- Instruction execution ---- */

/*
 * rv64_step — Execute one instruction.
 *
 * Returns 0 on success, -1 on fatal error.
 * On recoverable faults (page fault, illegal insn), takes a trap internally.
 */
int rv64_step(struct rv64_cpu *cpu)
{
	if (cpu->halted)
		return 0;

	/* ---- PC breakpoint check ---- */
	if (__builtin_expect(cpu->dbg.break_active && cpu->pc == cpu->dbg.break_pc, 0)) {
		int slot = cpu->dbg.break_log_count % DBG_BREAK_LOG_SIZE;
		cpu->dbg.break_log[slot].pc = cpu->pc;
		memcpy(cpu->dbg.break_log[slot].x, cpu->x, sizeof(cpu->x));
		memcpy(cpu->dbg.break_log[slot].f, cpu->f, sizeof(cpu->f));
		cpu->dbg.break_log[slot].sp = cpu->x[2];
		cpu->dbg.break_log[slot].instret = cpu->instret;
		cpu->dbg.break_log_count++;
		if (cpu->dbg.break_halt)
			cpu->dbg.debug_halt = 1;
	}

	/* Fetch instruction — may be 16-bit (compressed) or 32-bit */
	uint64_t insn_val;
	int exc = rv64_cpu_load(cpu, cpu->pc, 4, &insn_val, ACCESS_EXEC);
	if (exc) {
		/* Try 2-byte fetch in case we're at end of page */
		int exc2 = rv64_cpu_load(cpu, cpu->pc, 2, &insn_val, ACCESS_EXEC);
		if (exc2) { trap(cpu, exc2, cpu->pc); return 0; }
		/* If it's a 32-bit instruction, it crosses into a faulting page. Trap. */
		if ((insn_val & 3) == 3) { trap(cpu, exc, cpu->pc); return 0; }
	}
	uint32_t insn = (uint32_t)insn_val;
	int compressed = 0;

	/* Log instruction */
	int slot = cpu->dbg.inst_log_idx % DBG_INST_LOG_SIZE;
	cpu->dbg.inst_log[slot].pc = cpu->pc;
	cpu->dbg.inst_log[slot].insn = insn;
	cpu->dbg.inst_log[slot].instret = cpu->instret;
	cpu->dbg.inst_log_idx++;



	/*
	 * Compressed instruction detection: bits[1:0] != 0b11.
	 * Expand C-extension 16-bit insns to their 32-bit equivalents.
	 */
	if ((insn & 3) != 3) {
		compressed = 1;
		uint16_t ci = (uint16_t)(insn & 0xFFFF);
		uint32_t exp = 0; /* expanded instruction */
		uint32_t quad = ci & 3;
		uint32_t funct3c = (ci >> 13) & 7;

		switch (quad) {
		case 0: /* Quadrant 0 */
			switch (funct3c) {
			case 0: { /* C.ADDI4SPN: addi rd', x2, nzuimm */
				uint32_t nzuimm = ((ci >> 5) & 1) << 3 |
				                  ((ci >> 6) & 1) << 2 |
				                  ((ci >> 7) & 0xF) << 6 |
				                  ((ci >> 11) & 3) << 4;
				uint32_t rd = ((ci >> 2) & 7) + 8;
				exp = (nzuimm << 20) | (2 << 15) | (0 << 12) | (rd << 7) | 0x13;
				break;
			}
			case 1: { /* C.FLD: fld fd', offset(rs1') */
				uint32_t off = ((ci >> 5) & 3) << 6 |
				               ((ci >> 10) & 7) << 3;
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				uint32_t fd = ((ci >> 2) & 7) + 8;
				/* Expand to FLD: opcode=0x07, funct3=3 */
				exp = (off << 20) | (rs1 << 15) | (3 << 12) | (fd << 7) | 0x07;
				break;
			}
			case 2: { /* C.LW: lw rd', offset(rs1') */
				uint32_t off = ((ci >> 5) & 1) << 6 |
				               ((ci >> 6) & 1) << 2 |
				               ((ci >> 10) & 7) << 3;
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				uint32_t rd = ((ci >> 2) & 7) + 8;
				exp = (off << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03;
				break;
			}
			case 3: { /* C.LD: ld rd', offset(rs1') */
				uint32_t off = ((ci >> 5) & 3) << 6 |
				               ((ci >> 10) & 7) << 3;
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				uint32_t rd = ((ci >> 2) & 7) + 8;
				exp = (off << 20) | (rs1 << 15) | (3 << 12) | (rd << 7) | 0x03;
				break;
			}
			case 6: { /* C.SW: sw rs2', offset(rs1') */
				uint32_t off = ((ci >> 5) & 1) << 6 |
				               ((ci >> 6) & 1) << 2 |
				               ((ci >> 10) & 7) << 3;
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				uint32_t rs2 = ((ci >> 2) & 7) + 8;
				uint32_t imm_11_5 = (off >> 5) & 0x7F;
				uint32_t imm_4_0 = off & 0x1F;
				exp = (imm_11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) | (imm_4_0 << 7) | 0x23;
				break;
			}
			case 5: { /* C.FSD: fsd fs2', offset(rs1') */
				uint32_t off = ((ci >> 5) & 3) << 6 |
				               ((ci >> 10) & 7) << 3;
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				uint32_t fs2 = ((ci >> 2) & 7) + 8;
				uint32_t imm_11_5 = (off >> 5) & 0x7F;
				uint32_t imm_4_0 = off & 0x1F;
				/* Expand to FSD: opcode=0x27, funct3=3 */
				exp = (imm_11_5 << 25) | (fs2 << 20) | (rs1 << 15) | (3 << 12) | (imm_4_0 << 7) | 0x27;
				break;
			}
			case 7: { /* C.SD: sd rs2', offset(rs1') */
				uint32_t off = ((ci >> 5) & 3) << 6 |
				               ((ci >> 10) & 7) << 3;
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				uint32_t rs2 = ((ci >> 2) & 7) + 8;
				uint32_t imm_11_5 = (off >> 5) & 0x7F;
				uint32_t imm_4_0 = off & 0x1F;
				exp = (imm_11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (3 << 12) | (imm_4_0 << 7) | 0x23;
				break;
			}
			default: goto illegal_compressed;
			}
			break;

		case 1: /* Quadrant 1 */
			switch (funct3c) {
			case 0: { /* C.NOP / C.ADDI */
				uint32_t rd_rs1 = (ci >> 7) & 0x1F;
				int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
				imm = (imm << 26) >> 26; /* sign-extend from bit 5 */
				exp = ((imm & 0xFFF) << 20) | (rd_rs1 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x13;
				break;
			}
			case 1: { /* C.ADDIW */
				uint32_t rd_rs1 = (ci >> 7) & 0x1F;
				int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
				imm = (imm << 26) >> 26;
				exp = ((imm & 0xFFF) << 20) | (rd_rs1 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x1B;
				break;
			}
			case 2: { /* C.LI: addi rd, x0, imm */
				uint32_t rd = (ci >> 7) & 0x1F;
				int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
				imm = (imm << 26) >> 26;
				exp = ((imm & 0xFFF) << 20) | (0 << 15) | (0 << 12) | (rd << 7) | 0x13;
				break;
			}
			case 3: { /* C.LUI / C.ADDI16SP */
				uint32_t rd = (ci >> 7) & 0x1F;
				if (rd == 2) {
					/* C.ADDI16SP: addi x2, x2, nzimm */
					int32_t imm = (((ci >> 12) & 1) << 9) |
					              (((ci >> 3) & 3) << 7) |
					              (((ci >> 5) & 1) << 6) |
					              (((ci >> 2) & 1) << 5) |
					              (((ci >> 6) & 1) << 4);
					imm = (imm << 22) >> 22; /* sign-extend from bit 9 */
					exp = ((imm & 0xFFF) << 20) | (2 << 15) | (0 << 12) | (2 << 7) | 0x13;
				} else {
					/* C.LUI: lui rd, nzimm */
					int32_t imm = (((ci >> 12) & 1) << 17) | (((ci >> 2) & 0x1F) << 12);
					imm = (imm << 14) >> 14; /* sign-extend from bit 17 */
					exp = (imm & 0xFFFFF000U) | (rd << 7) | 0x37;
				}
				break;
			}
			case 4: { /* C.SRLI / C.SRAI / C.ANDI / C.SUB / etc. */
				uint32_t funct2 = (ci >> 10) & 3;
				uint32_t rd_rs1 = ((ci >> 7) & 7) + 8;
				uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
				switch (funct2) {
				case 0: /* C.SRLI */
					exp = (shamt << 20) | (rd_rs1 << 15) | (5 << 12) | (rd_rs1 << 7) | 0x13;
					break;
				case 1: /* C.SRAI */
					exp = ((0x20 << 5 | shamt) << 20) | (rd_rs1 << 15) | (5 << 12) | (rd_rs1 << 7) | 0x13;
					break;
				case 2: { /* C.ANDI */
					int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
					imm = (imm << 26) >> 26;
					exp = ((imm & 0xFFF) << 20) | (rd_rs1 << 15) | (7 << 12) | (rd_rs1 << 7) | 0x13;
					break;
				}
				case 3: { /* C.SUB, C.XOR, C.OR, C.AND, C.SUBW, C.ADDW */
					uint32_t rs2 = ((ci >> 2) & 7) + 8;
					uint32_t funct1 = (ci >> 12) & 1;
					uint32_t funct2b = (ci >> 5) & 3;
					if (funct1 == 0) {
						switch (funct2b) {
						case 0: exp = (0x20 << 25) | (rs2 << 20) | (rd_rs1 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x33; break; /* SUB */
						case 1: exp = (rs2 << 20) | (rd_rs1 << 15) | (4 << 12) | (rd_rs1 << 7) | 0x33; break; /* XOR */
						case 2: exp = (rs2 << 20) | (rd_rs1 << 15) | (6 << 12) | (rd_rs1 << 7) | 0x33; break; /* OR */
						case 3: exp = (rs2 << 20) | (rd_rs1 << 15) | (7 << 12) | (rd_rs1 << 7) | 0x33; break; /* AND */
						}
					} else {
						switch (funct2b) {
						case 0: exp = (0x20 << 25) | (rs2 << 20) | (rd_rs1 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x3B; break; /* SUBW */
						case 1: exp = (rs2 << 20) | (rd_rs1 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x3B; break; /* ADDW */
						default: goto illegal_compressed;
						}
					}
					break;
				}
				}
				break;
			}
			case 5: { /* C.J: jal x0, offset */
				int32_t imm = (((ci >> 12) & 1) << 11) |
				              (((ci >> 11) & 1) << 4) |
				              (((ci >> 9) & 3) << 8) |
				              (((ci >> 8) & 1) << 10) |
				              (((ci >> 7) & 1) << 6) |
				              (((ci >> 6) & 1) << 7) |
				              (((ci >> 3) & 7) << 1) |
				              (((ci >> 2) & 1) << 5);
				imm = (imm << 20) >> 20; /* sign-extend from bit 11 */
				/* Encode as JAL x0, imm */
				uint32_t jimm = ((imm >> 20) & 1) << 31 |
				                ((imm >> 1) & 0x3FF) << 21 |
				                ((imm >> 11) & 1) << 20 |
				                ((imm >> 12) & 0xFF) << 12;
				exp = jimm | (0 << 7) | 0x6F;
				break;
			}
			case 6: { /* C.BEQZ: beq rs1', x0, offset */
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				int32_t off = (((ci >> 12) & 1) << 8) |
				              (((ci >> 10) & 3) << 3) |
				              (((ci >> 5) & 3) << 6) |
				              (((ci >> 3) & 3) << 1) |
				              (((ci >> 2) & 1) << 5);
				off = (off << 23) >> 23;
				uint32_t bimm = (((off >> 12) & 1) << 31) |
				                (((off >> 5) & 0x3F) << 25) |
				                (((off >> 1) & 0xF) << 8) |
				                (((off >> 11) & 1) << 7);
				exp = bimm | (0 << 20) | (rs1 << 15) | (0 << 12) | 0x63;
				break;
			}
			case 7: { /* C.BNEZ: bne rs1', x0, offset */
				uint32_t rs1 = ((ci >> 7) & 7) + 8;
				int32_t off = (((ci >> 12) & 1) << 8) |
				              (((ci >> 10) & 3) << 3) |
				              (((ci >> 5) & 3) << 6) |
				              (((ci >> 3) & 3) << 1) |
				              (((ci >> 2) & 1) << 5);
				off = (off << 23) >> 23;
				uint32_t bimm = (((off >> 12) & 1) << 31) |
				                (((off >> 5) & 0x3F) << 25) |
				                (((off >> 1) & 0xF) << 8) |
				                (((off >> 11) & 1) << 7);
				exp = bimm | (0 << 20) | (rs1 << 15) | (1 << 12) | 0x63;
				break;
			}
			}
			break;

		case 2: /* Quadrant 2 */
			switch (funct3c) {
			case 0: { /* C.SLLI */
				uint32_t rd_rs1 = (ci >> 7) & 0x1F;
				uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
				exp = (shamt << 20) | (rd_rs1 << 15) | (1 << 12) | (rd_rs1 << 7) | 0x13;
				break;
			}
			case 2: { /* C.LWSP: lw rd, offset(x2) */
				uint32_t rd = (ci >> 7) & 0x1F;
				uint32_t off = (((ci >> 12) & 1) << 5) |
				               (((ci >> 4) & 7) << 2) |
				               (((ci >> 2) & 3) << 6);
				exp = (off << 20) | (2 << 15) | (2 << 12) | (rd << 7) | 0x03;
				break;
			}
			case 1: { /* C.FLDSP: fld fd, offset(x2) */
				uint32_t fd = (ci >> 7) & 0x1F;
				uint32_t off = (((ci >> 12) & 1) << 5) |
				               (((ci >> 5) & 3) << 3) |
				               (((ci >> 2) & 7) << 6);
				exp = (off << 20) | (2 << 15) | (3 << 12) | (fd << 7) | 0x07;
				break;
			}
			case 3: { /* C.LDSP: ld rd, offset(x2) */
				uint32_t rd = (ci >> 7) & 0x1F;
				uint32_t off = (((ci >> 12) & 1) << 5) |
				               (((ci >> 5) & 3) << 3) |
				               (((ci >> 2) & 7) << 6);
				exp = (off << 20) | (2 << 15) | (3 << 12) | (rd << 7) | 0x03;
				break;
			}
			case 4: { /* C.JR / C.MV / C.JALR / C.ADD */
				uint32_t rd_rs1 = (ci >> 7) & 0x1F;
				uint32_t rs2 = (ci >> 2) & 0x1F;
				uint32_t bit12 = (ci >> 12) & 1;
				if (bit12 == 0) {
					if (rs2 == 0) {
						/* C.JR: jalr x0, 0(rs1) */
						exp = (rd_rs1 << 15) | (0 << 12) | (0 << 7) | 0x67;
					} else {
						/* C.MV: add rd, x0, rs2 */
						exp = (rs2 << 20) | (0 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x33;
					}
				} else {
					if (rs2 == 0 && rd_rs1 == 0) {
						/* C.EBREAK */
						exp = 0x00100073;
					} else if (rs2 == 0) {
						/* C.JALR: jalr x1, 0(rs1) */
						exp = (rd_rs1 << 15) | (0 << 12) | (1 << 7) | 0x67;
					} else {
						/* C.ADD: add rd, rd, rs2 */
						exp = (rs2 << 20) | (rd_rs1 << 15) | (0 << 12) | (rd_rs1 << 7) | 0x33;
					}
				}
				break;
			}
			case 6: { /* C.SWSP: sw rs2, offset(x2) */
				uint32_t rs2 = (ci >> 2) & 0x1F;
				uint32_t off = (((ci >> 9) & 0xF) << 2) |
				               (((ci >> 7) & 3) << 6);
				uint32_t imm_11_5 = (off >> 5) & 0x7F;
				uint32_t imm_4_0 = off & 0x1F;
				exp = (imm_11_5 << 25) | (rs2 << 20) | (2 << 15) | (2 << 12) | (imm_4_0 << 7) | 0x23;
				break;
			}
			case 5: { /* C.FSDSP: fsd fs2, offset(x2) */
				uint32_t fs2 = (ci >> 2) & 0x1F;
				uint32_t off = (((ci >> 10) & 7) << 3) |
				               (((ci >> 7) & 7) << 6);
				uint32_t imm_11_5 = (off >> 5) & 0x7F;
				uint32_t imm_4_0 = off & 0x1F;
				exp = (imm_11_5 << 25) | (fs2 << 20) | (2 << 15) | (3 << 12) | (imm_4_0 << 7) | 0x27;
				break;
			}
			case 7: { /* C.SDSP: sd rs2, offset(x2) */
				uint32_t rs2 = (ci >> 2) & 0x1F;
				uint32_t off = (((ci >> 10) & 7) << 3) |
				               (((ci >> 7) & 7) << 6);
				uint32_t imm_11_5 = (off >> 5) & 0x7F;
				uint32_t imm_4_0 = off & 0x1F;
				exp = (imm_11_5 << 25) | (rs2 << 20) | (2 << 15) | (3 << 12) | (imm_4_0 << 7) | 0x23;
				break;
			}
			default: goto illegal_compressed;
			}
			break;

		default:
illegal_compressed:
			trap(cpu, EXC_ILLEGAL_INSN, ci);
			return 0;
		}
		insn = exp;
	}

	/* Decode fields */
	uint32_t opcode = insn & 0x7F;
	uint32_t rd     = (insn >> 7) & 0x1F;
	uint32_t funct3 = (insn >> 12) & 0x7;
	uint32_t rs1    = (insn >> 15) & 0x1F;
	uint32_t rs2    = (insn >> 20) & 0x1F;
	uint32_t funct7 = (insn >> 25) & 0x7F;

	/* Immediate decoders */
	int64_t imm_i = sign_extend((insn >> 20), 12);
	int64_t imm_s = sign_extend(((insn >> 25) << 5) | ((insn >> 7) & 0x1F), 12);
	int64_t imm_b = sign_extend(
		(((insn >> 31) & 1) << 12) |
		(((insn >> 7) & 1) << 11) |
		(((insn >> 25) & 0x3F) << 5) |
		(((insn >> 8) & 0xF) << 1), 13);
	int64_t imm_u = sign_extend(insn & 0xFFFFF000ULL, 32);
	int64_t imm_j = sign_extend(
		(((insn >> 31) & 1) << 20) |
		(((insn >> 12) & 0xFF) << 12) |
		(((insn >> 20) & 1) << 11) |
		(((insn >> 21) & 0x3FF) << 1), 21);

	uint64_t next_pc = cpu->pc + (compressed ? 2 : 4);

	/* Profile: classify instruction by opcode */
	switch (opcode) {
	case 0x03: case 0x07:               cpu->prof.insn_load++;   break;
	case 0x23: case 0x27:               cpu->prof.insn_store++;  break;
	case 0x63:                          cpu->prof.insn_branch++; break;
	case 0x6F: case 0x67:               cpu->prof.insn_jump++;   break;
	case 0x73:                          cpu->prof.insn_csr++;    break;
	case 0x43: case 0x47: case 0x4B:
	case 0x4F: case 0x53:               cpu->prof.insn_fpu++;    break;
	case 0x2F:                          cpu->prof.insn_atomic++;  break;
	case 0x33: case 0x3B: case 0x13:
	case 0x1B: case 0x37: case 0x17:    cpu->prof.insn_alu++;    break;
	default:                            cpu->prof.insn_other++;  break;
	}
	cpu->prof.exec_cycles++;

	switch (opcode) {

	/* ---- LUI ---- */
	case 0x37:
		WR(rd, (uint64_t)imm_u);
		break;

	/* ---- AUIPC ---- */
	case 0x17:
		WR(rd, cpu->pc + (uint64_t)imm_u);
		break;

	/* ---- JAL ---- */
	case 0x6F:
		WR(rd, next_pc);
		next_pc = cpu->pc + (uint64_t)imm_j;
		break;

	/* ---- JALR ---- */
	case 0x67: {
		/* Read rs1 BEFORE writing rd (they may be the same register) */
		uint64_t target = (cpu->x[rs1] + (uint64_t)imm_i) & ~1ULL;
		WR(rd, next_pc);
		next_pc = target;
		break;
	}

	/* ---- Branches ---- */
	case 0x63: {
		uint64_t a = cpu->x[rs1], b = cpu->x[rs2];
		int take = 0;
		switch (funct3) {
		case 0: take = (a == b); break;                       /* BEQ */
		case 1: take = (a != b); break;                       /* BNE */
		case 4: take = ((int64_t)a < (int64_t)b); break;      /* BLT */
		case 5: take = ((int64_t)a >= (int64_t)b); break;     /* BGE */
		case 6: take = (a < b); break;                         /* BLTU */
		case 7: take = (a >= b); break;                        /* BGEU */
		default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
		}
		if (take)
			next_pc = cpu->pc + (uint64_t)imm_b;
		break;
	}

	/* ---- Loads ---- */
	case 0x03: {
		uint64_t addr = cpu->x[rs1] + (uint64_t)imm_i;
		uint64_t val;
		int size;
		switch (funct3) {
		case 0: size = 1; break; /* LB */
		case 1: size = 2; break; /* LH */
		case 2: size = 4; break; /* LW */
		case 3: size = 8; break; /* LD */
		case 4: size = 1; break; /* LBU */
		case 5: size = 2; break; /* LHU */
		case 6: size = 4; break; /* LWU */
		default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
		}
		exc = rv64_cpu_load(cpu, addr, size, &val, ACCESS_READ);
		if (exc) { trap(cpu, exc, addr); return 0; }
		/* Sign-extend for signed variants */
		switch (funct3) {
		case 0: val = (uint64_t)(int64_t)(int8_t)val; break;
		case 1: val = (uint64_t)(int64_t)(int16_t)val; break;
		case 2: val = (uint64_t)(int64_t)(int32_t)val; break;
		}
		WR(rd, val);
		break;
	}

	/* ---- Stores ---- */
	case 0x23: {
		uint64_t addr = cpu->x[rs1] + (uint64_t)imm_s;
		uint64_t val = cpu->x[rs2];
		int size;
		switch (funct3) {
		case 0: size = 1; break; /* SB */
		case 1: size = 2; break; /* SH */
		case 2: size = 4; break; /* SW */
		case 3: size = 8; break; /* SD */
		default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
		}
		exc = cpu_store(cpu, addr, size, val);
		if (exc) { trap(cpu, exc, addr); return 0; }
		break;
	}

	/* ---- ALU immediate (64-bit) ---- */
	case 0x13: {
		uint64_t src = cpu->x[rs1];
		uint64_t result;
		uint32_t shamt = (insn >> 20) & 0x3F; /* 6-bit for RV64 */
		switch (funct3) {
		case 0: result = src + (uint64_t)imm_i; break;              /* ADDI */
		case 1: result = src << shamt; break;                        /* SLLI */
		case 2: result = ((int64_t)src < imm_i) ? 1 : 0; break;     /* SLTI */
		case 3: result = (src < (uint64_t)imm_i) ? 1 : 0; break;    /* SLTIU */
		case 4: result = src ^ (uint64_t)imm_i; break;              /* XORI */
		case 5:
			if (funct7 >> 1)  /* SRAI */
				result = (uint64_t)((int64_t)src >> shamt);
			else              /* SRLI */
				result = src >> shamt;
			break;
		case 6: result = src | (uint64_t)imm_i; break;              /* ORI */
		case 7: result = src & (uint64_t)imm_i; break;              /* ANDI */
		default: result = 0;
		}
		WR(rd, result);
		break;
	}

	/* ---- ALU register (64-bit) ---- */
	case 0x33: {
		uint64_t a = cpu->x[rs1], b = cpu->x[rs2];
		uint64_t result;

		if (funct7 == 0x01) {
			/* M extension: multiply/divide */
			switch (funct3) {
			case 0: result = a * b; break;                           /* MUL */
			case 1: result = (uint64_t)((__int128)(int64_t)a * (int64_t)b >> 64); break; /* MULH */
			case 2: result = (uint64_t)((__int128)(int64_t)a * (__uint128_t)b >> 64); break; /* MULHSU */
			case 3: result = (uint64_t)((__uint128_t)a * b >> 64); break; /* MULHU */
			case 4: result = b ? (uint64_t)((int64_t)a / (int64_t)b) : ~0ULL; break;    /* DIV */
			case 5: result = b ? a / b : ~0ULL; break;               /* DIVU */
			case 6: result = b ? (uint64_t)((int64_t)a % (int64_t)b) : a; break;       /* REM */
			case 7: result = b ? a % b : a; break;                   /* REMU */
			default: result = 0;
			}
		} else {
			switch (funct3) {
			case 0: result = (funct7 == 0x20) ? a - b : a + b; break; /* ADD/SUB */
			case 1: result = a << (b & 0x3F); break;                   /* SLL */
			case 2: result = ((int64_t)a < (int64_t)b) ? 1 : 0; break; /* SLT */
			case 3: result = (a < b) ? 1 : 0; break;                   /* SLTU */
			case 4: result = a ^ b; break;                              /* XOR */
			case 5:
				if (funct7 == 0x20)
					result = (uint64_t)((int64_t)a >> (b & 0x3F)); /* SRA */
				else
					result = a >> (b & 0x3F);                       /* SRL */
				break;
			case 6: result = a | b; break;                              /* OR */
			case 7: result = a & b; break;                              /* AND */
			default: result = 0;
			}
		}
		WR(rd, result);
		break;
	}

	/* ---- ALU immediate (32-bit, W-variants) ---- */
	case 0x1B: {
		uint32_t src = (uint32_t)cpu->x[rs1];
		uint32_t shamt = (insn >> 20) & 0x1F;
		int32_t result32;
		switch (funct3) {
		case 0: result32 = (int32_t)(src + (uint32_t)imm_i); break; /* ADDIW */
		case 1: result32 = (int32_t)(src << shamt); break;          /* SLLIW */
		case 5:
			if (funct7 == 0x20)
				result32 = (int32_t)src >> shamt;            /* SRAIW */
			else
				result32 = (int32_t)(src >> shamt);          /* SRLIW */
			break;
		default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
		}
		WR(rd, (uint64_t)(int64_t)result32);
		break;
	}

	/* ---- ALU register (32-bit, W-variants) ---- */
	case 0x3B: {
		uint32_t a = (uint32_t)cpu->x[rs1];
		uint32_t b = (uint32_t)cpu->x[rs2];
		int32_t result32;

		if (funct7 == 0x01) {
			/* M extension W-variants */
			switch (funct3) {
			case 0: result32 = (int32_t)(a * b); break;                /* MULW */
			case 4: result32 = b ? (int32_t)a / (int32_t)b : -1; break; /* DIVW */
			case 5: result32 = b ? (int32_t)(a / b) : -1; break;       /* DIVUW */
			case 6: result32 = b ? (int32_t)a % (int32_t)b : (int32_t)a; break; /* REMW */
			case 7: result32 = b ? (int32_t)(a % b) : (int32_t)a; break; /* REMUW */
			default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
			}
		} else {
			switch (funct3) {
			case 0:
				result32 = (funct7 == 0x20) ?
					(int32_t)(a - b) : (int32_t)(a + b);
				break;
			case 1: result32 = (int32_t)(a << (b & 0x1F)); break;  /* SLLW */
			case 5:
				if (funct7 == 0x20)
					result32 = (int32_t)a >> (b & 0x1F);    /* SRAW */
				else
					result32 = (int32_t)(a >> (b & 0x1F));  /* SRLW */
				break;
			default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
			}
		}
		WR(rd, (uint64_t)(int64_t)result32);
		break;
	}

	/* ---- Atomics (A extension) ---- */
	case 0x2F: {
		uint64_t addr = cpu->x[rs1];
		int is_64 = (funct3 == 3); /* funct3: 2=word, 3=doubleword */
		int size = is_64 ? 8 : 4;
		uint32_t funct5 = funct7 >> 2;

		uint64_t loaded;
		exc = rv64_cpu_load(cpu, addr, size, &loaded, ACCESS_READ);
		if (exc) { trap(cpu, exc, addr); return 0; }

		/* Sign-extend 32-bit loads */
		if (!is_64)
			loaded = (uint64_t)(int64_t)(int32_t)loaded;

		uint64_t result;
		int64_t sa = (int64_t)loaded;
		int64_t sb = (int64_t)cpu->x[rs2];

		switch (funct5) {
		case 0x02: /* LR */
			cpu->reservation.valid = 1;
			cpu->reservation.addr = addr;
			WR(rd, loaded);
			goto amo_done;

		case 0x03: /* SC */
			if (cpu->reservation.valid && cpu->reservation.addr == addr) {
				exc = cpu_store(cpu, addr, size, cpu->x[rs2]);
				if (exc) { trap(cpu, exc, addr); return 0; }
				WR(rd, 0);  /* success */
			} else {
				WR(rd, 1);  /* failure */
			}
			cpu->reservation.valid = 0;
			goto amo_done;

		case 0x01: result = cpu->x[rs2]; break;                /* AMOSWAP */
		case 0x00: result = loaded + cpu->x[rs2]; break;       /* AMOADD */
		case 0x04: result = loaded ^ cpu->x[rs2]; break;       /* AMOXOR */
		case 0x0C: result = loaded & cpu->x[rs2]; break;       /* AMOAND */
		case 0x08: result = loaded | cpu->x[rs2]; break;       /* AMOOR */
		case 0x10: result = (sa < sb) ? loaded : cpu->x[rs2]; break;  /* AMOMIN */
		case 0x14: result = (sa > sb) ? loaded : cpu->x[rs2]; break;  /* AMOMAX */
		case 0x18: result = (loaded < cpu->x[rs2]) ? loaded : cpu->x[rs2]; break; /* AMOMINU */
		case 0x1C: result = (loaded > cpu->x[rs2]) ? loaded : cpu->x[rs2]; break; /* AMOMAXU */
		default: trap(cpu, EXC_ILLEGAL_INSN, insn); return 0;
		}

		/* AMO: write result back to memory, return old value */
		exc = cpu_store(cpu, addr, size, result);
		if (exc) { trap(cpu, exc, addr); return 0; }
		WR(rd, loaded);

amo_done:
		break;
	}

	/* ---- FENCE (memory ordering & I-cache invalidation) ---- */
	case 0x0F:
		if (funct3 == 1 && cpu->jit) { /* FENCE.I */
			jit_flush(cpu->jit);
		}
		break;

	/* ---- SYSTEM ---- */
	case 0x73: {
		uint32_t csr_addr = (insn >> 20) & 0xFFF;

		if (funct3 == 0) {
			/* ECALL / EBREAK / MRET / SRET / WFI / SFENCE.VMA */
			switch (insn) {
			case 0x00000073: /* ECALL */
				switch (cpu->priv) {
				case PRIV_U: trap(cpu, EXC_ECALL_U, 0); return 0;
				case PRIV_S: trap(cpu, EXC_ECALL_S, 0); return 0;
				case PRIV_M: trap(cpu, EXC_ECALL_M, 0); return 0;
				}
				break;

			case 0x00100073: /* EBREAK */
				trap(cpu, EXC_BREAKPOINT, cpu->pc);
				return 0;

			case 0x30200073: /* MRET */
				if (cpu->priv < PRIV_M) {
					trap(cpu, EXC_ILLEGAL_INSN, insn);
					return 0;
				}
				/* Restore MIE from MPIE */
				uint64_t mpie = (cpu->mstatus >> 7) & 1;
				uint64_t mpp = (cpu->mstatus >> 11) & 3;
				cpu->mstatus = (cpu->mstatus & ~MSTATUS_MIE) |
				               (mpie << 3);
				cpu->mstatus |= MSTATUS_MPIE;
				cpu->mstatus &= ~MSTATUS_MPP;
				cpu->priv = mpp;
				next_pc = cpu->mepc;
				break;

			case 0x10200073: /* SRET */
				if (cpu->priv < PRIV_S) {
					trap(cpu, EXC_ILLEGAL_INSN, insn);
					return 0;
				}
				uint64_t spie = (cpu->mstatus >> 5) & 1;
				uint64_t spp = (cpu->mstatus >> 8) & 1;
				cpu->mstatus = (cpu->mstatus & ~MSTATUS_SIE) |
				               (spie << 1);
				cpu->mstatus |= MSTATUS_SPIE;
				cpu->mstatus &= ~MSTATUS_SPP;
				cpu->priv = spp;
				next_pc = cpu->sepc;
				/* Backfill trap log: capture syscall return value
				 * and detect signal delivery (ret_pc != expected) */
				if (spp == PRIV_U && cpu->dbg.trap_log_count > 0) {
					int s = cpu->dbg.trap_log_last_slot;
					cpu->dbg.trap_log[s].ret_a0 = cpu->x[10];
					cpu->dbg.trap_log[s].ret_pc = cpu->sepc;
				}
				break;

			case 0x10500073: /* WFI */
				cpu->halted = 1; /* will wake on interrupt */
				break;

			default:
				/* SFENCE.VMA (0001001 rs2 rs1 000 00000 1110011) */
				if (funct7 == 0x09) {
					rv64_tlb_flush(cpu);
					break;
				}
				trap(cpu, EXC_ILLEGAL_INSN, insn);
				return 0;
			}
		} else {
			/* CSR instructions */
			uint64_t old_val;
			if (csr_read(cpu, csr_addr, &old_val) < 0) {
				trap(cpu, EXC_ILLEGAL_INSN, insn);
				return 0;
			}

			uint64_t src = (funct3 & 4) ? rs1 : cpu->x[rs1]; /* imm or reg */
			uint64_t new_val;

			switch (funct3 & 3) {
			case 1: new_val = src; break;              /* CSRRW / CSRRWI */
			case 2: new_val = old_val | src; break;    /* CSRRS / CSRRSI */
			case 3: new_val = old_val & ~src; break;   /* CSRRC / CSRRCI */
			default: new_val = old_val;
			}

			/* Write CSR if source register is not x0 (or non-zero imm) */
			if ((funct3 & 3) == 1 || src != 0) {
				uint64_t prev_mstatus = cpu->mstatus;
				csr_write(cpu, csr_addr, new_val);
				if (csr_addr == CSR_SSTATUS || csr_addr == CSR_MSTATUS) {
					if (!(prev_mstatus & MSTATUS_SIE) && (cpu->mstatus & MSTATUS_SIE))
						cpu->pending_irq_check = 1;
				}
			}

			WR(rd, old_val);
		}
		break;
	}

	/* ---- Floating-point instructions ---- */
	case 0x07:  /* FLW / FLD */
	case 0x27:  /* FSW / FSD */
	case 0x43:  /* FMADD */
	case 0x47:  /* FMSUB */
	case 0x4B:  /* FNMSUB */
	case 0x4F:  /* FNMADD */
	case 0x53: { /* FP arithmetic */
		int fp_rc = rv64_fp_exec(cpu, insn);
		if (fp_rc < 0) {
			trap(cpu, EXC_ILLEGAL_INSN, insn);
			return 0;
		}
		if (fp_rc > 0)
			return 0; /* memory fault — trap already delivered by FPU handler */
		break;
	}

	default:
		fprintf(stderr, "rv64: UNKNOWN OPCODE 0x%02x insn=0x%08x pc=0x%lx\n",
			opcode, insn, (unsigned long)cpu->pc);
		trap(cpu, EXC_ILLEGAL_INSN, insn);
		return 0;
	}

	cpu->pc = next_pc;
	cpu->instret++;
	cpu->cycle++;
	return 0;
}

/* ---- Main run loop ---- */

/* Forward declaration — defined below rv64_run */
static uint64_t rv64_run_jit(struct rv64_cpu *cpu, uint64_t n);

uint64_t rv64_run(struct rv64_cpu *cpu, uint64_t n)
{
	/* Use JIT-accelerated loop if available */
	if (cpu->jit && cpu->jit->enabled)
		return rv64_run_jit(cpu, n);

	uint64_t start = cpu->cycle;
	uint64_t max_cycles = start + n;
	uint64_t check_interval = 9973; /* check interrupts every N cycles (prime to avoid loop aliasing) */
	uint64_t next_check_cycle = start + check_interval;
	cpu->run_exit_reason = RUN_EXIT_NONE;

	while (cpu->cycle < max_cycles && !cpu->error) {
		/* Advance timer and check interrupts periodically */
		if (cpu->pending_irq_check || cpu->cycle >= next_check_cycle) {
			if (cpu->cycle >= next_check_cycle) {
				next_check_cycle = cpu->cycle + check_interval;
				/*
				 * Drive mtime from the host's monotonic clock.
				 * RISC-V specifies a 10 MHz timebase (1 tick = 100 ns).
				 * This makes guest sleep/timeout durations track real
				 * wall-clock time regardless of emulated MIPS.
				 */
				struct timespec _ts;
				clock_gettime(CLOCK_MONOTONIC, &_ts);
				uint64_t _now_ns = (uint64_t)_ts.tv_sec * 1000000000ULL
				                 + (uint64_t)_ts.tv_nsec;
				cpu->clint.mtime = (_now_ns - vm_boot_time_ns) / 100;
			}
			cpu->pending_irq_check = 0;
			if (check_interrupts(cpu))
				continue;
		}

		if (cpu->halted) {
			/* WFI is a hint — if no interrupts are enabled (mie==0),
			 * nothing can wake us. Treat as NOP per RISC-V spec. */
			if (cpu->mie == 0) {
				cpu->halted = 0;
				continue;
			}
			while (cpu->halted && cpu->cycle < max_cycles) {
				/* Advance real-time mtime while WFI-halted */
				struct timespec _ts;
				clock_gettime(CLOCK_MONOTONIC, &_ts);
				uint64_t _now_ns = (uint64_t)_ts.tv_sec * 1000000000ULL
				                 + (uint64_t)_ts.tv_nsec;
				cpu->clint.mtime = (_now_ns - vm_boot_time_ns) / 100;
				cpu->cycle += check_interval;
				cpu->prof.wfi_cycles += check_interval;
				check_interrupts(cpu);
				if (cpu->mip & cpu->mie) {
					cpu->halted = 0;
					break;
				}
                usleep(1000); /* Sleep 1ms to allow real time to advance before burning cycle budget */
			}
			if (cpu->halted) {
				cpu->run_exit_reason = RUN_EXIT_WFI_TIMEOUT;
				break;
			}
			continue;
		}


		if (rv64_step(cpu) < 0) {
			cpu->error = 1;
			cpu->run_exit_reason = RUN_EXIT_ERROR;
			break;
		}

		/* Check debug halt — only honor if break_halt was explicitly set */
		if (cpu->dbg.debug_halt && cpu->dbg.break_halt) {
			cpu->run_exit_reason = RUN_EXIT_DEBUG_HALT;
			break;
		}
		cpu->dbg.debug_halt = 0; /* clear spurious corruption */
	}

	/* Set exit reason if we fell through the while condition */
	if (cpu->run_exit_reason == RUN_EXIT_NONE) {
		if (cpu->dbg.debug_halt)
			cpu->run_exit_reason = RUN_EXIT_DEBUG_HALT;
		else if (cpu->error)
			cpu->run_exit_reason = RUN_EXIT_ERROR;
		else
			cpu->run_exit_reason = RUN_EXIT_BUDGET;
	}

	return cpu->cycle - start;
}

/*
 * rv64_run_jit — JIT-accelerated run loop.
 *
 * Tries JIT execution first for each PC. Falls back to the interpreter
 * for instructions the JIT can't compile (CSR, atomics, FPU, ecall, etc).
 * Once the interpreter advances past the uncompilable instruction,
 * control returns to the JIT.
 */
static uint64_t rv64_run_jit(struct rv64_cpu *cpu, uint64_t n)
{
	uint64_t start = cpu->cycle;
	uint64_t max_cycles = start + n;
	uint64_t check_interval = 10000;
	uint64_t next_check_cycle = start + check_interval;
	struct jit_state *jit = cpu->jit;
	cpu->run_exit_reason = RUN_EXIT_NONE;

	while (cpu->cycle < max_cycles && !cpu->error) {
		/* Advance timer and check interrupts periodically */
		if (cpu->cycle >= next_check_cycle) {
            next_check_cycle = cpu->cycle + check_interval;
			/* Drive mtime from host CLOCK_MONOTONIC (10 MHz = 100 ns/tick) */
			struct timespec _ts;
			clock_gettime(CLOCK_MONOTONIC, &_ts);
			uint64_t _now_ns = (uint64_t)_ts.tv_sec * 1000000000ULL
			                 + (uint64_t)_ts.tv_nsec;
			cpu->clint.mtime = (_now_ns - vm_boot_time_ns) / 100;

			if (check_interrupts(cpu))
				continue;
		}

		if (cpu->halted) {
			/* WFI is a hint — if no interrupts are enabled (mie==0),
			 * nothing can wake us. Treat as NOP per RISC-V spec. */
			if (cpu->mie == 0) {
				cpu->halted = 0;
				continue;
			}
			while (cpu->halted && cpu->cycle < max_cycles) {
				/* Advance real-time mtime while WFI-halted */
				struct timespec _ts;
				clock_gettime(CLOCK_MONOTONIC, &_ts);
				uint64_t _now_ns = (uint64_t)_ts.tv_sec * 1000000000ULL
				                 + (uint64_t)_ts.tv_nsec;
				cpu->clint.mtime = (_now_ns - vm_boot_time_ns) / 100;
				cpu->cycle += check_interval;
				cpu->prof.wfi_cycles += check_interval;
				check_interrupts(cpu);
				if (cpu->mip & cpu->mie) {
					cpu->halted = 0;
					break;
				}
                usleep(1000); /* Sleep 1ms to allow real time to advance before burning cycle budget */
			}
			if (cpu->halted) {
				cpu->run_exit_reason = RUN_EXIT_WFI_TIMEOUT;
				break;
			}
			continue;
		}

		/* Record statistical JIT samples for flamegraphs */
		if (__builtin_expect(++cpu->prof.perf_sample_tick >= 128, 0)) {
			cpu->prof.perf_sample_tick = 0;
			uint32_t idx = cpu->prof.perf_sample_idx++ % DBG_PERF_LOG_SIZE;
			cpu->prof.perf_samples[idx].pc = cpu->pc;
			cpu->prof.perf_samples[idx].ra = cpu->x[1];
			cpu->prof.perf_samples[idx].fp = cpu->x[8];
		}

		/* Try JIT first */
#ifndef DISABLE_JIT
        if (!(cpu->mip & cpu->mie)) {
		    int jit_ran = jit_exec(cpu);
		    if (jit_ran > 0) {
			    cpu->cycle += jit_ran;
			    continue;
		    }
        }
#endif


		/* JIT couldn't handle this PC — fall back to interpreter for one insn */
		if (rv64_step(cpu) < 0) {
			cpu->error = 1;
			cpu->run_exit_reason = RUN_EXIT_ERROR;
			break;
		}

		if (cpu->dbg.debug_halt && cpu->dbg.break_halt) {
			cpu->run_exit_reason = RUN_EXIT_DEBUG_HALT;
			break;
		}
		cpu->dbg.debug_halt = 0;
	}

	if (cpu->run_exit_reason == RUN_EXIT_NONE) {
		if (cpu->error)
			cpu->run_exit_reason = RUN_EXIT_ERROR;
		else
			cpu->run_exit_reason = RUN_EXIT_BUDGET;
	}

	return cpu->cycle - start;
}

