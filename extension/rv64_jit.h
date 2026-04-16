/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rv64_jit.h — RISC-V to ARM64 JIT compiler for LinuxSQL VM
 *
 * Translates basic blocks of RISC-V instructions to native ARM64 code.
 * Uses macOS MAP_JIT for executable memory with W^X enforcement.
 */
#ifndef RV64_JIT_H
#define RV64_JIT_H

#include <stdint.h>

/* Code buffer: 16 MiB mmap'd with MAP_JIT */
#define JIT_CODE_BUF_SIZE   (64 * 1024 * 1024)

/* Code cache: direct-mapped by guest PC */
#define JIT_CACHE_BITS      16
#define JIT_CACHE_SIZE      (1 << JIT_CACHE_BITS)
#define JIT_CACHE_MASK      (JIT_CACHE_SIZE - 1)

/* Limit per-block compilation to avoid runaway emission */
#define JIT_MAX_BLOCK_INSNS 64

struct jit_block {
	uint64_t  guest_pc;     /* tag: guest PC at block entry */
	uint32_t *host_code;    /* ARM64 code pointer in code_buf */
	uint32_t  insn_count;   /* guest instructions in block */
	int       valid;
	int       has_store;    /* 1 if block contains store instructions */
	int       pending;      /* 1 if currently compiling in the background */
};

struct jit_state {
	uint32_t         *code_buf;
	uint32_t          code_off;       /* write cursor (in 32-bit words) */
	uint32_t          code_capacity;  /* total words in code_buf */
	struct jit_block  cache[JIT_CACHE_SIZE];
	int               enabled;       /* 0 if mmap failed or unsupported */
	int               offline_mode;  /* 1 if executing as parallel bg worker */
	
	/* Offload Inter-Process Communication */
	void             *mqh_req;
	void             *mqh_res;
	uint64_t          last_req_pc;

	/* Helper function addresses, resolved at init */
	void             *fn_load;   /* rv64_jit_load */
	void             *fn_store;  /* rv64_jit_store */

	/* Diagnostic counters */
	uint64_t          stat_hits;        /* cache hits (block re-executed) */
	uint64_t          stat_compiles;    /* blocks compiled */
	uint64_t          stat_compile_fail;/* PCs that couldn't be compiled */
	uint64_t          stat_jit_insns;   /* guest insns executed via JIT */

	/* ---- Runtime diagnostic controls ---- */

	/* Tool 1: adjustable max block size (0 = use compile-time default) */
	int               rt_max_block_insns;

	/* Tool 2: per-opcode disable bitmask (128 bits covers all 7-bit opcodes) */
	uint64_t          disabled_ops[2]; /* bit N = opcode N is forced to interpreter */
};

struct rv64_cpu;

int  jit_init(struct jit_state *jit);
void jit_destroy(struct jit_state *jit);
void jit_flush(struct jit_state *jit);
void jit_invalidate(struct jit_state *jit);
int  jit_exec(struct rv64_cpu *cpu);
uint32_t jit_compile_offline(struct rv64_cpu *cpu, struct jit_state *jit, uint64_t pc, struct jit_block *out_b);

#endif /* RV64_JIT_H */
