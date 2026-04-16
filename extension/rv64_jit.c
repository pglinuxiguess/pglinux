/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rv64_jit.c — RISC-V → ARM64 JIT compiler
 *
 * Translates basic blocks of guest code to native ARM64 machine code.
 * Compiled blocks are cached and re-executed on subsequent hits.
 *
 * Register convention in generated code:
 *   x19 = struct rv64_cpu *cpu  (pinned, callee-saved)
 *   x21 = guest instruction counter within block (callee-saved)
 *   x0-x4 = temporaries for instruction translation
 *   x16 = scratch for helper function addresses (IP0)
 *   [sp, #0] = 8-byte slot for load helper output value
 */
#include <sys/mman.h>
#include <pthread.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#else
static inline void sys_icache_invalidate(void *start, size_t len) {
    __builtin___clear_cache((char *)start, ((char *)start) + len);
}
static inline void pthread_jit_write_protect_np(int enabled) {}
#endif
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "rv64_cpu.h"
#include "rv64_jit.h"
#include "postgres.h"
#include "storage/shm_mq.h"

#ifndef MAP_JIT
#define MAP_JIT 0
#endif

/* ================================================================
 * Section 1: ARM64 encoding helpers
 * ================================================================ */

static inline void emit(struct jit_state *j, uint32_t insn)
{
	if (j->code_off < j->code_capacity)
		j->code_buf[j->code_off++] = insn;
}

/* Load 64-bit immediate into Xrd via MOVZ + up to 3 MOVK */
static void emit_mov_imm64(struct jit_state *j, int rd, uint64_t imm)
{
	int first = 1;
	for (int s = 0; s < 4; s++) {
		uint16_t chunk = (imm >> (s * 16)) & 0xFFFF;
		if (chunk || (s == 3 && first)) {
			if (first) {
				/* MOVZ Xd, #chunk, LSL #(s*16) */
				emit(j, 0xD2800000 | (s << 21) | ((uint32_t)chunk << 5) | rd);
				first = 0;
			} else {
				/* MOVK Xd, #chunk, LSL #(s*16) */
				emit(j, 0xF2800000 | (s << 21) | ((uint32_t)chunk << 5) | rd);
			}
		}
	}
	if (first) /* imm == 0 */
		emit(j, 0xD2800000 | rd);
}

/* LDR Xd, [Xn, #imm]  (unsigned offset, scaled by 8)
 * Maximum offset: 4095 * 8 = 32760 bytes. For larger offsets,
 * compute address with emit_mov_imm64 + emit_add first. */
static inline void emit_ldr_x(struct jit_state *j, int rd, int rn, int off)
{
	emit(j, 0xF9400000 | ((off / 8) << 10) | (rn << 5) | rd);
}

/* STR Xd, [Xn, #imm]  (unsigned offset, scaled by 8, max 32760) */
static inline void emit_str_x(struct jit_state *j, int rd, int rn, int off)
{
	emit(j, 0xF9000000 | ((off / 8) << 10) | (rn << 5) | rd);
}

/* MOV Xd, Xm  (= ORR Xd, XZR, Xm) */
static inline void emit_mov(struct jit_state *j, int rd, int rm)
{
	emit(j, 0xAA0003E0 | (rm << 16) | rd);
}

/* ADD Xd, Xn, Xm */
static inline void emit_add(struct jit_state *j, int rd, int rn, int rm)
{
	emit(j, 0x8B000000 | (rm << 16) | (rn << 5) | rd);
}

/* SUB Xd, Xn, Xm */
static inline void emit_sub(struct jit_state *j, int rd, int rn, int rm)
{
	emit(j, 0xCB000000 | (rm << 16) | (rn << 5) | rd);
}

/* ADD Xd, Xn, #imm12  (unsigned 12-bit immediate) */
static inline void emit_add_imm12(struct jit_state *j, int rd, int rn, uint32_t imm12)
{
	emit(j, 0x91000000 | (imm12 << 10) | (rn << 5) | rd);
}

/* SUB Xd, Xn, #imm12 */
static inline void emit_sub_imm12(struct jit_state *j, int rd, int rn, uint32_t imm12)
{
	emit(j, 0xD1000000 | (imm12 << 10) | (rn << 5) | rd);
}

/* Add signed immediate: picks ADD or SUB, or loads into temp for large values */
static void emit_add_simm(struct jit_state *j, int rd, int rn, int64_t imm)
{
	if (imm >= 0 && imm <= 4095) {
		emit_add_imm12(j, rd, rn, (uint32_t)imm);
	} else if (imm < 0 && imm >= -4095) {
		emit_sub_imm12(j, rd, rn, (uint32_t)(-imm));
	} else {
		emit_mov_imm64(j, 4, (uint64_t)imm); /* x4 = imm */
		emit_add(j, rd, rn, 4);
	}
}

/* CMP Xn, Xm  (= SUBS XZR, Xn, Xm) */
static inline void emit_cmp(struct jit_state *j, int rn, int rm)
{
	emit(j, 0xEB00001F | (rm << 16) | (rn << 5));
}

/* B offset (unconditional, PC-relative, offset in bytes) */
static inline void emit_b(struct jit_state *j, int32_t off_bytes)
{
	emit(j, 0x14000000 | ((off_bytes / 4) & 0x3FFFFFF));
}

/* B.cond offset (conditional, offset in bytes) */
static inline void emit_bcond(struct jit_state *j, int cond, int32_t off_bytes)
{
	emit(j, 0x54000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | cond);
}

/* CBNZ Wn, offset */
static inline void emit_cbnz_w(struct jit_state *j, int rn, int32_t off_bytes)
{
	emit(j, 0x35000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | rn);
}

/* BLR Xn */
static inline void emit_blr(struct jit_state *j, int rn)
{
	emit(j, 0xD63F0000 | (rn << 5));
}

/* Condition codes */
#define A64_EQ   0
#define A64_NE   1
#define A64_HS   2  /* unsigned >= */
#define A64_LO   3  /* unsigned <  */
#define A64_GE  10  /* signed >=   */
#define A64_LT  11  /* signed <    */

/* CSET Xd, cond  (= CSINC Xd, XZR, XZR, inv(cond)) */
static inline void emit_cset(struct jit_state *j, int rd, int inv_cond)
{
	emit(j, 0x9A9F07E0 | (inv_cond << 12) | rd);
}

/* SXTW Xd, Wn  (= SBFM Xd, Xn, #0, #31) */
static inline void emit_sxtw(struct jit_state *j, int rd, int rn)
{
	emit(j, 0x93407C00 | (rn << 5) | rd);
}

/* SXTH Xd, Wn  (= SBFM Xd, Xn, #0, #15) */
static inline void emit_sxth(struct jit_state *j, int rd, int rn)
{
	emit(j, 0x93403C00 | (rn << 5) | rd);
}

/* SXTB Xd, Wn  (= SBFM Xd, Xn, #0, #7) */
static inline void emit_sxtb(struct jit_state *j, int rd, int rn)
{
	emit(j, 0x93401C00 | (rn << 5) | rd);
}

/* AND Xd, Xn, #bitmask — only used for AND x, x, 0xFFFFFFFF (32-bit truncate)
 * Encoding: UBFM Xd, Xn, #0, #31 = UXTW Xd, Wn
 * But simpler: just use MOV Wd, Wn which zeroes upper 32 bits.
 * We'll use: ORR Wd, WZR, Wn  (32-bit MOV)
 */
static inline void emit_uxtw(struct jit_state *j, int rd, int rn)
{
	/* MOV Wd, Wn = ORR Wd, WZR, Wn */
	emit(j, 0x2A0003E0 | (rn << 16) | rd);
}

/* ADD Wd, Wn, Wm  (32-bit add) */
static inline void emit_add_w(struct jit_state *j, int rd, int rn, int rm)
{
	emit(j, 0x0B000000 | (rm << 16) | (rn << 5) | rd);
}

/* SUB Wd, Wn, Wm  (32-bit sub) */
static inline void emit_sub_w(struct jit_state *j, int rd, int rn, int rm)
{
	emit(j, 0x4B000000 | (rm << 16) | (rn << 5) | rd);
}

/* ADD Wd, Wn, #imm12 */
static inline void emit_add_w_imm12(struct jit_state *j, int rd, int rn, uint32_t imm12)
{
	emit(j, 0x11000000 | (imm12 << 10) | (rn << 5) | rd);
}

/* MOVZ Wd, #imm16 */
static inline void emit_movz_w(struct jit_state *j, int rd, uint16_t imm)
{
	emit(j, 0x52800000 | ((uint32_t)imm << 5) | rd);
}

/* ADD Wd, Wn, #imm (signed, picks ADDW/SUBW/load) */
static void emit_add_w_simm(struct jit_state *j, int rd, int rn, int32_t imm)
{
	if (imm >= 0 && imm <= 4095) {
		emit_add_w_imm12(j, rd, rn, (uint32_t)imm);
	} else if (imm < 0 && imm >= -4095) {
		/* SUB Wd, Wn, #(-imm) */
		emit(j, 0x51000000 | ((uint32_t)(-imm) << 10) | (rn << 5) | rd);
	} else {
		emit_movz_w(j, 4, (uint16_t)(imm & 0xFFFF));
		if (imm >> 16)
			emit(j, 0x72A00000 | ((uint32_t)((imm >> 16) & 0xFFFF) << 5) | 4); /* MOVK Wd, #hi, LSL 16 */
		emit_add_w(j, rd, rn, 4);
	}
}

/* ---- Additional emitters for inline TLB fast-path ---- */

/* LSR Xd, Xn, #shift  (UBFM Xd, Xn, #shift, #63) */
static inline void emit_lsr_imm(struct jit_state *j, int rd, int rn, int shift)
{
	emit(j, 0xD340FC00 | (shift << 16) | (rn << 5) | rd);
}

/* LSL Xd, Xn, #shift  (UBFM Xd, Xn, #(64-shift), #(63-shift)) */
static inline void emit_lsl_imm(struct jit_state *j, int rd, int rn, int shift)
{
	int immr = (64 - shift) & 63;
	int imms = 63 - shift;
	emit(j, 0xD3400000 | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}

/* AND Xd, Xn, #mask — encoded as logical immediate.
 * Only supports simple masks that are contiguous bit runs.
 * For TLB_INDEX_MASK (0xFFF = 12 bits), N=1, immr=0, imms=11. */
static inline void emit_and_imm(struct jit_state *j, int rd, int rn, int bits)
{
	/* AND Xd, Xn, #((1 << bits) - 1) = UBFX-like bitmask
	 * Encoding: N=1, immr=0, imms=bits-1 */
	uint32_t imms = bits - 1;
	emit(j, 0x92400000 | (imms << 10) | (rn << 5) | rd);
}

/* LDRB Wd, [Xn, #uimm12] — unsigned byte load */
static inline void emit_ldrb_uoff(struct jit_state *j, int rt, int rn, int off)
{
	emit(j, 0x39400000 | (off << 10) | (rn << 5) | rt);
}

/* LDRH Wd, [Xn, #uimm12*2] — unsigned halfword load */
static inline void emit_ldrh_uoff(struct jit_state *j, int rt, int rn, int off)
{
	emit(j, 0x79400000 | ((off / 2) << 10) | (rn << 5) | rt);
}

/* LDR Wd, [Xn, #uimm12*4] — unsigned word load */
static inline void emit_ldr_w_uoff(struct jit_state *j, int rt, int rn, int off)
{
	emit(j, 0xB9400000 | ((off / 4) << 10) | (rn << 5) | rt);
}

/* LDR Xd, [Xn, Xm] — register offset, no scale */
static inline void emit_ldr_x_reg(struct jit_state *j, int rt, int rn, int rm)
{
	/* LDR Xt, [Xn, Xm, LSL #0] = LDR Xt, [Xn, Xm] */
	emit(j, 0xF8606800 | (rm << 16) | (rn << 5) | rt);
}

/* LDRB Wd, [Xn, Xm] — register offset byte load */
static inline void emit_ldrb_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0x38606800 | (rm << 16) | (rn << 5) | rt);
}

