/**
 * z3_string.h
 *
 * Description:
 *   Provides heap-allocated string management with automatic memory handling.
 *
 * Features:
 *   - Growable heap-allocated strings
 *   - String interpolation
 *   - String escape/unescape utilities
 *   - Scoped resource cleanup for string memory
 *
 * Requires:
 *   - C23 Standard (Use -std=c23).
 *   - z3_toys.h
 *
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

//~ Heap-allocated growable string
typedef struct {
  size_t max; /**< Maximum capacity */
  size_t len; /**< Current length (excluding null terminator) */
  char *chr;  /**< Pointer to the character array */
} String;

//~ Print debug information about a string
#define z3_str_dbg(s)                                                                         \
  printf (                                                                                    \
      #s " = String {\n  len: %zu,\n  max: %zu,\n  chr: '%s'\n}\n", (s).len, (s).max, (s).chr \
  );

//~ Create a new empty String, with at least `min` capacity
String z3_str (size_t min);

//~ Create a new String from a C-style string
//! This does look for `\0` terminator
String z3_strcpy (const char *s);

//~ Create a duplicate of an existing String
String z3_strdup (const String *str);

//~ Append a single char to a String
void z3_pushc (String *str, const char s);

//~ Append a C-style string to a String
void z3_pushl (String *str, const char *s, size_t l);

//~ Ensure String is null terminated
void z3_ensure0 (String *str);

//~ Ensure String has enough allocated memory
void z3_reserve (String *str, size_t additional);

//~ Free the memory used by a String
void z3_drops (String *str);

//~ Escape a string, converting control characters to escape sequences
String z3_escape (const char *input, size_t len);

//~ Unescape a string, converting escape sequences to their respective characters
String z3_unescape (const char *input, size_t len);

//~ Interpolate a template string by replacing placeholders with values
//
//~ This function takes ctx and a template string containing placeholders in the format
//  `#{<possible_id>}`, ~ and generates a new string by replacing those placeholders with
//  corresponding values obtained ~ from the provided `filler` function. The `filler` function
//  takes the placeholder's ID (as a string) ~ and returns the replacement string.
//
//~ Note: The original template string is not modified. A new `String` is created with the
//  interpolated values.
String z3_interp (
    const String *templt, bool (*filler) (String *, void *, char *, size_t), void *ctx
);

#ifdef Z3_TOYS_SCOPED
//~ Cleanup function for String (used with attribute cleanup)
void __cleanup_String (String *s);

//~ Define a String with automatic cleanup
#define ScopedString __attribute__ ((cleanup (z3_drops))) String
#endif  // Z3_TOYS_SCOPED

#ifdef Z3_STRING_IMPL
#include <ctype.h>
#include "z3_toys.h"

void z3_reserve (String *str, size_t additional) {
  if (!str || !str->chr) return;

  if (str->len + additional + 1 > str->max) {
    while (str->max < str->len + additional + 1) {
      str->max *= 2;
    }

    str->chr = realloc (str->chr, str->max);
    if (str->chr == NULL) {
      eprintf ("String realloc: requested %zu bytes\n", str->max);
      exit (EXIT_FAILURE);
    }
  }
}

void z3_ensure0 (String *str) {
  if (!str || !str->chr) return;

  if (str->chr[str->len] == '\0') return;
  str->chr[str->len] = '\0';
}

void z3_pushc (String *str, char c) {
  if (!str || !str->chr) return;

  z3_reserve (str, 1);
  str->chr[str->len] = c;
  str->len++;
  str->chr[str->len] = '\0';
}

void z3_pushl (String *str, const char *s, size_t l) {
  if (!str || !str->chr || !s || l == 0) return;

  z3_reserve (str, l);
  memcpy (str->chr + str->len, s, l);
  str->len += l;
  str->chr[str->len] = '\0';
}

String z3_str (size_t min) {
  String str = {0};
  str.max = next_power_of2 (min);  // Initial capacity
  str.len = 0;
  str.chr = malloc (str.max);
  if (str.chr) {
    str.chr[0] = '\0';
  }
  return str;
}

String z3_strcpy (const char *s) {
  String str = {0};
  size_t len = strlen (s) + 1;
  str.max = ((len & (len - 1)) == 0) ? len : next_power_of2 (len);
  str.len = len - 1;
  str.chr = malloc (str.max);
  if (str.chr == NULL) {
    fprintf (stderr, "failed to allocate memory for string\n");
    exit (EXIT_FAILURE);
  }
  strcpy (str.chr, s);
  str.chr[str.len] = '\0';
  return str;
}

String z3_strdup (const String *str) {
  String s = {0};
  if (!str || !str->chr) return s;

  s.max = str->max;
  s.len = str->len;
  s.chr = malloc (s.max);

  if (s.chr) {
    memcpy (s.chr, str->chr, str->len);
    s.chr[s.len] = '\0';
  }

  return s;
}

void z3_drops (String *str) {
  if (!str || !str->chr) return;

  free (str->chr);
  str->chr = NULL;
  str->len = 0;
  str->max = 0;
}

