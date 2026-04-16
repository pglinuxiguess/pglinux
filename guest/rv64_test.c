/*
 * rv64_test.c — RISC-V environment validation suite
 *
 * Compiled with the same cross-compiler + musl that builds PostgreSQL.
 * Runs inside the guest VM to verify that fundamental C library and
 * CPU operations work correctly before we attempt anything complex
 * like initdb.
 *
 * Exit code: 0 = all pass, 1 = at least one failure.
 * Each test prints PASS/FAIL to stdout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <getopt.h>
#include <fcntl.h>

static int failures = 0;
static int tests = 0;

#define CHECK(name, cond) do { \
    tests++; \
    if (cond) { \
        printf("  PASS: %s\n", name); \
    } else { \
        printf("  FAIL: %s\n", name); \
        failures++; \
    } \
} while (0)

#define CHECK_INT(name, got, expected) do { \
    tests++; \
    long _g = (long)(got), _e = (long)(expected); \
    if (_g == _e) { \
        printf("  PASS: %s = %ld\n", name, _g); \
    } else { \
        printf("  FAIL: %s = %ld (expected %ld)\n", name, _g, _e); \
        failures++; \
    } \
} while (0)

/* ================================================================
 * 1. Integer Parsing (strtol, strtoll, atoi, atol)
 * PG uses strtol extensively for GUC integer parsing.
 * ================================================================ */
static void test_integer_parsing(void) {
    printf("[Integer Parsing]\n");
    char *endptr;

    /* strtol basics */
    CHECK_INT("strtol(\"100\", 10)", strtol("100", NULL, 10), 100);
    CHECK_INT("strtol(\"0\", 10)", strtol("0", NULL, 10), 0);
    CHECK_INT("strtol(\"-1\", 10)", strtol("-1", NULL, 10), -1);
    CHECK_INT("strtol(\"2048\", 10)", strtol("2048", NULL, 10), 2048);
    CHECK_INT("strtol(\"16777216\", 10)", strtol("16777216", NULL, 10), 16777216);
    CHECK_INT("strtol(\"1000000\", 10)", strtol("1000000", NULL, 10), 1000000);

    /* base 0 (auto-detect) — this is what PG uses */
    CHECK_INT("strtol(\"100\", 0)", strtol("100", NULL, 0), 100);
    CHECK_INT("strtol(\"0x1F\", 0)", strtol("0x1F", NULL, 0), 31);
    CHECK_INT("strtol(\"077\", 0)", strtol("077", NULL, 0), 63);

    /* endptr behavior */
    long v = strtol("100", &endptr, 0);
    CHECK_INT("strtol(\"100\", &end, 0) val", v, 100);
    CHECK("strtol(\"100\") endptr at NUL", *endptr == '\0');

    v = strtol("100kB", &endptr, 0);
    CHECK_INT("strtol(\"100kB\") val", v, 100);
    CHECK("strtol(\"100kB\") endptr at 'k'", *endptr == 'k');

    /* atoi */
    CHECK_INT("atoi(\"42\")", atoi("42"), 42);
    CHECK_INT("atoi(\"0\")", atoi("0"), 0);
    CHECK_INT("atoi(\"-7\")", atoi("-7"), -7);
    CHECK_INT("atoi(\"262143\")", atoi("262143"), 262143);

    /* atol */
    CHECK_INT("atol(\"8388608\")", atol("8388608"), 8388608);

    /* strtoll */
    CHECK_INT("strtoll(\"100\", 10)", (long)strtoll("100", NULL, 10), 100);

    /* large values */
    CHECK_INT("strtol(\"2147483647\", 10)", strtol("2147483647", NULL, 10), 2147483647L);

    printf("\n");
}

/* ================================================================
 * Helper: extract raw IEEE 754 bits from a double without using FP.
 * This is our ground truth — memcpy cannot lie.
 * ================================================================ */
static uint64_t dbl_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