/* LDRH Wd, [Xn, Xm] — register offset halfword load */
static inline void emit_ldrh_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0x78606800 | (rm << 16) | (rn << 5) | rt);
}

/* LDR Wd, [Xn, Xm] — register offset word load */
static inline void emit_ldr_w_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0xB8606800 | (rm << 16) | (rn << 5) | rt);
}

/* STR Xd, [Xn, Xm] — register offset store */
static inline void emit_str_x_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0xF8206800 | (rm << 16) | (rn << 5) | rt);
}

/* STRB Wd, [Xn, Xm] — register offset byte store */
static inline void emit_strb_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0x38206800 | (rm << 16) | (rn << 5) | rt);
}

/* STRH Wd, [Xn, Xm] — register offset halfword store */
static inline void emit_strh_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0x78206800 | (rm << 16) | (rn << 5) | rt);
}

/* STR Wd, [Xn, Xm] — register offset word store */
static inline void emit_str_w_reg(struct jit_state *j, int rt, int rn, int rm)
{
	emit(j, 0xB8206800 | (rm << 16) | (rn << 5) | rt);
}

/* CBZ Xn, offset (offset in bytes, ±1MB) */
static inline void emit_cbz_x(struct jit_state *j, int rn, int32_t off_bytes)
{
	emit(j, 0xB4000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | rn);
}

/* CBNZ Xn, offset (offset in bytes, ±1MB) */
static inline void emit_cbnz_x(struct jit_state *j, int rn, int32_t off_bytes)
{
	emit(j, 0xB5000000 | (((off_bytes / 4) & 0x7FFFF) << 5) | rn);
}

/* ================================================================
 * Section 2: Guest register access helpers
 * ================================================================ */

#define CPU_REG     19  /* ARM64 x19 = pinned cpu pointer */
#define XOFF(i)     ((int)(i) * 8)   /* offset of cpu->x[i] */
#define OFF_PC      ((int)offsetof(struct rv64_cpu, pc))
#define OFF_INSTRET ((int)offsetof(struct rv64_cpu, instret))
#define OFF_CYCLE   ((int)offsetof(struct rv64_cpu, cycle))

/* Load guest register rs into ARM64 xN */
static inline void emit_load_guest(struct jit_state *j, int arm_rd, uint32_t guest_rs)
{
	if (guest_rs == 0) {
		/* RISC-V x0 is always zero */
		emit_mov_imm64(j, arm_rd, 0);
	} else {
		emit_ldr_x(j, arm_rd, CPU_REG, XOFF(guest_rs));
	}
}

/* Store ARM64 xN to guest register rd (NOP if rd == 0) */
static inline void emit_store_guest(struct jit_state *j, int arm_rs, uint32_t guest_rd)
{
	if (guest_rd != 0)
		emit_str_x(j, arm_rs, CPU_REG, XOFF(guest_rd));
}

/* ================================================================
 * Section 3: Prologue / epilogue emission
 * ================================================================ */

/*
 * Prologue layout (48 bytes of stack):
 *   [sp + 0]  = saved x19
 *   [sp + 8]  = saved x30 (LR)
 *   [sp + 16] = saved x20 (unused, for STP alignment)
 *   [sp + 24] = saved x21
 *   [sp + 32] = 16 bytes scratch for load-value output
 */
static void emit_prologue(struct jit_state *j)
{
	emit(j, 0xA9BD7BF3); /* STP x19, x30, [sp, #-48]! */
	emit(j, 0xA90157F4); /* STP x20, x21, [sp, #16]   */
	emit_mov(j, CPU_REG, 0);         /* MOV x19, x0 (pin cpu ptr)  */
	emit_movz_w(j, 21, 0);           /* MOV w21, #0 (insn counter) */
}

/*
 * Epilogue: update instret/cycle, return insn count in w0.
 * Returns the word-offset where the epilogue starts (for patching branches).
 */
static uint32_t emit_epilogue(struct jit_state *j)
{
	uint32_t epi_off = j->code_off;

	/* cpu->instret += x21 */
	emit_ldr_x(j, 0, CPU_REG, OFF_INSTRET);
	emit_add(j, 0, 0, 21);
	emit_str_x(j, 0, CPU_REG, OFF_INSTRET);

	/* cpu->cycle += x21 */
	emit_ldr_x(j, 0, CPU_REG, OFF_CYCLE);
	emit_add(j, 0, 0, 21);
	emit_str_x(j, 0, CPU_REG, OFF_CYCLE);

	/* cpu->prof.exec_cycles += x21
	 * The prof struct is at a large offset (>32K) from cpu base,
	 * so we cannot use LDR/STR with immediate offset (12-bit max).
	 * Instead, compute the full address and use offset 0. */
	int off_exec = (int)offsetof(struct rv64_cpu, prof.exec_cycles);
	emit_mov_imm64(j, 2, (uint64_t)(int64_t)off_exec);
	emit_add(j, 2, CPU_REG, 2);         /* x2 = &cpu->prof.exec_cycles */
	emit_ldr_x(j, 0, 2, 0);
	emit_add(j, 0, 0, 21);
	emit_str_x(j, 0, 2, 0);

	/* return value = w21 (insn count) */
	emit_mov(j, 0, 21);

	/* Restore and return */
	emit(j, 0xA94157F4); /* LDP x20, x21, [sp, #16]  */
	emit(j, 0xA8C37BF3); /* LDP x19, x30, [sp], #48  */
	emit(j, 0xD65F03C0); /* RET                       */

	return epi_off;
}

/* ================================================================
 * Section 4: Per-instruction JIT compilers
 * ================================================================ */

static int64_t sign_extend(uint64_t val, int b)
{
	uint64_t m = 1ULL << (b - 1);
	return (int64_t)((val ^ m) - m);
}

