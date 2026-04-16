
/* rv64_sljit.c - RV64I Decoder mapped to SLJIT (with Memory Loads/Stores & Branches) */
#include "rv64_cpu.h"
#include "rv64_jit.h"
#include "sljit_src/sljitLir.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define CPU_REG SLJIT_S0
#define COUNT_REG SLJIT_S1  /* runtime instruction counter for accurate early-exit returns */

extern int rv64_fp_exec(struct rv64_cpu *cpu, uint32_t insn);

typedef int (*block_fn_t)(struct rv64_cpu *, uint64_t);

static sljit_sw helper_sljit_load(struct rv64_cpu *cpu, sljit_sw vaddr, sljit_sw size, sljit_sw *val) {
    return (sljit_sw)rv64_jit_load(cpu, (uint64_t)vaddr, (int)size, (uint64_t *)val);
}

static sljit_sw helper_sljit_store(struct rv64_cpu *cpu, sljit_sw vaddr, sljit_sw size, sljit_sw val) {
    return (sljit_sw)rv64_jit_store(cpu, (uint64_t)vaddr, (int)size, (uint64_t)val);
}


static uint32_t sljit_uncompress(uint16_t ci) {
    uint32_t exp = 0;
 /* expanded instruction */
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
			default: return 0;
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
						default: return 0;
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
				uint32_t jimm; jimm = ((imm >> 20) & 1) << 31 |
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
				if (off & 0x100) off -= 0x200;
				uint32_t bimm; bimm = (((off >> 12) & 1) << 31) |
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
				if (off & 0x100) off -= 0x200;
				uint32_t bimm; bimm = (((off >> 12) & 1) << 31) |
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
			default: return 0;
			}
			break;

		default: return 0;
		}
		
    return exp;
}

static inline void emit_load_guest(struct sljit_compiler *c, int sljit_dst, int grs) {
    if (grs == 0) {
        sljit_emit_op1(c, SLJIT_MOV, sljit_dst, 0, SLJIT_IMM, 0);
    } else {
        sljit_emit_op1(c, SLJIT_MOV, sljit_dst, 0, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, x) + grs * 8);
    }
}

static inline void emit_store_guest(struct sljit_compiler *c, int grs, int sljit_src) {
    if (grs != 0) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, x) + grs * 8, sljit_src, 0);
    }
}

static struct sljit_jump *emit_memory_icall(struct sljit_compiler *c, int is_store, int size, int rd, int rs1, int32_t imm, int rs2) {
    struct sljit_jump *exc_jmp;
    if (rs1 == 0) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(int64_t)imm);
    } else {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, x) + rs1 * 8);
        sljit_emit_op2(c, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(int64_t)imm);
    }
    
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, CPU_REG, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, size);
    
    if (is_store) {
        if (rs2 == 0) sljit_emit_op1(c, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, 0);
        else sljit_emit_op1(c, SLJIT_MOV, SLJIT_R3, 0, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, x) + rs2 * 8);
        sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS4(W, W, W, W, W), SLJIT_IMM, (sljit_sw)helper_sljit_store);
    } else {
        sljit_get_local_base(c, SLJIT_R3, 0, 0);
        sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS4(W, W, W, W, W), SLJIT_IMM, (sljit_sw)helper_sljit_load);
    }
    
    exc_jmp = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_RETURN_REG, 0, SLJIT_IMM, 0);
    
    if (!is_store) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), 0);
    }
    return exc_jmp;
}

int jit_init(struct jit_state *jit) { jit->enabled = 1; return 0; }
void jit_destroy(struct jit_state *jit) {
    if (!jit) return;
    for (int i = 0; i < JIT_CACHE_SIZE; i++) {
        if (jit->cache[i].host_code) {
            sljit_free_code(jit->cache[i].host_code, NULL);
            jit->cache[i].host_code = NULL;
        }
    }
}
void jit_flush(struct jit_state *jit) { jit_destroy(jit); memset(jit->cache, 0, sizeof(jit->cache)); }
void jit_invalidate(struct jit_state *jit) { jit_flush(jit); }

static struct jit_block *jit_compile_block(struct rv64_cpu *cpu, struct jit_state *jit, uint64_t guest_pc) {
    uint64_t pc = guest_pc;
    int insn_count = 0;
    int block_ended = 0;
    struct sljit_jump *epilogue_jumps[64];
    int num_epilogue_jumps = 0;
    struct sljit_label *epilogue;
    void *code;
    uint32_t idx;
    struct jit_block *b;

    struct sljit_compiler *c = sljit_create_compiler(NULL);
    if (!c) return NULL;