/* Helper: construct a double from raw bits */
static double dbl_from_bits(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* Helper macro: check a double's bits directly */
#define CHECK_DBL_BITS(name, val, expected_hex) do { \
    uint64_t _b = dbl_bits(val); \
    tests++; \
    if (_b == (expected_hex)) { \
        printf("  PASS: %s bits=0x%016lx\n", name, _b); \
    } else { \
        printf("  FAIL: %s bits=0x%016lx (expected 0x%016lx)\n", \
               name, _b, (uint64_t)(expected_hex)); \
        failures++; \
    } \
} while (0)

/* ================================================================
 * 2. FP Instruction Tests
 *
 * Strategy: NEVER use printf("%f") to verify results.
 * Instead, use memcpy to extract raw bits, or cast to int
 * (which uses fcvt.w.d — a different instruction we can
 * cross-check independently).
 *
 * Tests are ordered by dependency:
 *   A. Literal loading (compiler-generated constants)
 *   B. Integer↔FP conversions (each direction, each width)
 *   C. FP memory round-trips (fsd/fld through guest RAM)
 *   D. FP arithmetic (add, sub, mul, div, sqrt)
 *   E. FP comparisons (eq, lt, le)
 *   F. FP register persistence across function calls
 *   G. strtod verification (is it broken or just printf?)
 *   H. PG parse_int end-to-end simulation
 * ================================================================ */

/* ---- 2A: Literal Loading ---- */
static void test_fp_literals(void) {
    printf("[FP 2A: Literal Loading]\n");

    /* Compiler should emit these as constants in .rodata, loaded via fld */
    double d1 = 0.0;
    CHECK_DBL_BITS("0.0",   d1, 0x0000000000000000ULL);

    double d2 = 1.0;
    CHECK_DBL_BITS("1.0",   d2, 0x3FF0000000000000ULL);

    double d3 = -1.0;
    CHECK_DBL_BITS("-1.0",  d3, 0xBFF0000000000000ULL);

    double d4 = 100.0;
    CHECK_DBL_BITS("100.0", d4, 0x4059000000000000ULL);

    double d5 = 3.14159265358979;
    CHECK_DBL_BITS("pi",    d5, 0x400921FB54442D18ULL);

    /* Volatile to prevent constant folding */
    volatile double vd = 42.0;
    CHECK_DBL_BITS("volatile 42.0", vd, 0x4045000000000000ULL);

    printf("\n");
}

