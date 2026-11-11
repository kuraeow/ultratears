// bufcat: append formatted text to a fixed buffer without ever writing past it. at = bufcat(buf, sizeof buf, at, ...)
// takes the length so far and returns the new one, at most size - 1: text that does not fit is cut off and later calls
// write nothing. The pattern it replaces, at += snprintf(buf + at, sizeof buf - at, ...), leaves at past the end once a
// piece is cut off, and the next call's size underflows (CodeQL cpp/overflowing-snprintf).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static inline int bufcat(char *buf, size_t size, int at, const char *fmt, ...) {
    if (!size) return 0;
    if (at < 0) at = 0;
    if ((size_t)at >= size - 1) { buf[size - 1] = 0; return (int)(size - 1); }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + at, size - (size_t)at, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[at] = 0; return at; }
    return (size_t)at + (size_t)n >= size ? (int)(size - 1) : at + n;
}