/* R-type ALU: opcode 0x33 */
static int jit_op(struct jit_state *j, uint32_t rd, uint32_t rs1,
                  uint32_t rs2, uint32_t funct3, uint32_t funct7)
{
	if (rd == 0) return 1; /* NOP */
	emit_load_guest(j, 0, rs1);
	emit_load_guest(j, 1, rs2);

	if (funct7 == 0x01) { /* M extension */
		switch (funct3) {
		case 0: emit(j, 0x9B017C00); break; /* MUL  x0, x0, x1  */
		case 1: emit(j, 0x9B417C00); break; /* SMULH x0, x0, x1 */
		case 3: emit(j, 0x9BC17C00); break; /* UMULH x0, x0, x1 */
		case 4: /* SDIV — ARM64 gives 0 for /0, RISC-V gives -1 */
			emit_cmp(j, 1, 31); /* cmp x1, xzr (check divisor) */
			/* Handle x0/0 → -1: if x1==0, set x0=-1, skip div */
			/* emit CBZ x1, +12 (skip SDIV, go to MOVN) ... too complex.
			 * Fall back for now. */
			return 0;
		case 5: return 0; /* DIVU */
		case 6: return 0; /* REM */
		case 7: return 0; /* REMU */
		case 2: return 0; /* MULHSU — no ARM64 equiv */
		default: return 0;
		}
	} else {
		switch (funct3) {
		case 0: /* ADD/SUB */
			if (funct7 == 0x00)      emit_add(j, 0, 0, 1);
			else if (funct7 == 0x20) emit_sub(j, 0, 0, 1);
			else return 0;
			break;
		case 1: /* SLL */
			emit(j, 0x9AC12000); /* LSLV x0, x0, x1 */
			break;
		case 2: /* SLT */
			emit_cmp(j, 0, 1);
			emit_cset(j, 0, A64_GE); /* CSET x0, LT (inv=GE) */
			break;
		case 3: /* SLTU */
			emit_cmp(j, 0, 1);
			emit_cset(j, 0, A64_HS); /* CSET x0, LO (inv=HS) */
			break;
		case 4: /* XOR */
			emit(j, 0xCA010000); /* EOR x0, x0, x1 */
			break;
		case 5: /* SRL/SRA */
			if (funct7 == 0x00)      emit(j, 0x9AC12400); /* LSRV */
			else if (funct7 == 0x20) emit(j, 0x9AC12800); /* ASRV */
			else return 0;
			break;
		case 6: /* OR */
			emit(j, 0xAA010000); /* ORR x0, x0, x1 */
			break;
		case 7: /* AND */
			emit(j, 0x8A010000); /* AND x0, x0, x1 */
			break;
		}
	}
	emit_store_guest(j, 0, rd);
	return 1;
}

/* I-type ALU: opcode 0x13 */
static int jit_op_imm(struct jit_state *j, uint32_t insn,
                      uint32_t rd, uint32_t rs1, uint32_t funct3)
{
	if (rd == 0) return 1;
	int64_t imm_i = sign_extend(insn >> 20, 12);
	uint32_t shamt = (insn >> 20) & 0x3F;

	emit_load_guest(j, 0, rs1);

	switch (funct3) {
	case 0: /* ADDI */
		emit_add_simm(j, 0, 0, imm_i);
		break;
	case 2: /* SLTI */
		emit_mov_imm64(j, 1, (uint64_t)imm_i);
		emit_cmp(j, 0, 1);
		emit_cset(j, 0, A64_GE);
		break;
	case 3: /* SLTIU */
		emit_mov_imm64(j, 1, (uint64_t)imm_i);
		emit_cmp(j, 0, 1);
		emit_cset(j, 0, A64_HS);
		break;
	case 4: /* XORI */
		if (imm_i == -1) {
			/* MVN x0, x0  (= ORN x0, XZR, x0) */
			emit(j, 0xAA200000 | (0 << 16) | (31 << 5) | 0);
		} else {
			emit_mov_imm64(j, 1, (uint64_t)imm_i);
			emit(j, 0xCA010000); /* EOR x0, x0, x1 */
		}
		break;
	case 6: /* ORI */
		emit_mov_imm64(j, 1, (uint64_t)imm_i);
		emit(j, 0xAA010000); /* ORR x0, x0, x1 */
		break;
	case 7: /* ANDI */
		emit_mov_imm64(j, 1, (uint64_t)imm_i);
		emit(j, 0x8A010000); /* AND x0, x0, x1 */
		break;
	case 1: /* SLLI */
		/* LSL Xd, Xn, #shamt = UBFM Xd, Xn, #(64-shamt), #(63-shamt) */
		if (shamt == 0) break;
		{
			uint32_t immr = (64 - shamt) & 63;
			uint32_t imms = 63 - shamt;
			emit(j, 0xD3400000 | (immr << 16) | (imms << 10) | (0 << 5) | 0);
		}
		break;
	case 5: /* SRLI/SRAI */
		if (shamt == 0) break;
		if (insn & (1U << 30)) {
			/* SRAI: ASR Xd, Xn, #shamt = SBFM Xd, Xn, #shamt, #63 */
			emit(j, 0x9340FC00 | (shamt << 16) | (0 << 5) | 0);
		} else {
			/* SRLI: LSR Xd, Xn, #shamt = UBFM Xd, Xn, #shamt, #63 */
			emit(j, 0xD340FC00 | (shamt << 16) | (0 << 5) | 0);
		}
		break;
	}
	emit_store_guest(j, 0, rd);
	return 1;
}

/* R-type 32-bit ALU: opcode 0x3B */
static int jit_op32(struct jit_state *j, uint32_t rd, uint32_t rs1,
                    uint32_t rs2, uint32_t funct3, uint32_t funct7)
{
	if (rd == 0) return 1;
	emit_load_guest(j, 0, rs1);
	emit_load_guest(j, 1, rs2);

	if (funct7 == 0x01) { /* M extension 32-bit */
		switch (funct3) {
		case 0: /* MULW */
			emit(j, 0x1B017C00); /* MUL w0, w0, w1 */
			break;
		case 4: return 0; /* DIVW */
		case 5: return 0; /* DIVUW */
		case 6: return 0; /* REMW */
		case 7: return 0; /* REMUW */
		default: return 0;
		}
	} else {
		switch (funct3) {
		case 0: /* ADDW/SUBW */
			if (funct7 == 0x00)      emit_add_w(j, 0, 0, 1);
			else if (funct7 == 0x20) emit_sub_w(j, 0, 0, 1);
			else return 0;
			break;
		case 1: /* SLLW */
			emit(j, 0x1AC12000); /* LSLV w0, w0, w1 */
			break;
		case 5: /* SRLW/SRAW */
			if (funct7 == 0x00)      emit(j, 0x1AC12400); /* LSRV w0, w0, w1 */
			else if (funct7 == 0x20) emit(j, 0x1AC12800); /* ASRV w0, w0, w1 */
			else return 0;
			break;
		default: return 0;
		}
	}
	emit_sxtw(j, 0, 0); /* sign-extend 32-bit result */
	emit_store_guest(j, 0, rd);
	return 1;
}

/* I-type 32-bit ALU: opcode 0x1B */
static int jit_op_imm32(struct jit_state *j, uint32_t insn,
                        uint32_t rd, uint32_t rs1, uint32_t funct3)
{
	if (rd == 0) return 1;
	int32_t imm_i = (int32_t)((insn >> 20) << 20) >> 20; /* sign-extend 12b */
	uint32_t shamt = (insn >> 20) & 0x1F;

	emit_load_guest(j, 0, rs1);

	switch (funct3) {
	case 0: /* ADDIW */
		emit_add_w_simm(j, 0, 0, imm_i);
		break;
	case 1: /* SLLIW */
		if (shamt) {
			/* LSL Wd, Wn, #shamt = UBFM Wd, Wn, #(32-shamt), #(31-shamt) */
			uint32_t immr = (32 - shamt) & 31;
			uint32_t imms = 31 - shamt;
			emit(j, 0x53000000 | (immr << 16) | (imms << 10) | (0 << 5) | 0);
		}
		break;
	case 5: /* SRLIW/SRAIW */
		if (shamt) {
			if (insn & (1U << 30)) {
				/* SRAIW: ASR Wd, Wn, #shamt = SBFM Wd, Wn, #shamt, #31 */
				emit(j, 0x13007C00 | (shamt << 16) | (0 << 5) | 0);
			} else {
				/* SRLIW: LSR Wd, Wn, #shamt = UBFM Wd, Wn, #shamt, #31 */
				emit(j, 0x53007C00 | (shamt << 16) | (0 << 5) | 0);
			}
		}
		break;
	default: return 0;
	}
	emit_sxtw(j, 0, 0);
	emit_store_guest(j, 0, rd);
	return 1;
}

/* LUI: opcode 0x37 */
static int jit_lui(struct jit_state *j, uint32_t insn, uint32_t rd)
{
	if (rd == 0) return 1;
	int64_t imm = sign_extend(insn & 0xFFFFF000ULL, 32);
	emit_mov_imm64(j, 0, (uint64_t)imm);
	emit_store_guest(j, 0, rd);
	return 1;
}

/* AUIPC: opcode 0x17 */
static int jit_auipc(struct jit_state *j, uint32_t insn, uint32_t rd,
                     uint64_t pc)
{
	if (rd == 0) return 1;
	int64_t imm = sign_extend(insn & 0xFFFFF000ULL, 32);
	emit_mov_imm64(j, 0, pc + (uint64_t)imm);
	emit_store_guest(j, 0, rd);
	return 1;
}