/* ---- 2B: Integer ↔ FP Conversions ---- */
static void test_fp_conversions(void) {
    printf("[FP 2B: Conversions]\n");

    /* -- fcvt.d.w (int32 → double) -- */
    int32_t i32 = 100;
    double from_i32 = (double)i32;
    CHECK_DBL_BITS("fcvt.d.w(100)", from_i32, 0x4059000000000000ULL);

    i32 = -1;
    from_i32 = (double)i32;
    CHECK_DBL_BITS("fcvt.d.w(-1)", from_i32, 0xBFF0000000000000ULL);

    /* -- fcvt.d.l (int64 → double) via inline asm -- */
    long l_in = 42;
    double d_out;
    __asm__ volatile ("fcvt.d.l %0, %1" : "=f"(d_out) : "r"(l_in));
    CHECK_DBL_BITS("asm fcvt.d.l(42)", d_out, 0x4045000000000000ULL);

    l_in = 16777216;
    __asm__ volatile ("fcvt.d.l %0, %1" : "=f"(d_out) : "r"(l_in));
    CHECK_DBL_BITS("asm fcvt.d.l(16M)", d_out, 0x4170000000000000ULL);

    /* -- fcvt.d.l via C cast (implicit long → double) -- */
    long lv = 100;
    double dv = (double)lv;
    CHECK_DBL_BITS("C cast long→double 100", dv, 0x4059000000000000ULL);

    /* -- fcvt.w.d (double → int32) -- */
    double d_100 = dbl_from_bits(0x4059000000000000ULL); /* 100.0 */
    int32_t to_i32;
    __asm__ volatile ("fcvt.w.d %0, %1, rtz" : "=r"(to_i32) : "f"(d_100));
    CHECK_INT("asm fcvt.w.d(100.0)", to_i32, 100);

    double d_neg = dbl_from_bits(0xC059000000000000ULL); /* -100.0 */
    __asm__ volatile ("fcvt.w.d %0, %1, rtz" : "=r"(to_i32) : "f"(d_neg));
    CHECK_INT("asm fcvt.w.d(-100.0)", to_i32, -100);

    /* -- fcvt.l.d (double → int64) -- */
    long to_i64;
    __asm__ volatile ("fcvt.l.d %0, %1, rtz" : "=r"(to_i64) : "f"(d_100));
    CHECK_INT("asm fcvt.l.d(100.0)", to_i64, 100);

    double d_big = dbl_from_bits(0x4170000000000000ULL); /* 16777216.0 */
    __asm__ volatile ("fcvt.l.d %0, %1, rtz" : "=r"(to_i64) : "f"(d_big));
    CHECK_INT("asm fcvt.l.d(16M)", to_i64, 16777216);

    /* -- fcvt.l.d via C cast -- */
    long c_cast = (long)d_100;
    CHECK_INT("C cast double→long 100.0", c_cast, 100);

    /* -- C cast (int)double — what PG's parse_int uses -- */
    int c_int = (int)d_100;
    CHECK_INT("C cast (int)100.0", c_int, 100);

    /* -- rint + cast — exact PG parse_int sequence -- */
    double val = dv; /* 100.0 from earlier */
    val = rint(val);
    CHECK_DBL_BITS("rint(100.0) bits", val, 0x4059000000000000ULL);
    int result = (int)val;
    CHECK_INT("(int)rint(100.0)", result, 100);

    /* -- fmv.x.d (copy FP bits to GP register, no conversion) -- */
    double fmv_in = dbl_from_bits(0x4059000000000000ULL);
    uint64_t fmv_out;
    __asm__ volatile ("fmv.x.d %0, %1" : "=r"(fmv_out) : "f"(fmv_in));
    CHECK("asm fmv.x.d(100.0)", fmv_out == 0x4059000000000000ULL);

    /* -- fmv.d.x (copy GP bits to FP register, no conversion) -- */
    uint64_t fmv_bits = 0x4045000000000000ULL; /* 42.0 */
    double fmv_d;
    __asm__ volatile ("fmv.d.x %0, %1" : "=f"(fmv_d) : "r"(fmv_bits));
    CHECK_DBL_BITS("asm fmv.d.x(42.0 bits)", fmv_d, 0x4045000000000000ULL);

    printf("\n");
}

/* ---- 2C: FP Memory Round-trips ---- */
static void test_fp_memory(void) {
    printf("[FP 2C: Memory Round-trips]\n");

    /* Store a double to stack, read back via integer, verify bits */
    volatile double stack_d = 100.0;
    uint64_t stack_bits;
    memcpy(&stack_bits, (const void *)&stack_d, 8);
    CHECK("stack round-trip 100.0", stack_bits == 0x4059000000000000ULL);

    /* Store via fsd, load via ld (integer) */
    double fsd_val = dbl_from_bits(0x4045000000000000ULL); /* 42.0 */
    uint64_t mem_buf;
    __asm__ volatile ("fsd %1, 0(%0)" : : "r"(&mem_buf), "f"(fsd_val) : "memory");
    CHECK("fsd+ld round-trip 42.0", mem_buf == 0x4045000000000000ULL);

    /* Store via sd (integer), load via fld */
    uint64_t store_bits = 0x4059000000000000ULL; /* 100.0 */
    double fld_val;
    uint64_t tmp_store = store_bits;
    __asm__ volatile (
        "sd %1, 0(%2)\n\t"
        "fld %0, 0(%2)"
        : "=f"(fld_val) : "r"(store_bits), "r"(&tmp_store) : "memory"
    );
    CHECK_DBL_BITS("sd+fld round-trip 100.0", fld_val, 0x4059000000000000ULL);

    /* Array of doubles */
    double arr[4] = {1.0, 2.0, 3.0, 4.0};
    CHECK_DBL_BITS("arr[0]=1.0", arr[0], 0x3FF0000000000000ULL);
    CHECK_DBL_BITS("arr[3]=4.0", arr[3], 0x4010000000000000ULL);

    printf("\n");
}

