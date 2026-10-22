#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/* float point form */
#define FP_SHIFT  14
#define FP_F      (1 << FP_SHIFT)

typedef int fp_t;

/* int -> fixed */
static inline
fp_t int_to_fixed(int n) { return n * FP_F; }

/* fixed -> int(trunc) */
static inline
int fixed_to_int_trunc(fp_t x) { return x / FP_F; }

/* fixed -> int(round) */
static inline
int fixed_to_int_round(fp_t x) { return (x >= 0) ? (x + FP_F / 2) / FP_F
                                                 : (x - FP_F / 2) / FP_F; }
/* fixed +- fixed */
static inline
fp_t fixed_add(fp_t x1, fp_t x2) { return x1 + x2; }

static inline
fp_t fixed_sub(fp_t x1, fp_t x2) { return x1 - x2; }

/* fixed +- int */
static inline
fp_t fixed_add_int(fp_t x, int n) { return x + n * FP_F; }

static inline
fp_t fixed_sub_int(fp_t x, int n) { return x - n * FP_F; }

/* fixed mul/div fixed */
static inline
fp_t fixed_mul(fp_t x1, fp_t x2) { return ((int64_t) x1) * x2 / FP_F; }

static inline
fp_t fixed_div(fp_t x1, fp_t x2) { return ((int64_t) x1) * FP_F / x2; }

/* fixed mul/div int */
static inline
fp_t fixed_mul_int(fp_t x, int n) { return x * n; }

static inline
fp_t fixed_div_int(fp_t x, int n) { return x / n; }

#endif