/* JAL: opcode 0x6F — block terminator */
static int jit_jal(struct jit_state *j, uint32_t insn, uint32_t rd,
                   uint64_t pc, int insn_size)
{
	int64_t imm_j = sign_extend(
		(((insn >> 31) & 1) << 20) |
		(((insn >> 12) & 0xFF) << 12) |
		(((insn >> 20) & 1) << 11) |
		(((insn >> 21) & 0x3FF) << 1), 21);

	/* rd = pc + insn_size (return address) */
	if (rd != 0) {
		emit_mov_imm64(j, 0, pc + insn_size);
		emit_store_guest(j, 0, rd);
	}
	/* cpu->pc = pc + imm */
	emit_mov_imm64(j, 0, pc + (uint64_t)imm_j);
	emit_str_x(j, 0, CPU_REG, OFF_PC);
	return 1;
}

/* JALR: opcode 0x67 — block terminator */
static int jit_jalr(struct jit_state *j, uint32_t insn, uint32_t rd,
                    uint32_t rs1, uint64_t pc, int insn_size)
{
	int64_t imm = sign_extend(insn >> 20, 12);

	/* Save link address first (in case rd == rs1) */
	if (rd != 0) {
		emit_mov_imm64(j, 2, pc + insn_size);
	}
	/* target = (rs1 + imm) & ~1 */
	emit_load_guest(j, 0, rs1);
	emit_add_simm(j, 0, 0, imm);
	/* AND x0, x0, #~1  — clear bit 0 */
	/* BIC x0, x0, #1  = AND x0, x0, #-2 */
	emit_mov_imm64(j, 1, ~1ULL);
	emit(j, 0x8A010000); /* AND x0, x0, x1 */
	emit_str_x(j, 0, CPU_REG, OFF_PC);

	if (rd != 0)
		emit_store_guest(j, 2, rd);
	return 1;
}

/* BRANCH: opcode 0x63 — block terminator */
static int jit_branch(struct jit_state *j, uint32_t insn, uint32_t rs1,
                      uint32_t rs2, uint32_t funct3, uint64_t pc,
                      int insn_size, uint32_t epi_off)
{
	int64_t imm_b = sign_extend(
		(((insn >> 31) & 1) << 12) |
		(((insn >> 7) & 1) << 11) |
		(((insn >> 25) & 0x3F) << 5) |
		(((insn >> 8) & 0xF) << 1), 13);

	emit_load_guest(j, 0, rs1);
	emit_load_guest(j, 1, rs2);
	emit_cmp(j, 0, 1);

	/*
	 * Map RISC-V branch condition to ARM64 condition code.
	 * We emit: B.cond taken_path (skip not-taken setup).
	 */
	int a64_cond;
	switch (funct3) {
	case 0: a64_cond = A64_EQ; break; /* BEQ */
	case 1: a64_cond = A64_NE; break; /* BNE */
	case 4: a64_cond = A64_LT; break; /* BLT */
	case 5: a64_cond = A64_GE; break; /* BGE */
	case 6: a64_cond = A64_LO; break; /* BLTU */
	case 7: a64_cond = A64_HS; break; /* BGEU */
	default: return 0;
	}

	/*
	 * Layout:
	 *   CMP ...
	 *   B.cond +16  (to taken_path)
	 *   ; not-taken: set pc = next_pc, branch to epilogue
	 *   MOV-imm pc+insn_size  (1-4 insns, ~12 bytes typ)
	 *   STR [cpu->pc]
	 *   B epilogue
	 *   ; taken:
	 *   MOV-imm pc+imm_b
	 *   STR [cpu->pc]
	 *   B epilogue
	 *
	 * But we don't know the size of the not-taken path yet.
	 * Use a two-pass approach: emit not-taken unconditionally,
	 * then patch the conditional branch offset.
	 */
	uint32_t bcond_off = j->code_off; /* remember B.cond position */
	emit(j, 0); /* placeholder for B.cond */

	/* Not-taken path: cpu->pc = pc + insn_size */
	emit_mov_imm64(j, 0, pc + insn_size);
	emit_str_x(j, 0, CPU_REG, OFF_PC);
	/* B epilogue */
	int32_t epi_dist = (int32_t)(epi_off - j->code_off) * 4;
	emit_b(j, epi_dist);

	/* Taken path starts here — patch the B.cond */
	int32_t taken_dist = (int32_t)(j->code_off - bcond_off) * 4;
	j->code_buf[bcond_off] = 0x54000000 |
		(((taken_dist / 4) & 0x7FFFF) << 5) | a64_cond;

	/* cpu->pc = pc + imm_b */
	emit_mov_imm64(j, 0, pc + (uint64_t)imm_b);
	emit_str_x(j, 0, CPU_REG, OFF_PC);
	/* B epilogue */
	epi_dist = (int32_t)(epi_off - j->code_off) * 4;
	emit_b(j, epi_dist);

	return 1;
}

/* Helper function call declarations from rv64_cpu.c */
int rv64_jit_load(struct rv64_cpu *cpu, uint64_t vaddr, int size,
                  uint64_t *val);
int rv64_jit_store(struct rv64_cpu *cpu, uint64_t vaddr, int size,
                   uint64_t val);

/* LOAD: opcode 0x03
 * Inline TLB fast-path: on hit, loads directly from host memory
 * without any C function call. Falls back to rv64_jit_load on miss. */
static int jit_load(struct jit_state *j, uint32_t insn, uint32_t rd,
                    uint32_t rs1, uint32_t funct3, uint64_t pc)
{
	int64_t imm = sign_extend(insn >> 20, 12);
	int size;
	int do_sext = 0;
	int sext_bits = 0;

	switch (funct3) {
	case 0: size = 1; do_sext = 1; sext_bits = 8;  break; /* LB */
	case 1: size = 2; do_sext = 1; sext_bits = 16; break; /* LH */
	case 2: size = 4; do_sext = 1; sext_bits = 32; break; /* LW */
	case 3: size = 8; break;                                /* LD */
	case 4: size = 1; break;                                /* LBU */
	case 5: size = 2; break;                                /* LHU */
	case 6: size = 4; break;                                /* LWU */
	default: return 0;
	}

	/* Store current guest PC for exception handling (needed by slow path) */
	emit_mov_imm64(j, 0, pc);
	emit_str_x(j, 0, CPU_REG, OFF_PC);

	/* x1 = vaddr = cpu->x[rs1] + imm */
	emit_load_guest(j, 1, rs1);
	emit_add_simm(j, 1, 1, imm);

	/* ---- Inline TLB fast-path ----
	 * Layout: tlb_entry is 32 bytes at cpu+888
	 *   [+0]  vpn (8 bytes)
	 *   [+16] host_base (8 bytes)
	 *   [+27] valid (1 byte)
	 *
	 * Register usage:
	 *   x1 = vaddr (preserved for slow path)
	 *   x5 = vpn = vaddr >> 12
	 *   x6 = &tlb_entry = cpu + 888 + (vpn & 0xFFF) * 32
	 *   x7 = loaded vpn tag
	 *   x8 = host_base
	 *   x9 = page_offset = vaddr & 0xFFF
	 *   x0 = loaded value (result)
	 */
	#define OFF_TLB 888

	emit_lsr_imm(j, 5, 1, PAGE_SHIFT);       /* x5 = vaddr >> 12 = VPN */
	emit_and_imm(j, 6, 5, 12);               /* x6 = VPN & 0xFFF (TLB index) */
	emit_lsl_imm(j, 6, 6, 5);                /* x6 *= 32 (sizeof tlb_entry) */
	emit_add(j, 6, 6, CPU_REG);              /* x6 += cpu */
	emit_add_imm12(j, 6, 6, OFF_TLB);        /* x6 = &cpu->tlb[idx] */

	/* Load VPN tag from TLB entry and compare */
	emit_ldr_x(j, 7, 6, 0);                  /* x7 = te->vpn */
	emit_cmp(j, 7, 5);                       /* VPN tag match? */
	uint32_t bne_slow = j->code_off;
	emit(j, 0);                               /* placeholder B.NE slow_path */

	/* Check host_base is non-NULL (NULL = MMIO page) */
	emit_ldr_x(j, 8, 6, 16);                 /* x8 = te->host_base */
	uint32_t cbz_slow = j->code_off;
	emit(j, 0);                    	/* Check page-crossing: (vaddr & 0xFFF) + size <= 4096 */
	emit_and_imm(j, 9, 1, 12);               /* x9 = vaddr & 0xFFF (page offset) */
	if (size > 1) {
		emit_add_imm12(j, 10, 9, (uint32_t)size); /* x10 = offset + size */
		emit_mov_imm64(j, 4, PAGE_SIZE);      /* x4 = 4096 */
		emit_cmp(j, 10, 4);
		uint32_t bhi_slow = j->code_off;
		emit(j, 0);                           /* placeholder B.HI slow_path */
		/* We'll patch bhi_slow below */

		/* Direct host memory load: x2 = *(host_base + page_offset) */
		switch (size) {
		case 2: emit_ldrh_reg(j, 2, 8, 9); break;
		case 4: emit_ldr_w_reg(j, 2, 8, 9); break;
		case 8: emit_ldr_x_reg(j, 2, 8, 9); break;
		}

		/* Jump to done (skip slow path) */
		uint32_t b_done = j->code_off;
		emit(j, 0);                            /* placeholder B done */

		/* ---- slow_path label ---- */
		uint32_t slow_off = j->code_off;

		/* Patch forward branches to land here */
		j->code_buf[bne_slow] = 0x54000000 | A64_NE |
			(((slow_off - bne_slow) & 0x7FFFF) << 5);   /* B.NE */
		j->code_buf[cbz_slow] = 0xB4000000 | 8 |
			(((slow_off - cbz_slow) & 0x7FFFF) << 5);   /* CBZ x8 */
		j->code_buf[bhi_slow] = 0x54000000 | 8 |      /* B.HI = cond 8 */
			(((slow_off - bhi_slow) & 0x7FFFF) << 5);

		/* Slow path: call rv64_jit_load(cpu, vaddr, size, &val) */
		emit_mov(j, 0, CPU_REG);
		/* x1 still = vaddr */
		emit_movz_w(j, 2, (uint16_t)size);
		emit_add_imm12(j, 3, 31, 0); /* x3 = sp (&val) - safely inside our 32-byte temp frame */
		emit_mov_imm64(j, 16, (uint64_t)j->fn_load);
		emit_blr(j, 16);

		/* Check exception return */
		uint32_t cbnz_off = j->code_off;
		emit(j, 0);  /* placeholder CBNZ w0, epilogue */

		/* Load value from stack scratch into x2 */
		emit_ldr_x(j, 2, 31, 0);

		/* ---- done label ---- */
		uint32_t done_off = j->code_off;
		j->code_buf[b_done] = 0x14000000 |
			((done_off - b_done) & 0x3FFFFFF);  /* B done */

		if (do_sext) {
			switch (sext_bits) {
			case 8:  emit_sxtb(j, 2, 2); break;
			case 16: emit_sxth(j, 2, 2); break;
			case 32: emit_sxtw(j, 2, 2); break;
			}
		}

		emit_store_guest(j, 2, rd);
		j->code_buf[cbnz_off] = cbnz_off; /* sentinel for epilogue patching */
		return 1 | (cbnz_off << 8);
	}

	/* size == 1: no page-cross possible, simplify */
	emit_ldrb_reg(j, 2, 8, 9);               /* x2 = *(host_base + offset) */

	uint32_t b_done1 = j->code_off;
	emit(j, 0);                                /* placeholder B done */

	/* ---- slow_path for size==1 ---- */
	uint32_t slow_off1 = j->code_off;
	j->code_buf[bne_slow] = 0x54000000 | A64_NE |
		(((slow_off1 - bne_slow) & 0x7FFFF) << 5);
	j->code_buf[cbz_slow] = 0xB4000000 | 8 |
		(((slow_off1 - cbz_slow) & 0x7FFFF) << 5);

	emit_mov(j, 0, CPU_REG);
	emit_movz_w(j, 2, 1);
	emit_add_imm12(j, 3, 31, 0); /* x3 = sp */
	emit_mov_imm64(j, 16, (uint64_t)j->fn_load);
	emit_blr(j, 16);
	uint32_t cbnz_off1 = j->code_off;
	emit(j, 0);
	emit_ldr_x(j, 2, 31, 0); /* loaded into x2 */

	/* ---- done ---- */
	uint32_t done_off1 = j->code_off;
	j->code_buf[b_done1] = 0x14000000 | ((done_off1 - b_done1) & 0x3FFFFFF);

	if (do_sext)
		emit_sxtb(j, 2, 2);

	emit_store_guest(j, 2, rd);
	j->code_buf[cbnz_off1] = cbnz_off1;
	return 1 | (cbnz_off1 << 8);
}

