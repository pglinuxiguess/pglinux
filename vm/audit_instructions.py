#!/usr/bin/env python3
"""Cross-reference instructions used by postgres vs what our emulator handles."""

# All unique instructions from postgres binary (objdump output)
postgres_instrs = """
add addw amoadd.d.aq amoadd.w.aq amoand.w.aq amoor.w.aq
amoswap.d.aq amoswap.w amoswap.w.aq and auipc
beq beqz bge bgeu bgez bgtz blez blt bltu bltz bne bnez
csrs div divu divuw divw
fabs.d fabs.s fadd.d fadd.s fcvt.d.l fcvt.d.lu fcvt.d.s fcvt.d.w fcvt.d.wu
fcvt.l.d fcvt.l.s fcvt.lu.d fcvt.s.d fcvt.s.l fcvt.s.w fcvt.s.wu
fcvt.w.d fcvt.w.s fcvt.wu.d fcvt.wu.s
fdiv.d fdiv.s fence feq.d feq.s fld fle.d fle.s flt.d flt.s flw
fmadd.d fmadd.s fmsub.d fmul.d fmul.s fmv.d fmv.d.x fmv.s fmv.w.x fmv.x.d fmv.x.w
fneg.d fneg.s fnmsub.d fnmsub.s frrm fsd fsqrt.d fsqrt.s fsub.d fsub.s fsw
jal jalr jr lb lbu ld lh lhu li
lr.d.aq lr.d.aqrl lr.w.aq lr.w.aqrl
lui lw lwu mul mulh mulhu mulw mv neg negw not or
rem remu remuw remw sb
sc.d.aq sc.d.aqrl sc.w.aq sc.w.aqrl
sd seqz sext.w sgtz sh sll sllw slt slti sltiu sltu snez
sra sraw srl srlw sub subw sw xor zext.b
""".split()

# Pseudo-instructions that map to real instructions the emulator handles
pseudos = {
    'mv': 'add (addi rd, rs, 0)',
    'li': 'addi/lui',
    'neg': 'sub (sub rd, x0, rs)',
    'negw': 'subw (subw rd, x0, rs)',
    'not': 'xori rd, rs, -1',
    'seqz': 'sltiu rd, rs, 1',
    'snez': 'sltu rd, x0, rs',
    'sgtz': 'slt rd, x0, rs',
    'beqz': 'beq rs, x0, off',
    'bnez': 'bne rs, x0, off',
    'blez': 'bge x0, rs, off',
    'bgez': 'bge rs, x0, off',
    'bgtz': 'blt x0, rs, off',
    'bltz': 'blt rs, x0, off',
    'jr': 'jalr x0, 0(rs)',
    'sext.w': 'addiw rd, rs, 0',
    'zext.b': 'andi rd, rs, 0xff',
    'fabs.d': 'fsgnjx.d rd, rs, rs',
    'fabs.s': 'fsgnjx.s rd, rs, rs',
    'fmv.d': 'fsgnj.d rd, rs, rs',
    'fmv.s': 'fsgnj.s rd, rs, rs',
    'fneg.d': 'fsgnjn.d rd, rs, rs',
    'fneg.s': 'fsgnjn.s rd, rs, rs',
    'frrm': 'csrr rd, frm (csrrs rd, frm, x0)',
}

