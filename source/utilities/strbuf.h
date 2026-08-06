#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

/* Growable string buffer shared by value formatting (vm.c) and JSON encoding (aer_json.c).
   Always NUL-terminated; the caller owns buf and frees it (or hands it to aer_make_string). */
typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} StrBuf;

void strbuf_init(StrBuf* b);
void strbuf_append_n(StrBuf* b, const char* s, size_t n);
void strbuf_append(StrBuf* b, const char* s);
void strbuf_append_char(StrBuf* b, char ch);

#endif