/* STORE: opcode 0x23
 * Inline TLB fast-path: on hit, stores directly to host memory.
 * Falls back to rv64_jit_store on miss or MMIO. */
static int jit_store(struct jit_state *j, uint32_t insn, uint32_t rs1,
                     uint32_t rs2, uint32_t funct3, uint64_t pc)
{
	int32_t imm = sign_extend(((insn >> 25) << 5) | ((insn >> 7) & 0x1F), 12);
	int size;

	switch (funct3) {
	case 0: size = 1; break; /* SB */
	case 1: size = 2; break; /* SH */
	case 2: size = 4; break; /* SW */
	case 3: size = 8; break; /* SD */
	default: return 0;
	}

	/* Store current guest PC for exception handling */
	emit_mov_imm64(j, 0, pc);
	emit_str_x(j, 0, CPU_REG, OFF_PC);

	/* x1 = vaddr = cpu->x[rs1] + imm */
	emit_load_guest(j, 1, rs1);
	emit_add_simm(j, 1, 1, imm);

	/* x2 = val = cpu->x[rs2] */
	emit_load_guest(j, 2, rs2);

	/* ---- Inline TLB fast-path ---- */
	#define OFF_TLB 888

	emit_lsr_imm(j, 5, 1, PAGE_SHIFT);       /* x5 = VPN */
	emit_and_imm(j, 6, 5, 12);               /* x6 = TLB index */
	emit_lsl_imm(j, 6, 6, 5);                /* x6 *= 32 */
	emit_add(j, 6, 6, CPU_REG);
	emit_add_imm12(j, 6, 6, OFF_TLB);        /* x6 = &te */

	/* Check VPN */
	emit_ldr_x(j, 7, 6, 0);                  /* x7 = te->vpn */
	emit_cmp(j, 7, 5);
	uint32_t bne_slow = j->code_off;
	emit(j, 0);

	/* Check host_base */
	emit_ldr_x(j, 8, 6, 16);                 /* x8 = te->host_base */
	uint32_t cbz_slow = j->code_off;
	emit(j, 0);

	/* Check dirty (offset 26). If 0, must take slow-path to set PTE_D. */
	emit_ldrb_uoff(j, 10, 6, 26);
	uint32_t cbz_dirty = j->code_off;
	emit(j, 0);

	/* Check page-crossing */
	emit_and_imm(j, 9, 1, 12);               /* x9 = vaddr & 0xFFF */
	if (size > 1) {
		emit_add_imm12(j, 11, 9, (uint32_t)size);
		emit_mov_imm64(j, 4, PAGE_SIZE);
		emit_cmp(j, 11, 4);
		uint32_t bhi_slow = j->code_off;
		emit(j, 0);

		/* Direct host memory store: *(host_base + page_offset) = x2 */
		switch (size) {
		case 2: emit_strh_reg(j, 2, 8, 9); break;
		case 4: emit_str_w_reg(j, 2, 8, 9); break;
		case 8: emit_str_x_reg(j, 2, 8, 9); break;
		}

		uint32_t b_done = j->code_off;
		emit(j, 0);

		/* ---- slow_path ---- */
		uint32_t slow_off = j->code_off;
		j->code_buf[bne_slow] = 0x54000000 | A64_NE | (((slow_off - bne_slow) & 0x7FFFF) << 5);
		j->code_buf[cbz_slow] = 0xB4000000 | 8 | (((slow_off - cbz_slow) & 0x7FFFF) << 5);
		j->code_buf[cbz_dirty] = 0xB4000000 | 10 | (((slow_off - cbz_dirty) & 0x7FFFF) << 5);
		j->code_buf[bhi_slow] = 0x54000000 | 8 | (((slow_off - bhi_slow) & 0x7FFFF) << 5);

		emit_mov(j, 0, CPU_REG);
		/* x1 = vaddr */
		emit_mov(j, 3, 2); /* x3 = val (which is currently in x2) */
		emit_movz_w(j, 2, (uint16_t)size); /* x2 = size */
		emit_mov_imm64(j, 16, (uint64_t)j->fn_store);
		emit_blr(j, 16);

		uint32_t cbnz_off = j->code_off;
		emit(j, 0); /* CBNZ w0, exception_exit */

		/* ---- done ---- */
		uint32_t done_off = j->code_off;
		j->code_buf[b_done] = 0x14000000 | ((done_off - b_done) & 0x3FFFFFF);

		j->code_buf[cbnz_off] = cbnz_off;
		return 1 | (cbnz_off << 8);
	}

	/* size == 1: no page-cross possible */
	emit_strb_reg(j, 2, 8, 9);

	uint32_t b_done1 = j->code_off;
	emit(j, 0);

	/* ---- slow_path size==1 ---- */
	uint32_t slow_off1 = j->code_off;
	j->code_buf[bne_slow] = 0x54000000 | A64_NE | (((slow_off1 - bne_slow) & 0x7FFFF) << 5);
	j->code_buf[cbz_slow] = 0xB4000000 | 8 | (((slow_off1 - cbz_slow) & 0x7FFFF) << 5);
	j->code_buf[cbz_dirty] = 0xB4000000 | 10 | (((slow_off1 - cbz_dirty) & 0x7FFFF) << 5);

	emit_mov(j, 0, CPU_REG);
	emit_mov(j, 3, 2);
	emit_movz_w(j, 2, 1);
	emit_mov_imm64(j, 16, (uint64_t)j->fn_store);
	emit_blr(j, 16);

	uint32_t cbnz_off1 = j->code_off;
	emit(j, 0);

	/* ---- done ---- */
	uint32_t done_off1 = j->code_off;
	j->code_buf[b_done1] = 0x14000000 | ((done_off1 - b_done1) & 0x3FFFFFF);

	j->code_buf[cbnz_off1] = cbnz_off1;
	return 1 | (cbnz_off1 << 8);
}