String z3_interp (
    const String *tmplt, bool (*filler) (String *, void *, char *, size_t), void *ctx
) {
  String result = z3_str (32);

  size_t i = 0;

  while (i < tmplt->len) {
    if (tmplt->chr[i] == '\\') {
      z3_pushc (&result, (i++, tmplt->chr[i++]));
      continue;
    }

    if (!(i + 1 < tmplt->len && tmplt->chr[i] == '#' && tmplt->chr[i + 1] == '{')) {
      z3_pushc (&result, tmplt->chr[i]);
      i++;
      continue;
    }

    size_t path_start = i + 2;  // Skip "#{"
    size_t path_end = path_start;

    // Find the closing '}'
    while (path_end < tmplt->len && tmplt->chr[path_end] != '}') {
      if (!(isalnum (tmplt->chr[path_end]) || tmplt->chr[path_end] == '_' ||
            tmplt->chr[path_end] == '-'))
        break;
      path_end++;
    }
    if (tmplt->chr[path_end] != '}' || path_end >= tmplt->len) {
      // No closing '}' found, treat as literal text
      size_t path_len = path_end - path_start + 2;
      char *path = malloc (path_len + 1);
      if (path) {
        memcpy (path, tmplt->chr + path_start - 2, path_len);
        path[path_len] = '\0';
        z3_pushl (&result, path, path_len);
        free (path);
      }
      i += path_len;
      continue;
    }
    size_t path_len = path_end - path_start;
    char *path = (char *)(tmplt->chr + path_start);

    if (!filler (&result, ctx, path, path_len)) {
      z3_pushl (&result, tmplt->chr + i, path_len + 3);  // push entire #{...}
    }

    // Move past the closing '}'
    i = path_end + 1;
  }

  return result;
}

String z3_escape (const char *input, size_t len) {
  char hex_digits[] = "0123456789abcdef";
  String s = z3_str (32);
  size_t l = 0;

  // loop until `\0`, or until length
  while (l < len && *input) {
    unsigned char c = *input;
    switch (c) {
      case '\a':
        z3_pushl (&s, "\\a", 2);
        break;  // Bell
      case '\b':
        z3_pushl (&s, "\\b", 2);
        break;  // Backspace
      case '\f':
        z3_pushl (&s, "\\f", 2);
        break;  // Formfeed
      case '\n':
        z3_pushl (&s, "\\n", 2);
        break;
      case '\r':
        z3_pushl (&s, "\\r", 2);
        break;
      case '\t':
        z3_pushl (&s, "\\t", 2);
        break;
      case '\v':
        z3_pushl (&s, "\\v", 2);
        break;  // Vertical tab
      case '\\':
        z3_pushl (&s, "\\\\", 2);
        break;
      case '\"':
        z3_pushl (&s, "\\\"", 2);
        break;
      case '\'':
        z3_pushl (&s, "\\\'", 2);
        break;

      // Printable ASCII (0x20 - 0x7E), no need to escape
      default:
        if (c < 0x20 || c > 0x7E) {
          // Non-printables escaped as hex
          z3_pushc (&s, '\\');  // Escape char
          z3_pushc (&s, 'x');   // 'x' for hex escape

          z3_pushc (&s, hex_digits[(c >> 4) & 0xF]);
          z3_pushc (&s, hex_digits[c & 0xF]);
        } else {
          z3_pushc (&s, c);
        }
        break;
    }
    input++;
    l++;
  }
  z3_ensure0 (&s);
  return s;
}

String z3_unescape (const char *input, size_t len) {
  String s = z3_str (32);
  size_t l = 0;

  // loop until `\0`, or until length
  while (l < len && *input) {
    if (*input == '\\') {
      input++;  // Skip the backslash

      switch (*input) {
        case 'a':
          z3_pushc (&s, '\a');
          break;
        case 'b':
          z3_pushc (&s, '\b');
          break;
        case 'f':
          z3_pushc (&s, '\f');
          break;
        case 'n':
          z3_pushc (&s, '\n');
          break;
        case 'r':
          z3_pushc (&s, '\r');
          break;
        case 't':
          z3_pushc (&s, '\t');
          break;
        case 'v':
          z3_pushc (&s, '\v');
          break;
        case '\\':
          z3_pushc (&s, '\\');
          break;
        case '\"':
          z3_pushc (&s, '\"');
          break;
        case '\'':
          z3_pushc (&s, '\'');
          break;

        case 'x': {
          input++;  // Skip 'x'
          if (!isxdigit (*input)) {
            z3_pushl (&s, "\\x", 2);
            z3_pushc (&s, *input++);
            break;
          }
          l++;
          unsigned char byte_value = 0;
          char c = *input++;

          if (c >= '0' && c <= '9')
            byte_value |= (c - '0');
          else if (c >= 'a' && c <= 'f')
            byte_value |= (c - 'a' + 10);
          else if (c >= 'A' && c <= 'F')
            byte_value |= (c - 'A' + 10);

          c = *input;
          byte_value <<= 4;

          if (c >= '0' && c <= '9')
            byte_value |= (c - '0');
          else if (c >= 'a' && c <= 'f')
            byte_value |= (c - 'a' + 10);
          else if (c >= 'A' && c <= 'F')
            byte_value |= (c - 'A' + 10);
          z3_pushc (&s, byte_value);

          break;
        }

        // In case of invalid escape, just add the backslash
        default:
          z3_pushc (&s, '\\');
          z3_pushc (&s, *input);
          break;
      }
    } else {
      z3_pushc (&s, *input);
    }
    input++;
    l++;
  }
  z3_ensure0 (&s);
  return s;
}

#ifdef Z3_TOYS_SCOPED
void __cleanup_String (String *s) {
  z3_drops (s);
}
#endif  // Z3_TOYS_SCOPED

#endif  // Z3_STRING_IMPL