    sljit_emit_enter(c, 0, SLJIT_ARGS2(W, W, W), 4, 4, 8);

    /* S1 = runtime instruction counter, starts at 0 */
    sljit_emit_op1(c, SLJIT_MOV, COUNT_REG, 0, SLJIT_IMM, 0);

    int max_insns = (jit->rt_max_block_insns > 0) ? jit->rt_max_block_insns
                                                   : JIT_MAX_BLOCK_INSNS;

    while (insn_count < max_insns && !block_ended) {
        uint64_t val;
        uint32_t insn;
        int is_compressed;
        uint32_t op, rd, rs1, rs2, funct3, funct7;
        int supported = 0;

        if (rv64_cpu_load(cpu, pc, 4, &val, ACCESS_EXEC) != 0) break;
        insn = (uint32_t)val;
        is_compressed = ((insn & 3) != 3);
        if (is_compressed) {
            insn = sljit_uncompress((uint16_t)(insn & 0xFFFF));
            if (insn == 0) {
                supported = 0;
                goto check_support;
            }
        }
        
        op = insn & 0x7F;

        /* Tool 2: check per-opcode disable bitmask */
        if (jit->disabled_ops[op / 64] & (1ULL << (op % 64))) {
            supported = 0;
            goto check_support;
        }
        rd = (insn >> 7) & 0x1F;
        rs1 = (insn >> 15) & 0x1F;
        rs2 = (insn >> 20) & 0x1F;
        funct3 = (insn >> 12) & 7;
        funct7 = insn >> 25;

        {
            if (op == 0x33) { /* OP-64 */
                emit_load_guest(c, SLJIT_R0, rs1);
                emit_load_guest(c, SLJIT_R1, rs2);
                supported = 1;
                
                if (funct3 == 0) sljit_emit_op2(c, funct7 == 0x20 ? SLJIT_SUB : SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 1) sljit_emit_op2(c, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 2) {
                    sljit_emit_op2(c, SLJIT_SUB | SLJIT_SET_SIG_LESS, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                    sljit_emit_op_flags(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_LESS);
                }
                else if (funct3 == 3) {
                    sljit_emit_op2(c, SLJIT_SUB | SLJIT_SET_LESS, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                    sljit_emit_op_flags(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_LESS);
                }
                else if (funct3 == 4) sljit_emit_op2(c, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 5) sljit_emit_op2(c, funct7 == 0x20 ? SLJIT_ASHR : SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 6) sljit_emit_op2(c, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 7) sljit_emit_op2(c, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else supported = 0;
                
                if (supported) emit_store_guest(c, rd, SLJIT_R0);
            } 
            else if (op == 0x13) { /* OP-IMM-64 */
                int32_t imm = (int32_t)insn >> 20;
                emit_load_guest(c, SLJIT_R0, rs1);
                supported = 1;
                
                if (funct3 == 0) sljit_emit_op2(c, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                else if (funct3 == 2) {
                    sljit_emit_op2(c, SLJIT_SUB | SLJIT_SET_SIG_LESS, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                    sljit_emit_op_flags(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_LESS);
                }
                else if (funct3 == 3) {
                    sljit_emit_op2(c, SLJIT_SUB | SLJIT_SET_LESS, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                    sljit_emit_op_flags(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_LESS);
                }
                else if (funct3 == 4) sljit_emit_op2(c, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                else if (funct3 == 6) sljit_emit_op2(c, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                else if (funct3 == 7) sljit_emit_op2(c, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                else if (funct3 == 1) sljit_emit_op2(c, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm & 0x3F);
                else if (funct3 == 5) sljit_emit_op2(c, ((insn >> 26) == 0x10) ? SLJIT_ASHR : SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm & 0x3F);
                else supported = 0;
                
                if (supported) emit_store_guest(c, rd, SLJIT_R0);
            }
            else if (op == 0x3B) { /* OP-32 */
                emit_load_guest(c, SLJIT_R0, rs1);
                emit_load_guest(c, SLJIT_R1, rs2);
                supported = 1;
                if (funct3 == 0) sljit_emit_op2(c, funct7 == 0x20 ? SLJIT_SUB32 : SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 1) sljit_emit_op2(c, SLJIT_SHL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else if (funct3 == 5) sljit_emit_op2(c, funct7 == 0x20 ? SLJIT_ASHR32 : SLJIT_LSHR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                else supported = 0;
                if (supported) {
                    sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
                    emit_store_guest(c, rd, SLJIT_R0);
                }
            }
            else if (op == 0x1B) { /* OP-IMM-32 */
                int32_t imm = (int32_t)insn >> 20;
                emit_load_guest(c, SLJIT_R0, rs1);
                supported = 1;
                if (funct3 == 0) sljit_emit_op2(c, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm);
                else if (funct3 == 1) sljit_emit_op2(c, SLJIT_SHL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm & 0x1F);
                else if (funct3 == 5) sljit_emit_op2(c, funct7 == 0x20 ? SLJIT_ASHR32 : SLJIT_LSHR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, imm & 0x1F);
                else supported = 0;
                if (supported) {
                    sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
                    emit_store_guest(c, rd, SLJIT_R0);
                }
            }
            else if (op == 0x37) { /* LUI */
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)(int64_t)(int32_t)(insn & 0xFFFFF000));
                emit_store_guest(c, rd, SLJIT_R0);
                supported = 1;
            }
            else if (op == 0x17) { /* AUIPC */
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, pc + (sljit_sw)(int64_t)(int32_t)(insn & 0xFFFFF000));
                emit_store_guest(c, rd, SLJIT_R0);
                supported = 1;
            }
            else if (op == 0x6F) { /* JAL */
                int32_t off = (int32_t)((((insn >> 31) & 1) << 20) | (((insn >> 21) & 0x3FF) << 1) | (((insn >> 20) & 1) << 11) | (((insn >> 12) & 0xFF) << 12));
                if (off & 0x100000) off -= 0x200000;
                uint64_t target = pc + (uint64_t)(int64_t)off;
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, pc + (is_compressed ? 2 : 4));
                emit_store_guest(c, rd, SLJIT_R0);
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, target);
                supported = 1;
                block_ended = 1;
            }
            else if (op == 0x67) { /* JALR */
                int64_t imm = ((int32_t)insn >> 20);
                emit_load_guest(c, SLJIT_R1, rs1);
                sljit_emit_op2(c, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, imm);
                sljit_emit_op2(c, SLJIT_AND, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, ~1ULL);
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, pc + (is_compressed ? 2 : 4));
                emit_store_guest(c, rd, SLJIT_R0);
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_R1, 0);
                supported = 1;
                block_ended = 1;
            }
            else if (op == 0x63) { /* Branch */
                int32_t off = (int32_t)((((insn >> 31) & 1) << 12) | (((insn >> 7) & 1) << 11) | (((insn >> 25) & 0x3F) << 5) | (((insn >> 8) & 0xF) << 1));
                if (off & 0x1000) off -= 0x2000;
                uint64_t target_taken = pc + (uint64_t)(int64_t)off;
                uint64_t target_fall = pc + (is_compressed ? 2 : 4);
                int type = -1;
                struct sljit_jump *cmp, *jmp_end;
                
                emit_load_guest(c, SLJIT_R0, rs1);
                emit_load_guest(c, SLJIT_R1, rs2);
                
                if (funct3 == 0) type = SLJIT_EQUAL;
                else if (funct3 == 1) type = SLJIT_NOT_EQUAL;
                else if (funct3 == 4) type = SLJIT_SIG_LESS;
                else if (funct3 == 5) type = SLJIT_SIG_GREATER_EQUAL;
                else if (funct3 == 6) type = SLJIT_LESS;
                else if (funct3 == 7) type = SLJIT_GREATER_EQUAL;
                
                if (type != -1) {
                    cmp = sljit_emit_cmp(c, type, SLJIT_R0, 0, SLJIT_R1, 0);
                    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, target_fall);
                    jmp_end = sljit_emit_jump(c, SLJIT_JUMP);
                    
                    sljit_set_label(cmp, sljit_emit_label(c));
                    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, target_taken);
                    sljit_set_label(jmp_end, sljit_emit_label(c));
                    supported = 1;
                    block_ended = 1;
                }
            }
            else if (op == 0x03) { /* LOAD */
                int32_t imm = (int32_t)insn >> 20;
                int size = 1 << (funct3 & 3);
                /* Write PC of THIS instruction before the potentially-faulting call
                 * so an early exit leaves cpu->pc pointing at the faulting insn */
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, pc);
                struct sljit_jump *ext_jmp = emit_memory_icall(c, 0, size, rd, rs1, imm, 0);
                epilogue_jumps[num_epilogue_jumps++] = ext_jmp;
                
                if (funct3 == 0) sljit_emit_op1(c, SLJIT_MOV_S8, SLJIT_R0, 0, SLJIT_R0, 0);
                else if (funct3 == 1) sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R0, 0, SLJIT_R0, 0);
                else if (funct3 == 2) sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
                else if (funct3 == 4) sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_R0, 0);
                else if (funct3 == 5) sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_R0, 0, SLJIT_R0, 0);
                else if (funct3 == 6) sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_R0, 0);
                