/* ---- 2D: FP Arithmetic ---- */
static void test_fp_arithmetic(void) {
    printf("[FP 2D: Arithmetic]\n");

    double a = dbl_from_bits(0x4000000000000000ULL); /* 2.0 */
    double b = dbl_from_bits(0x4008000000000000ULL); /* 3.0 */

    double sum;
    __asm__ volatile ("fadd.d %0, %1, %2" : "=f"(sum) : "f"(a), "f"(b));
    CHECK_DBL_BITS("fadd.d(2+3)", sum, 0x4014000000000000ULL); /* 5.0 */

    double diff;
    __asm__ volatile ("fsub.d %0, %1, %2" : "=f"(diff) : "f"(b), "f"(a));
    CHECK_DBL_BITS("fsub.d(3-2)", diff, 0x3FF0000000000000ULL); /* 1.0 */

    double prod;
    __asm__ volatile ("fmul.d %0, %1, %2" : "=f"(prod) : "f"(a), "f"(b));
    CHECK_DBL_BITS("fmul.d(2*3)", prod, 0x4018000000000000ULL); /* 6.0 */

    double quot;
    __asm__ volatile ("fdiv.d %0, %1, %2" : "=f"(quot) : "f"(b), "f"(a));
    CHECK_DBL_BITS("fdiv.d(3/2)", quot, 0x3FF8000000000000ULL); /* 1.5 */

    double sq;
    double four = dbl_from_bits(0x4010000000000000ULL); /* 4.0 */
    __asm__ volatile ("fsqrt.d %0, %1" : "=f"(sq) : "f"(four));
    CHECK_DBL_BITS("fsqrt.d(4)", sq, 0x4000000000000000ULL); /* 2.0 */

    /* C-level arithmetic (uses same instructions but compiler-scheduled) */
    volatile double ca = 10.0, cb = 3.0;
    volatile double cr = ca + cb;
    CHECK_DBL_BITS("C 10+3", cr, 0x402A000000000000ULL); /* 13.0 */
    cr = ca * cb;
    CHECK_DBL_BITS("C 10*3", cr, 0x403E000000000000ULL); /* 30.0 */

    printf("\n");
}

/* ---- 2E: FP Comparisons ---- */
static void test_fp_comparisons(void) {
    printf("[FP 2E: Comparisons]\n");

    double a = dbl_from_bits(0x4000000000000000ULL); /* 2.0 */
    double b = dbl_from_bits(0x4008000000000000ULL); /* 3.0 */
    double c = dbl_from_bits(0x4000000000000000ULL); /* 2.0 */

    long eq_ab, eq_ac, lt_ab, lt_ba, le_ac, le_ba;

    __asm__ volatile ("feq.d %0, %1, %2" : "=r"(eq_ab) : "f"(a), "f"(b));
    CHECK_INT("feq.d(2,3)", eq_ab, 0);

    __asm__ volatile ("feq.d %0, %1, %2" : "=r"(eq_ac) : "f"(a), "f"(c));
    CHECK_INT("feq.d(2,2)", eq_ac, 1);

    __asm__ volatile ("flt.d %0, %1, %2" : "=r"(lt_ab) : "f"(a), "f"(b));
    CHECK_INT("flt.d(2,3)", lt_ab, 1);

    __asm__ volatile ("flt.d %0, %1, %2" : "=r"(lt_ba) : "f"(b), "f"(a));
    CHECK_INT("flt.d(3,2)", lt_ba, 0);

    __asm__ volatile ("fle.d %0, %1, %2" : "=r"(le_ac) : "f"(a), "f"(c));
    CHECK_INT("fle.d(2,2)", le_ac, 1);

    __asm__ volatile ("fle.d %0, %1, %2" : "=r"(le_ba) : "f"(b), "f"(a));
    CHECK_INT("fle.d(3,2)", le_ba, 0);

    /* C-level comparisons */
    volatile double x = 100.0, y = 200.0;
    CHECK("C 100<200", x < y);
    CHECK("C 200>100", y > x);
    CHECK("C 100==100", x == 100.0);

    printf("\n");
}

/* ---- 2F: FP Across Function Calls ---- */
/* Force noinline to ensure real call/return with register save/restore */
static double __attribute__((noinline)) fp_identity(double x) {
    volatile double tmp = x; /* force use of FP register */
    return tmp;
}