# What our emulator implements (from rv64_cpu.c switch + rv64_fpu.c)
# Organized by opcode
implemented = {
    # Integer ALU
    'lui', 'auipc',
    'add', 'sub', 'sll', 'slt', 'sltu', 'xor', 'srl', 'sra', 'or', 'and',  # R-type 0x33
    'addw', 'subw', 'sllw', 'srlw', 'sraw',  # R-type 0x3B (RV64)
    'addi', 'slti', 'sltiu', 'xori', 'ori', 'andi', 'slli', 'srli', 'srai',  # I-type 0x13
    'addiw', 'slliw', 'srliw', 'sraiw',  # I-type 0x1B (RV64)

    # M extension (multiply/divide)
    'mul', 'mulh', 'mulhsu', 'mulhu', 'div', 'divu', 'rem', 'remu',  # 0x33 funct7=1
    'mulw', 'divw', 'divuw', 'remw', 'remuw',  # 0x3B funct7=1

    # Load/Store
    'lb', 'lh', 'lw', 'ld', 'lbu', 'lhu', 'lwu',  # 0x03
    'sb', 'sh', 'sw', 'sd',  # 0x23

    # Branches
    'beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu',  # 0x63

    # Jumps
    'jal', 'jalr',  # 0x6F, 0x67

    # System
    'fence', 'ecall', 'ebreak',  # 0x0F, 0x73
    'csrrw', 'csrrs', 'csrrc', 'csrrwi', 'csrrsi', 'csrrci',  # 0x73

    # Atomics (0x2F)
    'lr.w', 'sc.w', 'lr.d', 'sc.d',
    'amoswap.w', 'amoadd.w', 'amoand.w', 'amoor.w', 'amoxor.w',
    'amomin.w', 'amomax.w', 'amominu.w', 'amomaxu.w',
    'amoswap.d', 'amoadd.d', 'amoand.d', 'amoor.d', 'amoxor.d',
    'amomin.d', 'amomax.d', 'amominu.d', 'amomaxu.d',

    # FP Load/Store
    'flw', 'fld', 'fsw', 'fsd',  # 0x07, 0x27

    # FP Arithmetic (single)
    'fadd.s', 'fsub.s', 'fmul.s', 'fdiv.s', 'fsqrt.s',
    'fmin.s', 'fmax.s',
    'fmadd.s', 'fmsub.s', 'fnmsub.s', 'fnmadd.s',

    # FP Arithmetic (double)
    'fadd.d', 'fsub.d', 'fmul.d', 'fdiv.d', 'fsqrt.d',
    'fmin.d', 'fmax.d',
    'fmadd.d', 'fmsub.d', 'fnmsub.d', 'fnmadd.d',

    # FP comparison
    'feq.s', 'flt.s', 'fle.s',
    'feq.d', 'flt.d', 'fle.d',

    # FP sign injection (covers fmv.d, fabs.d, fneg.d pseudo-instructions)
    'fsgnj.s', 'fsgnjn.s', 'fsgnjx.s',
    'fsgnj.d', 'fsgnjn.d', 'fsgnjx.d',

    # FP conversion
    'fcvt.w.s', 'fcvt.wu.s', 'fcvt.s.w', 'fcvt.s.wu',
    'fcvt.w.d', 'fcvt.wu.d', 'fcvt.d.w', 'fcvt.d.wu',
    'fcvt.l.s', 'fcvt.lu.s', 'fcvt.s.l', 'fcvt.s.lu',
    'fcvt.l.d', 'fcvt.lu.d', 'fcvt.d.l', 'fcvt.d.lu',
    'fcvt.s.d', 'fcvt.d.s',

    # FP move
    'fmv.x.w', 'fmv.w.x',
    'fmv.x.d', 'fmv.d.x',

    # FP classify
    'fclass.s', 'fclass.d',
}

print("=" * 70)
print("RISC-V Instruction Audit: postgres binary vs emulator")
print("=" * 70)

missing = []
pseudo_ok = []
direct_ok = []

for instr in sorted(set(postgres_instrs)):
    # Strip .aq/.aqrl suffixes for matching
    base = instr.replace('.aqrl', '').replace('.aq', '')

    if base in implemented:
        direct_ok.append(instr)
    elif instr in pseudos:
        pseudo_ok.append(instr)
    else:
        missing.append(instr)

print(f"\n✅ Directly implemented: {len(direct_ok)}")
print(f"✅ Pseudo-instructions (handled via base): {len(pseudo_ok)}")
print(f"❌ MISSING: {len(missing)}")

if missing:
    print(f"\n{'='*70}")
    print("MISSING INSTRUCTIONS:")
    print(f"{'='*70}")
    for m in sorted(missing):
        if m in pseudos:
            print(f"  {m:20s}  (pseudo for {pseudos[m]})")
        else:
            print(f"  {m:20s}  *** NEEDS IMPLEMENTATION ***")

print(f"\n{'='*70}")
print("PSEUDO-INSTRUCTIONS (handled by existing base instructions):")
print(f"{'='*70}")
for p in sorted(pseudo_ok):
    print(f"  {p:20s} → {pseudos[p]}")