                emit_store_guest(c, rd, SLJIT_R0);
                supported = 1;
                if (num_epilogue_jumps > 60) block_ended = 1;
            }
            else if (op == 0x23) { /* STORE */
                int32_t imm = ((insn >> 25) << 5) | ((insn >> 7) & 0x1F);
                int size;
                struct sljit_jump *ext_jmp;
                if (imm & 0x800) imm -= 0x1000;
                size = 1 << (funct3 & 3);
                
                /* Write PC of THIS instruction before the potentially-faulting call */
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, pc);
                ext_jmp = emit_memory_icall(c, 1, size, 0, rs1, imm, rs2);
                epilogue_jumps[num_epilogue_jumps++] = ext_jmp;
                supported = 1;
                if (num_epilogue_jumps > 60) block_ended = 1;
            }
            else if (op == 0x07 || op == 0x27 || op == 0x43 || op == 0x47 || op == 0x4B || op == 0x4F || op == 0x53) {
                struct sljit_jump *exc_jmp;
                /* Sync PC cleanly natively before FPU execution to preserve exception paths */
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, pc);
                
                /* Call C FPU fallback asynchronously */
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, CPU_REG, 0);
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, insn);
                sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2(W, W, W), SLJIT_IMM, (sljit_sw)rv64_fp_exec);
                
                /* Terminate execution bounds seamlessly if the FPU generated a Memory / Illegal Insn fault */
                exc_jmp = sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_RETURN_REG, 0, SLJIT_IMM, 0);
                epilogue_jumps[num_epilogue_jumps++] = exc_jmp;
                
                supported = 1;
                if (num_epilogue_jumps > 60) block_ended = 1;
            }
        }

        check_support:
        if (!supported) {
            if (insn_count == 0) {
                struct jit_block *bc;
                uint32_t idxc;
                sljit_free_compiler(c);
                idxc = (guest_pc / 2) & JIT_CACHE_MASK;
                bc = &jit->cache[idxc];
                if (bc->host_code) sljit_free_code(bc->host_code, NULL);
                bc->guest_pc = guest_pc;
                bc->host_code = NULL;
                bc->insn_count = 0;
                bc->valid = 1;
                return bc;
            }
            break;
        }

        insn_count++;
        /* Increment runtime counter — this only executes if the instruction completed */
        sljit_emit_op2(c, SLJIT_ADD, COUNT_REG, 0, COUNT_REG, 0, SLJIT_IMM, 1);
        if (!block_ended) {
            pc += is_compressed ? 2 : 4;
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(CPU_REG), offsetof(struct rv64_cpu, pc), SLJIT_IMM, pc);
        } else {
            break;
        }
    }

    epilogue = sljit_emit_label(c);
    for (int i = 0; i < num_epilogue_jumps; i++) {
        sljit_set_label(epilogue_jumps[i], epilogue);
    }

    /* Return the runtime counter — accurate even on early exit via epilogue jumps */
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_RETURN_REG, 0, COUNT_REG, 0);
    sljit_emit_return(c, SLJIT_MOV, SLJIT_RETURN_REG, 0);

    code = sljit_generate_code(c, 0, NULL);
    sljit_free_compiler(c);
    if (!code) return NULL;

    idx = (guest_pc / 2) & JIT_CACHE_MASK;
    b = &jit->cache[idx];
    if (b->host_code) sljit_free_code(b->host_code, NULL);

    b->guest_pc = guest_pc;
    b->host_code = code;
    b->insn_count = insn_count;
    b->valid = 1;
    b->has_store = 0;



    return b;
}

int jit_exec(struct rv64_cpu *cpu) {
    uint32_t idx;
    struct jit_block *b;
    int count;

    if (!cpu->jit || !cpu->jit->enabled) return 0;
    idx = (cpu->pc / 2) & JIT_CACHE_MASK;
    b = &cpu->jit->cache[idx];

    if (!b->valid || b->guest_pc != cpu->pc) {
        b = jit_compile_block(cpu, cpu->jit, cpu->pc);
        if (!b) return 0;
    }
    
    if (!b->host_code) return 0;

    count = ((block_fn_t)b->host_code)(cpu, cpu->pc);
    cpu->instret += count;
    cpu->jit->stat_jit_insns += count;
    return count;
}

uint32_t jit_compile_offline(struct rv64_cpu *cpu, struct jit_state *jit, uint64_t tgt, struct jit_block *out) {
    return 0;
}