static double __attribute__((noinline)) fp_add_one(double x) {
    return x + 1.0;
}

static void test_fp_calls(void) {
    printf("[FP 2F: Cross-call Persistence]\n");

    double d = 100.0;
    double r = fp_identity(d);
    CHECK_DBL_BITS("identity(100.0)", r, 0x4059000000000000ULL);

    r = fp_add_one(d);
    CHECK_DBL_BITS("add_one(100.0)", r, 0x4059400000000000ULL); /* 101.0 */

    /* Callee-saved: value should survive across calls */
    double before = 42.0;
    volatile double dummy = fp_identity(99.0); /* clobber caller-saved FP regs */
    (void)dummy;
    CHECK_DBL_BITS("survive call 42.0", before, 0x4045000000000000ULL);

    printf("\n");
}

/* ---- 2G: strtod Verification ---- */
static void test_strtod(void) {
    printf("[FP 2G: strtod]\n");

    /*
     * We know printf("%f") is broken. So verify strtod by extracting
     * the raw bits and comparing to known IEEE 754 values.
     */
    char *endptr;
    double d;

    d = strtod("100", &endptr);
    uint64_t b = dbl_bits(d);
    printf("  INFO: strtod(\"100\") bits=0x%016lx endptr='%s'\n", b, endptr);
    CHECK("strtod(\"100\") bits", b == 0x4059000000000000ULL);
    CHECK("strtod(\"100\") endptr", *endptr == '\0');

    d = strtod("3.14159", &endptr);
    b = dbl_bits(d);
    printf("  INFO: strtod(\"3.14159\") bits=0x%016lx\n", b);
    /* 3.14159 = 0x400921FA_F311F71C (approximately) */
    CHECK("strtod(\"3.14159\") nonzero", b != 0);
    /* Cross-check: cast to int should give 3 */
    CHECK_INT("strtod(\"3.14159\") (int)", (int)d, 3);

    d = strtod("1e6", &endptr);
    b = dbl_bits(d);
    printf("  INFO: strtod(\"1e6\") bits=0x%016lx\n", b);
    CHECK("strtod(\"1e6\") bits", b == 0x412E848000000000ULL);

    d = strtod("-42.5", &endptr);
    b = dbl_bits(d);
    printf("  INFO: strtod(\"-42.5\") bits=0x%016lx\n", b);
    CHECK("strtod(\"-42.5\") bits", b == 0xC045400000000000ULL);
    CHECK_INT("strtod(\"-42.5\") (int)", (int)d, -42);

    d = strtod("0", &endptr);
    CHECK("strtod(\"0\") bits", dbl_bits(d) == 0x0000000000000000ULL);

    printf("\n");
}

/* ---- 2H: PG parse_int End-to-End ---- */
static void test_pg_parse_int(void) {
    printf("[FP 2H: PG parse_int Simulation]\n");

    /*
     * Replicate exactly what PG's guc.c parse_int() does:
     *   double val;
     *   val = strtol(value, &endptr, 0);
     *   if (endptr has '.', 'e', or ERANGE) val = strtod(value, &endptr);
     *   val = rint(val);
     *   *result = (int) val;
     *
     * Verify using bit inspection, not printf("%f").
     */
    const char *value;
    char *endptr;
    double val;
    int result;

    /* Test 1: "100" (max_connections) */
    value = "100";
    errno = 0;
    val = strtol(value, &endptr, 0);
    printf("  INFO: strtol(\"%s\"): bits=0x%016lx endptr='%s' (int)=%d\n",
           value, dbl_bits(val), endptr, (int)val);
    if (*endptr == '.' || *endptr == 'e' || *endptr == 'E' || errno == ERANGE) {
        errno = 0;
        val = strtod(value, &endptr);
        printf("  INFO: strtod fallback: bits=0x%016lx\n", dbl_bits(val));
    }
    val = rint(val);
    result = (int) val;
    CHECK_INT("parse_int(\"100\")", result, 100);

    /* Test 2: "16777216" (wal_segment_size) */
    value = "16777216";
    errno = 0;
    val = strtol(value, &endptr, 0);
    printf("  INFO: strtol(\"%s\"): bits=0x%016lx (int)=%d\n",
           value, dbl_bits(val), (int)val);
    if (*endptr == '.' || *endptr == 'e' || *endptr == 'E' || errno == ERANGE) {
        errno = 0;
        val = strtod(value, &endptr);
    }
    val = rint(val);
    result = (int) val;
    CHECK_INT("parse_int(\"16777216\")", result, 16777216);

    /* Test 3: "20" */
    value = "20";
    errno = 0;
    val = strtol(value, &endptr, 0);
    result = (int) rint(val);
    CHECK_INT("parse_int(\"20\")", result, 20);

    /* Test 4: critical operation — long→double→int round-trip */
    long lv = strtol("100", NULL, 0);
    double dv = lv;
    printf("  INFO: long→double→int: lv=%ld bits=0x%016lx (int)=%d\n",
           lv, dbl_bits(dv), (int)dv);
    CHECK_INT("long→double→int 100", (int)dv, 100);
    CHECK("long→double bits", dbl_bits(dv) == 0x4059000000000000ULL);

    printf("\n");
}

