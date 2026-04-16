/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rv64_mmu.c — Sv39 page table walker + TLB for the LinuxSQL RISC-V emulator
 *
 * Implements RISC-V Sv39 virtual memory translation:
 *   - 3-level page tables (VPN[2], VPN[1], VPN[0])
 *   - 39-bit virtual address → 56-bit physical address
 *   - Superpages (1 GiB and 2 MiB)
 *   - Permission checking (R/W/X/U bits)
 *   - Access/Dirty bit management
 *   - SUM (permit Supervisor User Memory access) support
 *   - MXR (Make eXecutable Readable) support
 *   - 256-entry direct-mapped TLB
 */
#include "rv64_cpu.h"
#include "rv64_jit.h"
#include <string.h>

extern uint8_t *get_guest_ram_ptr(void);

/* Flush the entire TLB and invalidate JIT code cache */
void rv64_tlb_flush(struct rv64_cpu *cpu)
{
	memset(cpu->tlb, 0, sizeof(cpu->tlb));

	/* Flush JIT cache: sfence.vma means the address space changed
	 * (e.g. execve, munmap, mprotect).  Stale JIT blocks compiled
	 * from a previous process would execute the wrong code at the
	 * same virtual address and crash the new process. */
	if (cpu->jit)
		jit_flush(cpu->jit);
}

/*
 * rv64_translate — Translate a virtual address to a physical address.
 *
 * If satp.MODE == Bare (or we're in M-mode), returns vaddr directly.
 * Otherwise checks the TLB, falling through to a full Sv39 page walk
 * on miss.
 *
 * Returns 0 on success (*pa is set).
 * Returns the appropriate exception code on fault.
 */
