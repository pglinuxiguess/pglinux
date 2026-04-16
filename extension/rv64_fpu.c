/*
 * rv64_fpu.c — RISC-V F/D floating-point extension
 *
 * Implements RV64F (single-precision) and RV64D (double-precision)
 * instructions: arithmetic, load/store, convert, compare, move.
 *
 * All FP values are stored as raw 64-bit patterns in cpu->f[].
 * Single-precision values are NaN-boxed: upper 32 bits = 0xFFFFFFFF.
 */
#include "rv64_cpu.h"
#include <math.h>
#include <string.h>
#include <fenv.h>
#include <stdio.h>

/* ---- NaN-boxing helpers ---- */

/* Read a double from FP register */
static inline double rd_d(struct rv64_cpu *cpu, int r)
{
	double d;
	memcpy(&d, &cpu->f[r], 8);
	return d;
}

/* Write a double to FP register */
static inline void wr_d(struct rv64_cpu *cpu, int r, double d)
{
	memcpy(&cpu->f[r], &d, 8);
}

/* Read a float from FP register (check NaN-boxing) */
static inline float rd_f(struct rv64_cpu *cpu, int r)
{
	uint64_t v = cpu->f[r];
	/* If upper 32 bits aren't all 1s, return canonical NaN */
	if ((v >> 32) != 0xFFFFFFFF) {
		float nan = NAN;
		return nan;
	}
	float f;
	uint32_t lo = (uint32_t)v;
	memcpy(&f, &lo, 4);
	return f;
}

/* Write a float to FP register (NaN-box it) */
static inline void wr_f(struct rv64_cpu *cpu, int r, float f)
{
	uint32_t bits;
	memcpy(&bits, &f, 4);
	cpu->f[r] = 0xFFFFFFFF00000000ULL | bits;
}

/* Read raw bits */
static inline uint64_t rd_bits(struct rv64_cpu *cpu, int r)
{
	return cpu->f[r];
}

/* Mark FP state as dirty in mstatus (set FS to Dirty = 11) */
static inline void mark_fs_dirty(struct rv64_cpu *cpu)
{
	cpu->mstatus |= (3ULL << 13);
}

/* Forward declaration — trap() lives in rv64_cpu.c but we need it here
 * to properly deliver memory fault exceptions from FP load/store. */
void rv64_trap(struct rv64_cpu *cpu, uint64_t cause, uint64_t tval);

/*
 * Execute a floating-point instruction.
 * Called from rv64_step for opcodes:
 *   0x07 (FLW/FLD), 0x27 (FSW/FSD), 0x43-0x4B (FMADD etc),
 *   0x53 (FP arithmetic)
 *
 * Returns:
 *   0  = success
 *  -1  = unimplemented instruction (caller should trap EXC_ILLEGAL_INSN)
 *   1  = memory fault already trapped (caller must NOT re-trap)
 */