/* ================================================================
 * 3. String Formatting (snprintf, sprintf)
 * PG uses snprintf extensively for error messages and GUC display.
 * ================================================================ */
static void test_string_formatting(void) {
    printf("[String Formatting]\n");
    char buf[256];

    snprintf(buf, sizeof(buf), "%d", 100);
    CHECK("snprintf %%d 100", strcmp(buf, "100") == 0);

    snprintf(buf, sizeof(buf), "%d", 0);
    CHECK("snprintf %%d 0", strcmp(buf, "0") == 0);

    snprintf(buf, sizeof(buf), "%ld", 8388608L);
    CHECK("snprintf %%ld 8388608", strcmp(buf, "8388608") == 0);

    snprintf(buf, sizeof(buf), "%d %s %d", 100, "kB", 2048);
    CHECK("snprintf mixed", strcmp(buf, "100 kB 2048") == 0);

    snprintf(buf, sizeof(buf), "%zd", (ssize_t)524288);
    CHECK("snprintf %%zd 524288", strcmp(buf, "524288") == 0);

    /* PG's GUC error format */
    snprintf(buf, sizeof(buf), "%d %s is outside the valid range for parameter \"%s\" (%d %s .. %d %s)",
             0, "kB", "max_stack_depth", 100, "kB", 2147483647, "kB");
    CHECK("snprintf GUC error format", strstr(buf, "0 kB is outside") != NULL);

    printf("\n");
}

/* ================================================================
 * 4. Arithmetic and Bit Operations
 * Verify basic integer and FP math on RISC-V.
 * ================================================================ */
static void test_arithmetic(void) {
    printf("[Arithmetic]\n");

    /* Integer division */
    CHECK_INT("8388608 / 1024", 8388608 / 1024, 8192);
    CHECK_INT("(8388608 - 524288) / 1024", (8388608 - 524288) / 1024, 7680);

    /* Signed arithmetic */
    CHECK_INT("-524288 / 1024", -524288 / 1024, -512);

    /* 64-bit multiplication */
    long long a = 1000000LL, b = 1000000LL;
    CHECK("1M * 1M == 1T", a * b == 1000000000000LL);

    /* Bit operations */
    CHECK_INT("0xFF & 0x0F", 0xFF & 0x0F, 0x0F);
    CHECK_INT("1 << 20", 1 << 20, 1048576);
    CHECK_INT("1UL << 63", (long)(1UL << 63), (long)(-9223372036854775807L - 1));

    /* Pointer size */
    CHECK_INT("sizeof(void*)", sizeof(void*), 8);
    CHECK_INT("sizeof(long)", sizeof(long), 8);
    CHECK_INT("sizeof(size_t)", sizeof(size_t), 8);

    printf("\n");
}

/* ================================================================
 * 5. Memory Operations
 * ================================================================ */
