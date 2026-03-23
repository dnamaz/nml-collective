/*
 * stdarg.h for NML Edge Worker — ARM Cortex-M4 bare metal.
 *
 * GCC provides __builtin_va_* in all modes (hosted and freestanding).
 * This header maps the standard names onto the builtins so our printf.c
 * and any user code can use va_list / va_start / va_arg / va_end portably.
 *
 * When using gcc-arm-embedded + newlib-nano this file is superseded by the
 * toolchain's own stdarg.h — remove it to let the toolchain header win.
 */
#ifndef _EDGE_STDARG_H
#define _EDGE_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#define va_copy(dst, src)   __builtin_va_copy(dst, src)

#endif /* _EDGE_STDARG_H */
