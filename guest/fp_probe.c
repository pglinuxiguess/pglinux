/*
 * fp_probe.c — Minimal FP instruction probe.
 *
 * Tests a single FLD → FMV.X.D sequence in the tightest possible loop.
 * All output is via integer printf (no FP formatting).
 * Compiled with: riscv64-musl-gcc -static -O0 -lm -o fp_probe fp_probe.c
 *
 * -O0 is critical: it prevents the compiler from optimizing away
 * the FP load/store sequence or reordering them.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint64_t dbl_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

int main(void) {
    printf("=== FP Probe ===\n");

    /* Test 1: fmv.d.x → fmv.x.d (GP→FP→GP, no memory) */
    {
        uint64_t in_bits = 0x4059000000000000ULL; /* 100.0 */
        double d;
        uint64_t out_bits;
        __asm__ volatile ("fmv.d.x %0, %1" : "=f"(d) : "r"(in_bits));
        __asm__ volatile ("fmv.x.d %0, %1" : "=r"(out_bits) : "f"(d));
        printf("T1 fmv round-trip: in=0x%016llx out=0x%016llx %s\n",
               (unsigned long long)in_bits, (unsigned long long)out_bits,
               in_bits == out_bits ? "PASS" : "FAIL");
    }

    /* Test 2: sd → fld → fmv.x.d (integer store, FP load, FP→GP) */
    {
        uint64_t bits = 0x4045000000000000ULL; /* 42.0 */
        volatile uint64_t mem;
        double d;
        uint64_t out;
        mem = bits; /* integer store (sd) */
        __asm__ volatile ("fld %0, %1" : "=f"(d) : "m"(mem));
        __asm__ volatile ("fmv.x.d %0, %1" : "=r"(out) : "f"(d));
        printf("T2 sd→fld→fmv.x.d: stored=0x%016llx loaded=0x%016llx %s\n",
               (unsigned long long)bits, (unsigned long long)out,
               bits == out ? "PASS" : "FAIL");
    }

    /* Test 3: fmv.d.x → fsd → ld (GP→FP, FP store, integer load) */
    {
        uint64_t in_bits = 0x4059000000000000ULL; /* 100.0 */
        double d;
        volatile uint64_t mem = 0xDEADBEEFDEADBEEFULL; /* sentinel */
        __asm__ volatile ("fmv.d.x %0, %1" : "=f"(d) : "r"(in_bits));
        __asm__ volatile ("fsd %1, %0" : "=m"(mem) : "f"(d));
        uint64_t out = mem;
        printf("T3 fmv.d.x→fsd→ld: in=0x%016llx out=0x%016llx %s\n",
               (unsigned long long)in_bits, (unsigned long long)out,
               in_bits == out ? "PASS" : "FAIL");
    }

    /* Test 4: fmv.d.x → fcvt.l.d (GP→FP, then FP→int conversion) */
    {
        uint64_t bits = 0x4059000000000000ULL; /* 100.0 */
        double d;
        long result;
        __asm__ volatile ("fmv.d.x %0, %1" : "=f"(d) : "r"(bits));
        __asm__ volatile ("fcvt.l.d %0, %1, rtz" : "=r"(result) : "f"(d));
        printf("T4 fmv.d.x→fcvt.l.d: bits=0x%016llx result=%ld %s\n",
               (unsigned long long)bits, result,
               result == 100 ? "PASS" : "FAIL");
    }

    /* Test 5: sd → fld → fcvt.l.d (pure memory path) */
    {
        uint64_t bits = 0x4059000000000000ULL; /* 100.0 */
        volatile uint64_t mem;
        double d;
        long result;
        mem = bits;
        __asm__ volatile ("fld %0, %1" : "=f"(d) : "m"(mem));
        __asm__ volatile ("fcvt.l.d %0, %1, rtz" : "=r"(result) : "f"(d));
        printf("T5 sd→fld→fcvt.l.d: result=%ld %s\n",
               result, result == 100 ? "PASS" : "FAIL");
    }

    /* Test 6: Two separate asm blocks using SAME fp register constraint */
    {
        uint64_t bits = 0x4059000000000000ULL;
        double d;
        __asm__ volatile ("fmv.d.x fa5, %0" : : "r"(bits) : "fa5");
        /* Now read fa5 back */
        uint64_t out;
        __asm__ volatile ("fmv.x.d %0, fa5" : "=r"(out) : : "fa5");
        printf("T6 explicit fa5: in=0x%016llx out=0x%016llx %s\n",
               (unsigned long long)bits, (unsigned long long)out,
               bits == out ? "PASS" : "FAIL");
    }

    /* Test 7: Does an integer instruction between corrupt the FP reg? */
    {
        uint64_t bits = 0x4059000000000000ULL;
        __asm__ volatile ("fmv.d.x fa5, %0" : : "r"(bits) : "fa5");
        /* Execute some integer instructions (these may trigger JIT) */
        volatile int x = 1;
        x = x + 1;
        x = x + 1;
        x = x + 1;
        x = x + 1;
        /* Now read fa5 — has it been clobbered? */
        uint64_t out;
        __asm__ volatile ("fmv.x.d %0, fa5" : "=r"(out) : : "fa5");
        printf("T7 int between fmv: in=0x%016llx out=0x%016llx %s\n",
               (unsigned long long)bits, (unsigned long long)out,
               bits == out ? "PASS" : "FAIL");
    }

    /* Test 8: Same as T7 but with a function call in between */
    {
        uint64_t bits = 0x4059000000000000ULL;
        __asm__ volatile ("fmv.d.x fa5, %0" : : "r"(bits) : "fa5");
        /* printf is a function call — will compiler save/restore fa5? */
        printf(""); /* force a call */
        uint64_t out;
        __asm__ volatile ("fmv.x.d %0, fa5" : "=r"(out) : : "fa5");
        printf("T8 call between fmv: in=0x%016llx out=0x%016llx %s\n",
               (unsigned long long)bits, (unsigned long long)out,
               bits == out ? "PASS" : "FAIL");
    }

    printf("=== FP Probe Done ===\n");
    return 0;
}