static void test_memory(void) {
    printf("[Memory]\n");

    char src[] = "Hello, RISC-V!";
    char dst[32] = {0};

    memcpy(dst, src, strlen(src) + 1);
    CHECK("memcpy", strcmp(dst, "Hello, RISC-V!") == 0);

    /* Overlapping memmove */
    char overlap[] = "ABCDEFGH";
    memmove(overlap + 2, overlap, 6);
    CHECK("memmove overlap", memcmp(overlap, "ABABCDEF", 8) == 0);

    /* memset */
    char zeros[16];
    memset(zeros, 0, sizeof(zeros));
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (zeros[i]) all_zero = 0;
    CHECK("memset zero", all_zero);

    /* malloc/free */
    char *p = malloc(4096);
    CHECK("malloc(4096) != NULL", p != NULL);
    if (p) {
        memset(p, 0xAA, 4096);
        CHECK("malloc write+read", (unsigned char)p[0] == 0xAA && (unsigned char)p[4095] == 0xAA);
        free(p);
    }

    printf("\n");
}

/* ================================================================
 * 6. getopt Argument Parsing
 * PG's bootstrap mode uses getopt to parse -c, -X, etc.
 * ================================================================ */
static void test_getopt(void) {
    printf("[getopt]\n");

    /* Simulate: postgres --check -F -c max_connections=100 -X 16777216 */
    optind = 1;  /* Reset getopt state */
    char *test_argv[] = {
        "postgres",
        "-F",
        "-c", "max_connections=100",
        "-X", "16777216",
        NULL
    };
    int test_argc = 6;

    int got_F = 0, got_c = 0, got_X = 0;
    char *c_val = NULL, *X_val = NULL;
    int ch;

    while ((ch = getopt(test_argc, test_argv, "Fc:X:")) != -1) {
        switch (ch) {
            case 'F': got_F = 1; break;
            case 'c': got_c = 1; c_val = optarg; break;
            case 'X': got_X = 1; X_val = optarg; break;
        }
    }

    CHECK("getopt: got -F", got_F);
    CHECK("getopt: got -c", got_c);
    CHECK("getopt: got -X", got_X);
    CHECK("getopt: -c val is 'max_connections=100'",
          c_val && strcmp(c_val, "max_connections=100") == 0);
    CHECK("getopt: -X val is '16777216'",
          X_val && strcmp(X_val, "16777216") == 0);

    /* Parse the value from -c */
    if (c_val) {
        char *eq = strchr(c_val, '=');
        if (eq) {
            long parsed = strtol(eq + 1, NULL, 0);
            CHECK_INT("getopt -c parsed value", parsed, 100);
        } else {
            CHECK("getopt -c has '='", 0);
        }
    }

    /* Parse the value from -X */
    if (X_val) {
        long parsed = strtol(X_val, NULL, 0);
        CHECK_INT("getopt -X parsed value", parsed, 16777216);
    }

    printf("\n");
}

/* ================================================================
 * 7. Process / Fork+Exec / Argument Passing
 * Verify that fork+exec preserves arguments correctly.
 * ================================================================ */
static void test_fork_exec(void) {
    printf("[Fork/Exec]\n");

    /* Basic fork */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exit with a known code */
        _exit(42);
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK_INT("fork child exit code", WEXITSTATUS(st), 42);

    /* Fork + exec argv passing via /proc/self/exe --selftest-echo */
    /* We can't easily exec ourselves with different args without
     * a helper, so just test that fork + waitpid works. */

    printf("\n");
}

/* ================================================================
 * 8. System Calls / Resource Limits
 * ================================================================ */
