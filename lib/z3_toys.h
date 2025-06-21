/**
 * z3_toys.h
 *
 * Description:
 *   General utility macros and functions for common operations.
 *
 * Features:
 *   - Error printing macros
 *   - Warning silence utilities
 *   - Array manipulation helpers
 *
 * Requires:
 *   - C23 Standard (Use -std=c23).
 *
 */
#pragma once

#ifndef __STDC_VERSION__
#error A modern C standard (like C23) is required
#elif __STDC_VERSION__ < 202311L
#error This code must be compiled with -std=c23
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Allow scoped cleanup in implementations
#ifndef Z3_IMPL_SCOPED
#define __Z3_TOYS_SCOPED
#endif

// Compiler-specific warning suppression
#if defined(__GNUC__) || defined(__clang__)
#define IGNORE_UNUSED(declaration)                                                \
  _Pragma ("GCC diagnostic push") _Pragma ("GCC diagnostic ignored \"-Wunused\"") \
      declaration _Pragma ("GCC diagnostic pop")
#else
#define IGNORE_UNUSED(declaration) declaration
#endif

// I enable **all** warnings, I want to pick which one to silence
// but still get them
#if defined(__GNUC__) || defined(__clang__)
#define IGNORE_WARNING(declaration)                                            \
  _Pragma ("GCC diagnostic push") _Pragma ("GCC diagnostic ignored \"-Wall\"") \
      declaration _Pragma ("GCC diagnostic pop")
#else
#define IGNORE_WARNING(declaration) declaration
#endif

//~ Print formatted error message to stderr with red coloring
#define eprintf(fmt, ...) \
  fprintf (stderr, "\x1b[38;5;9m[ERROR] " fmt "\x1b[0m" __VA_OPT__ (, ) __VA_ARGS__)

//~ Pop an element from the beginning of an array
#define popf(c, v)                                                                    \
  (c > 0 ? (--c, *v++)                                                                \
         : (eprintf ("Trying to access a non-existent value\n"), exit (EXIT_FAILURE), \
            (void *)0))

//~ Calculate the next power of 2 greater than or equal to n
size_t next_power_of2(size_t n);

#ifdef Z3_TOYS_IMPL
// Implementation of utility functions

//~ Calculate the next power of 2 greater than or equal to n
size_t next_power_of2(size_t n) {
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

#endif // Z3_TOYS_IMPL