int rv64_fp_exec(struct rv64_cpu *cpu, uint32_t insn)
{
	uint32_t opcode = insn & 0x7F;
	uint32_t rd     = (insn >> 7) & 0x1F;
	uint32_t funct3 = (insn >> 12) & 0x7;
	uint32_t rs1    = (insn >> 15) & 0x1F;
	uint32_t rs2    = (insn >> 20) & 0x1F;
	uint32_t funct7 = insn >> 25;
	int64_t imm_i   = (int64_t)((int32_t)insn >> 20);
	int64_t imm_s   = (int64_t)((int32_t)(((insn >> 25) << 5) |
	                   ((insn >> 7) & 0x1F)) << 20 >> 20);

	mark_fs_dirty(cpu);

	switch (opcode) {

	/* ---- FLW / FLD ---- */
	case 0x07: {
		uint64_t addr = cpu->x[rs1] + imm_i;
		uint64_t val;
		int exc;
		if (funct3 == 2) { /* FLW */
			exc = rv64_jit_load(cpu, addr, 4, &val);
			if (exc) { rv64_trap(cpu, exc, addr); return 1; }
			cpu->f[rd] = 0xFFFFFFFF00000000ULL | (val & 0xFFFFFFFF);
		} else if (funct3 == 3) { /* FLD */
			exc = rv64_jit_load(cpu, addr, 8, &val);
			if (exc) { rv64_trap(cpu, exc, addr); return 1; }
			cpu->f[rd] = val;
		} else {
			return -1;
		}
		return 0;
	}

	/* ---- FSW / FSD ---- */
	case 0x27: {
		uint64_t addr = cpu->x[rs1] + imm_s;
		int exc;
		if (funct3 == 2) { /* FSW */
			exc = rv64_jit_store(cpu, addr, 4, cpu->f[rs2] & 0xFFFFFFFF);
			if (exc) { rv64_trap(cpu, exc, addr); return 1; }
		} else if (funct3 == 3) { /* FSD */
			exc = rv64_jit_store(cpu, addr, 8, cpu->f[rs2]);
			if (exc) { rv64_trap(cpu, exc, addr); return 1; }
		} else {
			return -1;
		}
		return 0;
	}

	/* ---- FMADD.S/D (0x43), FMSUB (0x47), FNMSUB (0x4B), FNMADD (0x4F) ---- */
	case 0x43: case 0x47: case 0x4B: case 0x4F: {
		uint32_t rs3 = (insn >> 27) & 0x1F;
		uint32_t fmt = (insn >> 25) & 0x3;
		if (fmt == 0) { /* single */
			float a = rd_f(cpu, rs1);
			float b = rd_f(cpu, rs2);
			float c = rd_f(cpu, rs3);
			float r;
			switch (opcode) {
			case 0x43: r =  a * b + c; break; /* FMADD */
			case 0x47: r =  a * b - c; break; /* FMSUB */
			case 0x4B: r = -a * b + c; break; /* FNMSUB */
			case 0x4F: r = -a * b - c; break; /* FNMADD */
			default: return -1;
			}
			wr_f(cpu, rd, r);
		} else if (fmt == 1) { /* double */
			double a = rd_d(cpu, rs1);
			double b = rd_d(cpu, rs2);
			double c = rd_d(cpu, rs3);
			double r;
			switch (opcode) {
			case 0x43: r =  a * b + c; break;
			case 0x47: r =  a * b - c; break;
			case 0x4B: r = -a * b + c; break;
			case 0x4F: r = -a * b - c; break;
			default: return -1;
			}
			wr_d(cpu, rd, r);
		} else {
			return -1;
		}
		return 0;
	}

	/* ---- FP arithmetic (opcode 0x53) ---- */
	case 0x53: {
		switch (funct7) {

		/* FADD.S */
		case 0x00: wr_f(cpu, rd, rd_f(cpu, rs1) + rd_f(cpu, rs2)); return 0;
		/* FSUB.S */
		case 0x04: wr_f(cpu, rd, rd_f(cpu, rs1) - rd_f(cpu, rs2)); return 0;
		/* FMUL.S */
		case 0x08: wr_f(cpu, rd, rd_f(cpu, rs1) * rd_f(cpu, rs2)); return 0;
		/* FDIV.S */
		case 0x0C: wr_f(cpu, rd, rd_f(cpu, rs1) / rd_f(cpu, rs2)); return 0;

		/* FSQRT.S */
		case 0x2C: wr_f(cpu, rd, sqrtf(rd_f(cpu, rs1))); return 0;

		/* FSGNJ.S / FSGNJN.S / FSGNJX.S */
		case 0x10: {
			uint32_t a, b;
			float fa = rd_f(cpu, rs1), fb = rd_f(cpu, rs2);
			memcpy(&a, &fa, 4);
			memcpy(&b, &fb, 4);
			uint32_t res;
			switch (funct3) {
			case 0: res = (a & 0x7FFFFFFF) | (b & 0x80000000); break;
			case 1: res = (a & 0x7FFFFFFF) | (~b & 0x80000000); break;
			case 2: res = (a & 0x7FFFFFFF) | ((a ^ b) & 0x80000000); break;
			default: return -1;
			}
			float fr;
			memcpy(&fr, &res, 4);
			wr_f(cpu, rd, fr);
			return 0;
		}

		/* FMIN.S / FMAX.S */
		case 0x14: {
			float a = rd_f(cpu, rs1), b = rd_f(cpu, rs2);
			if (funct3 == 0) /* FMIN */
				wr_f(cpu, rd, (a < b || isnan(b)) ? a : b);
			else /* FMAX */
				wr_f(cpu, rd, (a > b || isnan(b)) ? a : b);
			return 0;
		}

		/* FCVT.W.S / FCVT.WU.S */
		case 0x60: {
			float f = rd_f(cpu, rs1);
			if (rs2 == 0) /* FCVT.W.S */
				cpu->x[rd] = (uint64_t)(int64_t)(int32_t)f;
			else if (rs2 == 1) /* FCVT.WU.S */
				cpu->x[rd] = (uint64_t)(uint32_t)f;
			else if (rs2 == 2) /* FCVT.L.S */
				cpu->x[rd] = (uint64_t)(int64_t)f;
			else /* FCVT.LU.S */
				cpu->x[rd] = (uint64_t)f;
			return 0;
		}

		/* FMV.X.W / FCLASS.S */
		case 0x70: {
			if (funct3 == 0) { /* FMV.X.W */
				uint32_t bits = (uint32_t)cpu->f[rs1];
				cpu->x[rd] = (uint64_t)(int64_t)(int32_t)bits;
			} else { /* FCLASS.S */
				float f = rd_f(cpu, rs1);
				uint32_t cls = 0;
				if (f == -INFINITY) cls = 1 << 0;
				else if (f < 0 && !isinf(f) && !isnan(f)) cls = 1 << 1;
				else if (f == 0.0f && signbit(f)) cls = 1 << 3;
				else if (f == 0.0f && !signbit(f)) cls = 1 << 4;
				else if (f > 0 && !isinf(f) && !isnan(f)) cls = 1 << 6;
				else if (f == INFINITY) cls = 1 << 7;
				else cls = 1 << 9; /* NaN */
				cpu->x[rd] = cls;
			}
			return 0;
		}

		/* FEQ.S / FLT.S / FLE.S */
		case 0x50: {
			float a = rd_f(cpu, rs1), b = rd_f(cpu, rs2);
			switch (funct3) {
			case 2: cpu->x[rd] = (a == b) ? 1 : 0; break; /* FEQ */
			case 1: cpu->x[rd] = (a < b) ? 1 : 0; break;  /* FLT */
			case 0: cpu->x[rd] = (a <= b) ? 1 : 0; break;  /* FLE */
			default: return -1;
			}
			return 0;
		}

		/* FCVT.S.W / FCVT.S.WU / FCVT.S.L / FCVT.S.LU */
		case 0x68: {
			if (rs2 == 0)
				wr_f(cpu, rd, (float)(int32_t)cpu->x[rs1]);
			else if (rs2 == 1)
				wr_f(cpu, rd, (float)(uint32_t)cpu->x[rs1]);
			else if (rs2 == 2)
				wr_f(cpu, rd, (float)(int64_t)cpu->x[rs1]);
			else
				wr_f(cpu, rd, (float)cpu->x[rs1]);
			return 0;
		}

		/* FMV.W.X */
		case 0x78: {
			uint32_t bits = (uint32_t)cpu->x[rs1];
			cpu->f[rd] = 0xFFFFFFFF00000000ULL | bits;
			return 0;
		}

		/* ---- Double-precision ---- */

		/* FADD.D */
		case 0x01: wr_d(cpu, rd, rd_d(cpu, rs1) + rd_d(cpu, rs2)); return 0;
		/* FSUB.D */
		case 0x05: wr_d(cpu, rd, rd_d(cpu, rs1) - rd_d(cpu, rs2)); return 0;
		/* FMUL.D */
		case 0x09: wr_d(cpu, rd, rd_d(cpu, rs1) * rd_d(cpu, rs2)); return 0;
		/* FDIV.D */
		case 0x0D: wr_d(cpu, rd, rd_d(cpu, rs1) / rd_d(cpu, rs2)); return 0;

		/* FSQRT.D */
		case 0x2D: wr_d(cpu, rd, sqrt(rd_d(cpu, rs1))); return 0;

		/* FSGNJ.D / FSGNJN.D / FSGNJX.D */
		case 0x11: {
			uint64_t a = cpu->f[rs1], b = cpu->f[rs2], res;
			switch (funct3) {
			case 0: res = (a & 0x7FFFFFFFFFFFFFFFULL) | (b & 0x8000000000000000ULL); break;
			case 1: res = (a & 0x7FFFFFFFFFFFFFFFULL) | (~b & 0x8000000000000000ULL); break;
			case 2: res = (a & 0x7FFFFFFFFFFFFFFFULL) | ((a ^ b) & 0x8000000000000000ULL); break;
			default: return -1;
			}
			cpu->f[rd] = res;
			return 0;
		}

		/* FMIN.D / FMAX.D */
		case 0x15: {
			double a = rd_d(cpu, rs1), b = rd_d(cpu, rs2);
			if (funct3 == 0)
				wr_d(cpu, rd, (a < b || isnan(b)) ? a : b);
			else
				wr_d(cpu, rd, (a > b || isnan(b)) ? a : b);
			return 0;
		}

		/* FCVT.S.D */
		case 0x20: wr_f(cpu, rd, (float)rd_d(cpu, rs1)); return 0;

		/* FCVT.D.S */
		case 0x21: wr_d(cpu, rd, (double)rd_f(cpu, rs1)); return 0;

		/* FEQ.D / FLT.D / FLE.D */
		case 0x51: {
			double a = rd_d(cpu, rs1), b = rd_d(cpu, rs2);
			switch (funct3) {
			case 2: cpu->x[rd] = (a == b) ? 1 : 0; break;
			case 1: cpu->x[rd] = (a < b) ? 1 : 0; break;
			case 0: cpu->x[rd] = (a <= b) ? 1 : 0; break;
			default: return -1;
			}
			return 0;
		}

		/* FCVT.W.D / FCVT.WU.D / FCVT.L.D / FCVT.LU.D */
		case 0x61: {
			double d = rd_d(cpu, rs1);
			if (rs2 == 0)
				cpu->x[rd] = (uint64_t)(int64_t)(int32_t)d;
			else if (rs2 == 1)
				cpu->x[rd] = (uint64_t)(uint32_t)d;
			else if (rs2 == 2) /* FCVT.L.D */
				cpu->x[rd] = (uint64_t)(int64_t)d;
			else
				cpu->x[rd] = (uint64_t)d;
			return 0;
		}

		/* FCVT.D.W / FCVT.D.WU / FCVT.D.L / FCVT.D.LU */
		case 0x69: {
			if (rs2 == 0)
				wr_d(cpu, rd, (double)(int32_t)cpu->x[rs1]);
			else if (rs2 == 1)
				wr_d(cpu, rd, (double)(uint32_t)cpu->x[rs1]);
			else if (rs2 == 2)
				wr_d(cpu, rd, (double)(int64_t)cpu->x[rs1]);
			else
				wr_d(cpu, rd, (double)cpu->x[rs1]);
			return 0;
		}

		/* FMV.X.D / FCLASS.D */
		case 0x71: {
			if (funct3 == 0) { /* FMV.X.D */
				cpu->x[rd] = cpu->f[rs1];
			} else { /* FCLASS.D */
				double d = rd_d(cpu, rs1);
				uint32_t cls = 0;
				if (d == -INFINITY) cls = 1 << 0;
				else if (d < 0 && !isinf(d) && !isnan(d)) cls = 1 << 1;
				else if (d == 0.0 && signbit(d)) cls = 1 << 3;
				else if (d == 0.0 && !signbit(d)) cls = 1 << 4;
				else if (d > 0 && !isinf(d) && !isnan(d)) cls = 1 << 6;
				else if (d == INFINITY) cls = 1 << 7;
				else cls = 1 << 9;
				cpu->x[rd] = cls;
			}
			return 0;
		}

		/* FMV.D.X */
		case 0x79: {
			cpu->f[rd] = cpu->x[rs1];
			return 0;
		}

		default:
			return -1;
		}
	} /* end opcode 0x53 */

	default:
		return -1;
	}
}
