#include <string.h>
#include "strbuf.h"
#include "error.h"

void strbuf_init(StrBuf* b) {
    b->cap = 64;
    b->buf = xmalloc(b->cap);
    b->len = 0;
    b->buf[0] = '\0';
}

void strbuf_append_n(StrBuf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap)
            b->cap *= 2;
        b->buf = xrealloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

void strbuf_append(StrBuf* b, const char* s) {
    strbuf_append_n(b, s, strlen(s));
}
void strbuf_append_char(StrBuf* b, char ch) {
    strbuf_append_n(b, &ch, 1);
}