/* FENCE: opcode 0x0F — treat as NOP for single-hart */
static int jit_fence(struct jit_state *j, uint32_t insn)
{
	uint32_t funct3 = (insn >> 12) & 7;
	if (funct3 == 1) return 0; /* FENCE.I — must flush JIT, end block */
	/* Regular FENCE is NOP on single-hart */
	return 1;
}

/* ================================================================
 * Section 5: Compressed instruction decompressor
 * ================================================================
 *
 * Converts a 16-bit C-extension instruction to its 32-bit equivalent.
 * Returns 0 if the instruction can't be decompressed (illegal or FP).
 *
 * This is a direct copy of the interpreter's decompressor for correctness.
 */
static uint32_t decompress(uint16_t ci)
{
	uint32_t quad = ci & 3;
	uint32_t funct3c = (ci >> 13) & 7;

	switch (quad) {
	case 0:
		switch (funct3c) {
		case 0: { /* C.ADDI4SPN */
			uint32_t nzuimm = ((ci >> 5) & 1) << 3 |
			                  ((ci >> 6) & 1) << 2 |
			                  ((ci >> 7) & 0xF) << 6 |
			                  ((ci >> 11) & 3) << 4;
			uint32_t rd = ((ci >> 2) & 7) + 8;
			if (nzuimm == 0) return 0;
			return (nzuimm << 20) | (2 << 15) | (0 << 12) | (rd << 7) | 0x13;
		}
		case 2: { /* C.LW */
			uint32_t off = ((ci >> 5) & 1) << 6 |
			               ((ci >> 6) & 1) << 2 |
			               ((ci >> 10) & 7) << 3;
			uint32_t rs1 = ((ci >> 7) & 7) + 8;
			uint32_t rd = ((ci >> 2) & 7) + 8;
			return (off << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03;
		}
		case 3: { /* C.LD */
			uint32_t off = ((ci >> 5) & 3) << 6 |
			               ((ci >> 10) & 7) << 3;
			uint32_t rs1 = ((ci >> 7) & 7) + 8;
			uint32_t rd = ((ci >> 2) & 7) + 8;
			return (off << 20) | (rs1 << 15) | (3 << 12) | (rd << 7) | 0x03;
		}
		case 6: { /* C.SW */
			uint32_t off = ((ci >> 5) & 1) << 6 |
			               ((ci >> 6) & 1) << 2 |
			               ((ci >> 10) & 7) << 3;
			uint32_t rs1 = ((ci >> 7) & 7) + 8;
			uint32_t rs2 = ((ci >> 2) & 7) + 8;
			uint32_t i115 = (off >> 5) & 0x7F;
			uint32_t i40 = off & 0x1F;
			return (i115 << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) | (i40 << 7) | 0x23;
		}
		case 7: { /* C.SD */
			uint32_t off = ((ci >> 5) & 3) << 6 |
			               ((ci >> 10) & 7) << 3;
			uint32_t rs1 = ((ci >> 7) & 7) + 8;
			uint32_t rs2 = ((ci >> 2) & 7) + 8;
			uint32_t i115 = (off >> 5) & 0x7F;
			uint32_t i40 = off & 0x1F;
			return (i115 << 25) | (rs2 << 20) | (rs1 << 15) | (3 << 12) | (i40 << 7) | 0x23;
		}
		default: return 0; /* FP or illegal */
		}

	case 1:
		switch (funct3c) {
		case 0: { /* C.NOP/C.ADDI */
			uint32_t rd = (ci >> 7) & 0x1F;
			int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
			imm = (imm << 26) >> 26;
			return ((imm & 0xFFF) << 20) | (rd << 15) | (0 << 12) | (rd << 7) | 0x13;
		}
		case 1: { /* C.ADDIW */
			uint32_t rd = (ci >> 7) & 0x1F;
			int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
			imm = (imm << 26) >> 26;
			return ((imm & 0xFFF) << 20) | (rd << 15) | (0 << 12) | (rd << 7) | 0x1B;
		}
		case 2: { /* C.LI */
			uint32_t rd = (ci >> 7) & 0x1F;
			int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
			imm = (imm << 26) >> 26;
			return ((imm & 0xFFF) << 20) | (0 << 15) | (0 << 12) | (rd << 7) | 0x13;
		}
		case 3: { /* C.LUI / C.ADDI16SP */
			uint32_t rd = (ci >> 7) & 0x1F;
			if (rd == 2) {
				int32_t imm = (((ci >> 12) & 1) << 9) |
				              (((ci >> 3) & 3) << 7) |
				              (((ci >> 5) & 1) << 6) |
				              (((ci >> 2) & 1) << 5) |
				              (((ci >> 6) & 1) << 4);
				imm = (imm << 22) >> 22;
				return ((imm & 0xFFF) << 20) | (2 << 15) | (0 << 12) | (2 << 7) | 0x13;
			} else {
				int32_t imm = (((ci >> 12) & 1) << 17) | (((ci >> 2) & 0x1F) << 12);
				imm = (imm << 14) >> 14;
				return (imm & 0xFFFFF000U) | (rd << 7) | 0x37;
			}
		}
		case 4: { /* C.SRLI/C.SRAI/C.ANDI/C.SUB etc. */
			uint32_t funct2 = (ci >> 10) & 3;
			uint32_t rd = ((ci >> 7) & 7) + 8;
			uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
			switch (funct2) {
			case 0: return (shamt << 20) | (rd << 15) | (5 << 12) | (rd << 7) | 0x13;
			case 1: return ((0x20 << 5 | shamt) << 20) | (rd << 15) | (5 << 12) | (rd << 7) | 0x13;
			case 2: {
				int32_t imm = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
				imm = (imm << 26) >> 26;
				return ((imm & 0xFFF) << 20) | (rd << 15) | (7 << 12) | (rd << 7) | 0x13;
			}
			case 3: {
				uint32_t rs2 = ((ci >> 2) & 7) + 8;
				uint32_t f1 = (ci >> 12) & 1;
				uint32_t f2b = (ci >> 5) & 3;
				if (f1 == 0) {
					switch (f2b) {
					case 0: return (0x20 << 25) | (rs2 << 20) | (rd << 15) | (0 << 12) | (rd << 7) | 0x33;
					case 1: return (rs2 << 20) | (rd << 15) | (4 << 12) | (rd << 7) | 0x33;
					case 2: return (rs2 << 20) | (rd << 15) | (6 << 12) | (rd << 7) | 0x33;
					case 3: return (rs2 << 20) | (rd << 15) | (7 << 12) | (rd << 7) | 0x33;
					}
				} else {
					switch (f2b) {
					case 0: return (0x20 << 25) | (rs2 << 20) | (rd << 15) | (0 << 12) | (rd << 7) | 0x3B;
					case 1: return (rs2 << 20) | (rd << 15) | (0 << 12) | (rd << 7) | 0x3B;
					default: return 0;
					}
				}
				return 0;
			}
			}
			return 0;
		}
		case 5: { /* C.J */
			int32_t imm = (((ci >> 12) & 1) << 11) |
			              (((ci >> 11) & 1) << 4) |
			              (((ci >> 9) & 3) << 8) |
			              (((ci >> 8) & 1) << 10) |
			              (((ci >> 7) & 1) << 6) |
			              (((ci >> 6) & 1) << 7) |
			              (((ci >> 3) & 7) << 1) |
			              (((ci >> 2) & 1) << 5);
			imm = (imm << 20) >> 20;
			uint32_t jimm = ((imm >> 20) & 1) << 31 |
			                ((imm >> 1) & 0x3FF) << 21 |
			                ((imm >> 11) & 1) << 20 |
			                ((imm >> 12) & 0xFF) << 12;
			return jimm | (0 << 7) | 0x6F;
		}
		case 6: { /* C.BEQZ */
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
			return bimm | (0 << 20) | (rs1 << 15) | (0 << 12) | 0x63;
		}
		case 7: { /* C.BNEZ */
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
			return bimm | (0 << 20) | (rs1 << 15) | (1 << 12) | 0x63;
		}
		}
		return 0;

	case 2:
		switch (funct3c) {
		case 0: { /* C.SLLI */
			uint32_t rd = (ci >> 7) & 0x1F;
			uint32_t shamt = (((ci >> 12) & 1) << 5) | ((ci >> 2) & 0x1F);
			return (shamt << 20) | (rd << 15) | (1 << 12) | (rd << 7) | 0x13;
		}
		case 2: { /* C.LWSP */
			uint32_t rd = (ci >> 7) & 0x1F;
			uint32_t off = (((ci >> 12) & 1) << 5) |
			               (((ci >> 4) & 7) << 2) |
			               (((ci >> 2) & 3) << 6);
			return (off << 20) | (2 << 15) | (2 << 12) | (rd << 7) | 0x03;
		}
		case 3: { /* C.LDSP */
			uint32_t rd = (ci >> 7) & 0x1F;
			uint32_t off = (((ci >> 12) & 1) << 5) |
			               (((ci >> 5) & 3) << 3) |
			               (((ci >> 2) & 7) << 6);
			return (off << 20) | (2 << 15) | (3 << 12) | (rd << 7) | 0x03;
		}
		case 4: { /* C.JR/C.MV/C.JALR/C.ADD */
			uint32_t rd = (ci >> 7) & 0x1F;
			uint32_t rs2 = (ci >> 2) & 0x1F;
			uint32_t bit12 = (ci >> 12) & 1;
			if (bit12 == 0) {
				if (rs2 == 0)
					return (rd << 15) | (0 << 12) | (0 << 7) | 0x67; /* C.JR */
				else
					return (rs2 << 20) | (0 << 15) | (0 << 12) | (rd << 7) | 0x33; /* C.MV */
			} else {
				if (rs2 == 0 && rd == 0)
					return 0x00100073; /* C.EBREAK */
				if (rs2 == 0)
					return (rd << 15) | (0 << 12) | (1 << 7) | 0x67; /* C.JALR */
				return (rs2 << 20) | (rd << 15) | (0 << 12) | (rd << 7) | 0x33; /* C.ADD */
			}
		}
		case 6: { /* C.SWSP */
			uint32_t rs2 = (ci >> 2) & 0x1F;
			uint32_t off = (((ci >> 9) & 0xF) << 2) |
			               (((ci >> 7) & 3) << 6);
			uint32_t i115 = (off >> 5) & 0x7F;
			uint32_t i40 = off & 0x1F;
			return (i115 << 25) | (rs2 << 20) | (2 << 15) | (2 << 12) | (i40 << 7) | 0x23;
		}
		case 7: { /* C.SDSP */
			uint32_t rs2 = (ci >> 2) & 0x1F;
			uint32_t off = (((ci >> 10) & 7) << 3) |
			               (((ci >> 7) & 7) << 6);
			uint32_t i115 = (off >> 5) & 0x7F;
			uint32_t i40 = off & 0x1F;
			return (i115 << 25) | (rs2 << 20) | (2 << 15) | (3 << 12) | (i40 << 7) | 0x23;
		}
		default: return 0; /* FP or illegal */
		}

	default: return 0;
	}
}

/* ================================================================
 * Section 6: Block compiler
 * ================================================================ */

static int jit_fetch_insn(struct rv64_cpu *cpu, uint64_t pc,
                          uint32_t *out_insn, int *out_compressed)
{
	uint64_t val;
	
	/* Use the existing CPU load path (handles page boundaries correctly) */
	if (rv64_cpu_load(cpu, pc, 4, &val, ACCESS_EXEC) == 0) {
		*out_insn = (uint32_t)val;
		*out_compressed = 0;
	} else if (rv64_cpu_load(cpu, pc, 2, &val, ACCESS_EXEC) == 0) {
		*out_insn = (uint32_t)(val & 0xFFFF);
		*out_compressed = 1;
	} else {
		return -1;
	}

	/* Re-evaluate compressed instruction detection just like interpreter */
	if ((*out_insn & 3) != 3) {
		*out_compressed = 1;
		*out_insn &= 0xFFFF;
	}

	return 0;
}

/*
 * Compile a basic block starting at guest_pc.
 * Returns pointer to the cache entry, or NULL on failure.
 */
static struct jit_block *jit_compile_block(struct rv64_cpu *cpu,
                                           struct jit_state *jit,
                                           uint64_t guest_pc)
{
	/* Check space: ensure we have room for at least one block.
	 * Worst case: prologue(4) + JIT_MAX_BLOCK_INSNS * 30 + epilogue(12) */
	uint32_t max_needed = 4 + JIT_MAX_BLOCK_INSNS * 30 + 12;
	if (jit->code_off + max_needed > jit->code_capacity) {
		/* Code buffer full — flush everything and start over */
		jit_flush(jit);
	}

	/* MAP_JIT: switch to write mode for code emission */
	if (!jit->offline_mode)
		pthread_jit_write_protect_np(0);

	uint32_t block_start = jit->code_off;
	uint32_t *block_code = &jit->code_buf[block_start];

	/* Emit prologue */
	emit_prologue(jit);

	/* Fixup list for CBNZ exception exits (load/store faults) */
	uint32_t fixups[JIT_MAX_BLOCK_INSNS];
	int num_fixups = 0;

	uint64_t pc = guest_pc;
	int insn_count = 0;
	int block_ended = 0;
	int has_store = 0; /* set if block contains store instructions */

	/* Track last instruction for deferred branch handling */
	uint32_t last_opcode = 0;
	uint64_t last_pc = 0;
	uint32_t last_raw_insn = 0;
	int last_compressed = 0;

	/* Scan and compile instructions */
	while (insn_count < JIT_MAX_BLOCK_INSNS && !block_ended) {
		uint32_t raw_insn;
		int compressed;

		if (jit_fetch_insn(cpu, pc, &raw_insn, &compressed) < 0)
			break; /* can't fetch — stop */

		uint32_t insn;
		int insn_size;

		if (compressed) {
			insn = decompress((uint16_t)raw_insn);
			if (insn == 0) break; /* can't decompress — stop */
			insn_size = 2;
		} else {
			insn = raw_insn;
			insn_size = 4;
		}

		/* Decode common fields */
		uint32_t opcode = insn & 0x7F;
		uint32_t rd     = (insn >> 7) & 0x1F;
		uint32_t funct3 = (insn >> 12) & 0x7;
		uint32_t rs1    = (insn >> 15) & 0x1F;
		uint32_t rs2    = (insn >> 20) & 0x1F;
		uint32_t funct7 = (insn >> 25) & 0x7F;

		/* Save for deferred handling */
		last_opcode = opcode;
		last_pc = pc;
		last_raw_insn = raw_insn;
		last_compressed = compressed;

		int result = 0;

		switch (opcode) {
		case 0x33: /* OP */
			result = jit_op(jit, rd, rs1, rs2, funct3, funct7);
			break;
		case 0x13: /* OP-IMM */
			result = jit_op_imm(jit, insn, rd, rs1, funct3);
			break;
		case 0x3B: /* OP-32 */
			result = jit_op32(jit, rd, rs1, rs2, funct3, funct7);
			break;
		case 0x1B: /* OP-IMM-32 */
			result = jit_op_imm32(jit, insn, rd, rs1, funct3);
			break;
		case 0x37: /* LUI */
			result = jit_lui(jit, insn, rd);
			break;
		case 0x17: /* AUIPC */
			result = jit_auipc(jit, insn, rd, pc);
			break;
		case 0x0F: /* FENCE */
			result = jit_fence(jit, insn);
			break;
		case 0x03: /* LOAD */
			result = jit_load(jit, insn, rd, rs1, funct3, pc);
			if (result & 1) {
				uint32_t fx = (uint32_t)result >> 8;
				if (fx && num_fixups < JIT_MAX_BLOCK_INSNS)
					fixups[num_fixups++] = fx;
				result = 1;
			}
			break;
		case 0x23: /* STORE */
			result = jit_store(jit, insn, rs1, rs2, funct3, pc);
			if (result & 1) {
				uint32_t fx = (uint32_t)result >> 8;
				if (fx && num_fixups < JIT_MAX_BLOCK_INSNS)
					fixups[num_fixups++] = fx;
				result = 1;
				has_store = 1;
			}
			break;

		/* Block terminators: branches and jumps */
		case 0x6F: /* JAL */
			result = jit_jal(jit, insn, rd, pc, insn_size);
			block_ended = 1;
			break;
		case 0x67: /* JALR */
			result = jit_jalr(jit, insn, rd, rs1, pc, insn_size);
			block_ended = 1;
			break;
		case 0x63: /* BRANCH — deferred, needs epilogue offset */
			block_ended = 1;
			result = -2;
			break;

		default:
			result = 0; /* can't compile */
			break;
		}

		if (result == -2) {
			/* Deferred branch: emit after we know epilogue offset */
			insn_count++;
			break;
		}

		if (result == 0)
			break; /* can't compile this instruction */

		/* Increment guest instruction counter: ADD w21, w21, #1 */
		emit(jit, 0x11000400 | (21 << 5) | 21);

		insn_count++;

		if (block_ended) {
			/* Terminal instruction already set cpu->pc. */
			break;
		}

		pc = pc + insn_size;
	}

	if (insn_count == 0) {
		/* Couldn't compile anything — revert */
		jit->code_off = block_start;
		pthread_jit_write_protect_np(1); /* back to execute mode */
		return NULL;
	}

	/* For non-terminal blocks (ended without branch/jump):
	 * set cpu->pc to the next instruction */
	if (!block_ended) {
		emit_mov_imm64(jit, 0, pc);
		emit_str_x(jit, 0, CPU_REG, OFF_PC);
	}

	/* Handle deferred branch: re-fetch the branch instruction and
	 * emit it with a forward reference to the epilogue, then emit
	 * the epilogue, then patch the forward references. */
	if (block_ended && last_opcode == 0x63) {
		uint32_t br_insn;
		int br_compressed;

		if (jit_fetch_insn(cpu, last_pc, &br_insn, &br_compressed) == 0) {
			uint32_t exp_insn = br_compressed ?
				decompress((uint16_t)br_insn) : br_insn;
			int br_size = br_compressed ? 2 : 4;

			/* Increment insn count for the branch itself */
			emit(jit, 0x11000400 | (21 << 5) | 21);

			/* Emit branch code with a dummy epilogue target.
			 * We'll patch the B instructions afterward. */
			uint32_t branch_start = jit->code_off;
			uint32_t dummy_epi = jit->code_off + 50;
			jit_branch(jit, exp_insn,
			           (exp_insn >> 15) & 0x1F,
			           (exp_insn >> 20) & 0x1F,
			           (exp_insn >> 12) & 0x7,
			           last_pc, br_size, dummy_epi);

			/* Emit the real epilogue */
			uint32_t epi_off = emit_epilogue(jit);

			/* Patch B instructions that targeted dummy_epi */
			for (uint32_t p = branch_start; p < epi_off; p++) {
				uint32_t w = jit->code_buf[p];
				if ((w & 0xFC000000) == 0x14000000) {
					int32_t old_off = (int32_t)(w << 6) >> 6;
					uint32_t old_target = (uint32_t)((int32_t)p + old_off);
					if (old_target == dummy_epi) {
						int32_t new_off = (int32_t)epi_off - (int32_t)p;
						jit->code_buf[p] = 0x14000000 | (new_off & 0x3FFFFFF);
					}
				}
			}

			/* Patch CBNZ fixups to point to epilogue */
			for (int i = 0; i < num_fixups; i++) {
				uint32_t fx = fixups[i];
				if (fx < jit->code_capacity && jit->code_buf[fx] == fx) {
					int32_t off = (int32_t)(epi_off - fx) * 4;
					jit->code_buf[fx] = 0x35000000 |
						(((off / 4) & 0x7FFFF) << 5) | 0;
				}
			}
			num_fixups = 0; /* already patched */

			/* Flush icache and switch to execute mode */
			uint32_t block_size_bytes = (jit->code_off - block_start) * 4;
			if (!jit->offline_mode) {
				sys_icache_invalidate(block_code, block_size_bytes);
				pthread_jit_write_protect_np(1); /* switch to execute mode */
			}
			/* Insert into cache */
			unsigned int idx = (guest_pc >> 1) & JIT_CACHE_MASK;
			struct jit_block *b = &jit->cache[idx];
			b->guest_pc = guest_pc;
			b->host_code = block_code;
			b->insn_count = insn_count;
			b->has_store = has_store;
			b->valid = 1;
			return b;
		}
		/* Fetch failed — fall through to non-branch path */
	}

	/* Emit epilogue */
	uint32_t epi_off = emit_epilogue(jit);

	/* Patch all CBNZ fixups to point to epilogue */
	for (int i = 0; i < num_fixups; i++) {
		uint32_t fx = fixups[i];
		if (fx < jit->code_capacity && jit->code_buf[fx] == fx) {
			int32_t off = (int32_t)(epi_off - fx) * 4;
			jit->code_buf[fx] = 0x35000000 |
				(((off / 4) & 0x7FFFF) << 5) | 0;
		}
	}

	/* Flush icache and switch to execute mode */
	uint32_t block_size_bytes = (jit->code_off - block_start) * 4;
	if (!jit->offline_mode) {
		sys_icache_invalidate(block_code, block_size_bytes);
		pthread_jit_write_protect_np(1); /* switch to execute mode */
	}

	/* Insert into cache */
	unsigned int idx = (guest_pc >> 1) & JIT_CACHE_MASK;
	struct jit_block *b = &jit->cache[idx];
	b->guest_pc = guest_pc;
	b->host_code = block_code;
	b->insn_count = insn_count;
	b->has_store = has_store;
	b->valid = 1;

	return b;
}

/* ================================================================
 * Section 7: Public API
 * ================================================================ */

int jit_init(struct jit_state *jit)
{
	memset(jit, 0, sizeof(*jit));

	/* Allocate executable code buffer with MAP_JIT.
	 * Apple Silicon requires MAP_JIT for simultaneous W+X.
	 * Plain RWX mmap silently downgrades to RX on ARM64 macOS.
	 *
	 * After mmap with MAP_JIT, the buffer starts in execute mode.
	 * We switch to write mode and leave it there — the ARM64 icache
	 * invalidation via sys_icache_invalidate ensures coherency
	 * without needing to toggle W^X per compilation. */
	void *buf = mmap(NULL, JIT_CODE_BUF_SIZE,
	                 PROT_READ | PROT_WRITE | PROT_EXEC,
	                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT,
	                 -1, 0);

	if (buf == MAP_FAILED) {
		jit->enabled = 0;
		return -1;
	}

	/* Switch from execute-only to writable, then back to execute.
	 * This round-trip ensures the per-thread W^X state is properly
	 * initialized. The buffer starts in execute mode, ready for use.
	 * jit_compile_block toggles to write mode as needed. */
	pthread_jit_write_protect_np(0);
	pthread_jit_write_protect_np(1);

	jit->code_buf = (uint32_t *)buf;
	jit->code_off = 0;
	jit->code_capacity = JIT_CODE_BUF_SIZE / 4;
	jit->enabled = 1;

	/* Capture helper function addresses */
	jit->fn_load = (void *)rv64_jit_load;
	jit->fn_store = (void *)rv64_jit_store;

	return 0;
}

uint32_t jit_compile_offline(struct rv64_cpu *cpu, struct jit_state *jit, uint64_t pc, struct jit_block *out_b)
{
	/* Reset buffer offset for this isolated run */
	jit->code_off = 0;
	jit->offline_mode = 1;

	struct jit_block *b = jit_compile_block(cpu, jit, pc);
	if (!b) return 0;
	
	*out_b = *b;
	return jit->code_off; /* Number of 32-bit words generated */
}

void jit_destroy(struct jit_state *jit)
{
	if (jit->code_buf) {
		munmap(jit->code_buf, JIT_CODE_BUF_SIZE);
		jit->code_buf = NULL;
	}
	jit->enabled = 0;
}

void jit_flush(struct jit_state *jit)
{
	memset(jit->cache, 0, sizeof(jit->cache));
	jit->code_off = 0;
}

/* Invalidate cache entries without resetting code buffer.
 * Used on TLB flush (sfence.vma) — page table changed but we
 * keep the code buffer intact to avoid recompilation storms. */
void jit_invalidate(struct jit_state *jit)
{
	for (int i = 0; i < JIT_CACHE_SIZE; i++)
		jit->cache[i].valid = 0;
}

int jit_exec(struct rv64_cpu *cpu)
{
	struct jit_state *jit = cpu->jit;
	if (!jit || !jit->enabled || cpu->halted)
		return 0;

	/* Poll for incoming completed JIT blocks */
	if (jit->mqh_res) {
		Size nbytes;
		void *data;
		while (shm_mq_receive(jit->mqh_res, &nbytes, &data, true) == SHM_MQ_SUCCESS) {
			if (nbytes < 20) continue; // Malformed
			uint64_t recv_pc = *(uint64_t *)data;
			uint32_t *p_words = (uint32_t *)data;
			uint32_t insns = p_words[2];
			uint32_t has_st = p_words[3];
			uint32_t sz_words = p_words[4];
			
			if (sz_words == 0) {
				unsigned int r_idx = (recv_pc >> 1) & JIT_CACHE_MASK;
				struct jit_block *recv_b = &jit->cache[r_idx];
				if (recv_b->guest_pc == recv_pc && recv_b->pending)
					recv_b->pending = 0;
				continue;
			}
			
			/* Stitch into main MAP_JIT code_buf */
			if (jit->code_off + sz_words > jit->code_capacity)
				jit_flush(jit);
				
			uint32_t block_start = jit->code_off;
			uint32_t *block_code = &jit->code_buf[block_start];
			
			pthread_jit_write_protect_np(0);
			memcpy(block_code, &p_words[5], sz_words * 4);
			jit->code_off += sz_words;
			
			uint32_t block_size_bytes = sz_words * 4;
			sys_icache_invalidate(block_code, block_size_bytes);
			pthread_jit_write_protect_np(1);

			/* Insert into cache */
			unsigned int r_idx = (recv_pc >> 1) & JIT_CACHE_MASK;
			struct jit_block *recv_b = &jit->cache[r_idx];
			recv_b->guest_pc = recv_pc;
			recv_b->host_code = block_code;
			recv_b->insn_count = insns;
			recv_b->has_store = has_st;
			recv_b->valid = 1;
			recv_b->pending = 0;
		}
	}
	uint64_t pc = cpu->pc;
	unsigned int idx = (pc >> 1) & JIT_CACHE_MASK;
	struct jit_block *b = &jit->cache[idx];

	if (b->valid && b->guest_pc == pc) {
		if (!b->host_code) {
			jit->stat_hits++;
			return 0; /* cached fallback to interpreter */
		}
		jit->stat_hits++;
	} else {
		/* Synchronous compilation fallback */
		b = jit_compile_block(cpu, jit, pc);
		if (!b) {
			/* Mark cache entry as persistently failed so we stop re-compiling */
			b = &jit->cache[idx];
			b->valid = 1;
			b->guest_pc = pc;
			b->host_code = NULL;
			jit->stat_compile_fail++;
			return 0;
		}
		jit->stat_compiles++;
	}

	/* Call the compiled block */
	typedef int (*block_fn)(struct rv64_cpu *);
	block_fn fn = (block_fn)b->host_code;
	int n = fn(cpu);
	jit->stat_jit_insns += n;
	return n;
}
