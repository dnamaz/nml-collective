/*
 * Minimal math.h for NML Edge Worker — ARM Cortex-M4 bare metal.
 *
 * The Cortex-M4 FPU handles single-precision in hardware; double-precision
 * operations are software-emulated.  Link with -lm (newlib-nano) or provide
 * your own soft-float math library.
 *
 * When using gcc-arm-embedded + newlib-nano, remove this file.
 */
#ifndef _EDGE_MATH_H
#define _EDGE_MATH_H

/* Common constants */
#ifndef M_PI
#define M_PI   3.14159265358979323846
#endif
#ifndef M_E
#define M_E    2.71828182845904523536
#endif

/* ── Double-precision ────────────────────────────────────────────────────── */

double sqrt(double x);
double cbrt(double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double modf(double x, double *iptr);

int    isnan(double x);
int    isinf(double x);
int    isfinite(double x);

/* ── Single-precision (FPU-accelerated on M4) ────────────────────────────── */

float sqrtf(float x);
float expf(float x);
float logf(float x);
float powf(float x, float y);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float fmodf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);
float tanhf(float x);

#endif /* _EDGE_MATH_H */