int rv64_translate(struct rv64_cpu *cpu, uint64_t vaddr, int access_type,
                   uint64_t *pa)
{
	/* M-mode always uses physical addresses */
	if (cpu->priv == PRIV_M) {
		*pa = vaddr;
		return 0;
	}

	/* Check satp.MODE */
	uint64_t mode = cpu->satp >> SATP_MODE_SHIFT;
	if (mode == SATP_MODE_BARE) {
		*pa = vaddr;
		return 0;
	}

	/* ---- TLB lookup ---- */
	uint64_t vpn_full = vaddr >> PAGE_SHIFT;
	unsigned int idx = vpn_full & TLB_INDEX_MASK;
	struct tlb_entry *te = &cpu->tlb[idx];

	if (te->valid && te->vpn == vpn_full) {
		/* Tag match — check permissions */
		int ok = 0;
		switch (access_type) {
		case ACCESS_READ:
			ok = (te->perm & PTE_R) ||
			     ((cpu->mstatus & MSTATUS_MXR) && (te->perm & PTE_X));
			break;
		case ACCESS_WRITE:
			ok = (te->perm & PTE_W);
			/* Must also have dirty bit; if not, fall through to walk
			 * so the PTE gets updated with PTE_D */
			if (ok && !te->dirty) {
				ok = 0; /* force walk to set D bit */
			}
			break;
		case ACCESS_EXEC:
			ok = (te->perm & PTE_X);
			break;
		}

		/* Privilege check */
		if (ok) {
			if (cpu->priv == PRIV_U && !(te->perm & PTE_U))
				ok = 0;
			if (cpu->priv == PRIV_S && (te->perm & PTE_U) &&
			    !(cpu->mstatus & MSTATUS_SUM))
				ok = 0;
		}

		if (ok) {
			/* Reconstruct physical address from cached PPN + page offset */
			cpu->prof.tlb_hits++;
			/* Reconstruct physical address from cached PPN + page offset */
			uint64_t page_offset;
			switch (te->level) {
			case 2: page_offset = vaddr & 0x3FFFFFFFULL; break; /* 1G */
			case 1: page_offset = vaddr & 0x1FFFFFULL;   break; /* 2M */
			default: page_offset = vaddr & PAGE_MASK;    break; /* 4K */
			}
			*pa = (te->ppn << PAGE_SHIFT) | page_offset;
			return 0;
		}
		/* Permission failure on cached entry — fall through to walk
		 * (might need to set D bit, or might be a real fault) */
	}

	cpu->prof.tlb_misses++;

	/* ---- Full Sv39 page table walk ---- */
	cpu->prof.mmu_walks++;

	uint64_t vpn[3];
	vpn[2] = (vaddr >> 30) & 0x1FF;
	vpn[1] = (vaddr >> 21) & 0x1FF;
	vpn[0] = (vaddr >> 12) & 0x1FF;
	uint64_t page_offset = vaddr & PAGE_MASK;

	/* Root page table physical address from satp.PPN */
	uint64_t pt_addr = (cpu->satp & 0xFFFFFFFFFFFULL) << PAGE_SHIFT;

	int fault_code;
	switch (access_type) {
	case ACCESS_EXEC:  fault_code = EXC_INSN_PAGE_FAULT; break;
	case ACCESS_READ:  fault_code = EXC_LOAD_PAGE_FAULT; break;
	case ACCESS_WRITE: fault_code = EXC_STORE_PAGE_FAULT; break;
	default:           fault_code = EXC_LOAD_PAGE_FAULT; break;
	}

	uint64_t pte;
	int level;

	for (level = 2; level >= 0; level--) {
		/* Read PTE from physical memory.
		 * Page table entries are always in RAM — inline the read
		 * to avoid bus_load's device dispatch overhead. */
		uint64_t pte_addr = pt_addr + vpn[level] * 8;
		cpu->prof.mmu_walk_steps++;
		if (__builtin_expect(pte_addr >= RAM_BASE &&
		                     pte_addr + 8 <= RAM_BASE + cpu->ram_size, 1)) {
			uint8_t *ram = get_guest_ram_ptr();
			if (ram) __builtin_memcpy(&pte, ram + (pte_addr - RAM_BASE), 8);
			else return fault_code;
		} else if (bus_load(cpu, pte_addr, 8, &pte) < 0) {
			return fault_code;
		}

		/* Check Valid bit */
		if (!(pte & PTE_V))
			return fault_code;

		/* Check for reserved bit patterns */
		uint64_t rwx = pte & (PTE_R | PTE_W | PTE_X);
		if (rwx == PTE_W || rwx == (PTE_W | PTE_X))
			return fault_code; /* W without R is reserved */

		/* Leaf PTE (has at least R, X, or both) */
		if (rwx != 0) {
			/* ---- Permission checks ---- */

			/* User-mode accessing non-User page */
			if (cpu->priv == PRIV_U && !(pte & PTE_U))
				return fault_code;

			/*
			 * Supervisor accessing User page:
			 * allowed only if SUM bit is set in mstatus.
			 */
			if (cpu->priv == PRIV_S && (pte & PTE_U) &&
			    !(cpu->mstatus & MSTATUS_SUM))
				return fault_code;

			/* Check access type permission */
			if (access_type == ACCESS_READ) {
				/*
				 * MXR (Make eXecutable Readable):
				 * if set, exec-only pages are also readable
				 */
				int readable = (pte & PTE_R) ||
					       ((cpu->mstatus & MSTATUS_MXR) &&
						(pte & PTE_X));
				if (!readable)
					return fault_code;
			}
			if (access_type == ACCESS_WRITE && !(pte & PTE_W))
				return fault_code;
			if (access_type == ACCESS_EXEC && !(pte & PTE_X))
				return fault_code;

			/* ---- Superpage alignment check ---- */
			if (level == 2) {
				/* 1 GiB page: VPN[1] and VPN[0] must be 0 in PTE */
				if ((pte >> 19) & 0x1FF || (pte >> 10) & 0x1FF)
					return fault_code;
			} else if (level == 1) {
				/* 2 MiB page: VPN[0] must be 0 in PTE */
				if ((pte >> 10) & 0x1FF)
					return fault_code;
			}

			/* ---- Set A and D bits ---- */
			uint64_t new_pte = pte | PTE_A;
			if (access_type == ACCESS_WRITE)
				new_pte |= PTE_D;
			if (new_pte != pte)
				bus_store(cpu, pte_addr, 8, new_pte);

			/* ---- Compute physical address ---- */
			uint64_t ppn = (pte >> 10);
			uint64_t result;

			if (level == 2) {
				/* 1 GiB superpage */
				result = (ppn << PAGE_SHIFT) |
					 (vpn[1] << 21) |
					 (vpn[0] << 12) |
					 page_offset;
			} else if (level == 1) {
				/* 2 MiB superpage */
				result = ((ppn >> 9) << 21) |
					 (vpn[0] << 12) |
					 page_offset;
			} else {
				/* 4 KiB page */
				result = (ppn << PAGE_SHIFT) | page_offset;
			}

			/* ---- Populate TLB ---- */
			te->vpn   = vpn_full;
			te->ppn   = result >> PAGE_SHIFT;
			te->perm  = (uint8_t)(pte & (PTE_R | PTE_W | PTE_X | PTE_U));
			te->level = (uint8_t)level;
			te->dirty = (new_pte & PTE_D) ? 1 : 0;
			te->valid = 1;

			/* Cache host pointer for RAM pages (bypass bus on hit) */
			uint64_t page_pa = (te->ppn << PAGE_SHIFT);
			uint8_t *ram = get_guest_ram_ptr();
			if (ram && page_pa >= RAM_BASE &&
			    page_pa + PAGE_SIZE <= RAM_BASE + cpu->ram_size)
				te->host_base = ram + (page_pa - RAM_BASE);
			else
				te->host_base = NULL; /* MMIO — must go through bus */

			*pa = result;
			return 0;
		}

		/* Non-leaf PTE: follow to next level */
		pt_addr = ((pte >> 10) & 0xFFFFFFFFFFFULL) << PAGE_SHIFT;
	}

	/* Ran out of levels without finding a leaf */
	return fault_code;
}