static void test_syscalls(void) {
    printf("[Syscalls]\n");

    /* getpid */
    pid_t p = getpid();
    CHECK("getpid() > 0", p > 0);

    /* getrlimit */
    struct rlimit rl;
    int rc = getrlimit(RLIMIT_STACK, &rl);
    CHECK_INT("getrlimit(STACK) rc", rc, 0);
    printf("  INFO: RLIMIT_STACK cur=%lu max=%lu\n",
           (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);

    /* setrlimit + verify */
    struct rlimit rl_new = { .rlim_cur = 4*1024*1024, .rlim_max = 4*1024*1024 };
    rc = setrlimit(RLIMIT_STACK, &rl_new);
    CHECK_INT("setrlimit(STACK, 4MB) rc", rc, 0);
    struct rlimit rl_check = {0};
    getrlimit(RLIMIT_STACK, &rl_check);
    CHECK_INT("getrlimit after setrlimit cur", (long)rl_check.rlim_cur, 4*1024*1024);

    /* Verify rlimit survives fork */
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit child_rl = {0};
        getrlimit(RLIMIT_STACK, &child_rl);
        /* Exit 0 if matches, 1 if not */
        _exit(child_rl.rlim_cur == 4*1024*1024 ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK("rlimit survives fork", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* File I/O */
    int fd = open("/tmp/rv64_test", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("open(/tmp/rv64_test) >= 0", fd >= 0);
    if (fd >= 0) {
        const char *data = "test123";
        ssize_t w = write(fd, data, 7);
        CHECK_INT("write 7 bytes", w, 7);
        close(fd);

        fd = open("/tmp/rv64_test", O_RDONLY);
        char rbuf[16] = {0};
        ssize_t r = read(fd, rbuf, sizeof(rbuf));
        CHECK_INT("read back 7 bytes", r, 7);
        CHECK("read data matches", strcmp(rbuf, "test123") == 0);
        close(fd);
        unlink("/tmp/rv64_test");
    }

    printf("\n");
}

/* ================================================================
 * 9. Struct Layout / ABI Verification
 * Verify that struct sizes match expectations for LP64D ABI.
 * ================================================================ */
static void test_abi(void) {
    printf("[ABI]\n");

    CHECK_INT("sizeof(int)", sizeof(int), 4);
    CHECK_INT("sizeof(long)", sizeof(long), 8);
    CHECK_INT("sizeof(long long)", sizeof(long long), 8);
    CHECK_INT("sizeof(double)", sizeof(double), 8);
    CHECK_INT("sizeof(float)", sizeof(float), 4);
    CHECK_INT("sizeof(pid_t)", sizeof(pid_t), 4);

    /* Endianness check */
    uint32_t x = 0x01020304;
    unsigned char *b = (unsigned char *)&x;
    CHECK("little-endian", b[0] == 0x04 && b[1] == 0x03 && b[2] == 0x02 && b[3] == 0x01);

    /* Alignment */
    struct { char c; long l; } align_test;
    CHECK("long alignment", (uintptr_t)&align_test.l % 8 == 0 ||
                             (uintptr_t)&align_test.l % sizeof(long) == 0);

    printf("\n");
}

/* ================================================================
 * 10. Environment Variables
 * PG reads PGDATA, PGPORT, etc. from env.
 * ================================================================ */
static void test_env(void) {
    printf("[Environment]\n");

    setenv("RV64_TEST_VAR", "hello123", 1);
    const char *v = getenv("RV64_TEST_VAR");
    CHECK("setenv+getenv", v && strcmp(v, "hello123") == 0);

    unsetenv("RV64_TEST_VAR");
    v = getenv("RV64_TEST_VAR");
    CHECK("unsetenv", v == NULL);

    /* PATH should be set */
    v = getenv("PATH");
    CHECK("PATH is set", v != NULL);

    printf("\n");
}

/* ================================================================ */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== RISC-V Environment Test Suite ===\n\n");

    test_integer_parsing();

    /* FP tests — ordered by dependency so first failure points to root cause */
    test_fp_literals();       /* 2A: can we load FP constants at all? */
    test_fp_conversions();    /* 2B: int↔FP conversion instructions */
    test_fp_memory();         /* 2C: FP store/load through guest RAM */
    test_fp_arithmetic();     /* 2D: fadd, fsub, fmul, fdiv, fsqrt */
    test_fp_comparisons();    /* 2E: feq, flt, fle */
    test_fp_calls();          /* 2F: FP values survive function calls? */
    test_strtod();            /* 2G: strtod — broken or just printf? */
    test_pg_parse_int();      /* 2H: full PG parse_int simulation */

    test_string_formatting();
    test_arithmetic();
    test_memory();
    test_getopt();
    test_fork_exec();
    test_syscalls();
    test_abi();
    test_env();

    printf("=== Results: %d/%d passed", tests - failures, tests);
    if (failures > 0)
        printf(", %d FAILED", failures);
    printf(" ===\n");

    return failures > 0 ? 1 : 0;
}
