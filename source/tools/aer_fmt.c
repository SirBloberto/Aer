/* A standalone AER source formatter with its own tokenizer: the real lexer discards comments and
   exact whitespace, which is precisely what a formatter needs. Only the indent-stack algorithm is
   duplicated from it on purpose. Style is 4-space indents, single spaces around binary operators,
   none inside brackets or around '.'/'..'; comments and string literals are reproduced verbatim.
   Only whitespace ever changes, so output is always semantically identical.
   Usage: aer-fmt <file.aer> prints to stdout; aer-fmt -w <file.aer> formats in place. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* ------------------------------------------------------------------ */
/* Tokenizing                                                           */
/* ------------------------------------------------------------------ */

typedef enum { T_WORD, T_NUMBER, T_STRING, T_COMMENT, T_OP, T_EOF } TokType;

typedef struct {
    TokType type;
    const char* text;
    unsigned int len;
    unsigned int line;
    unsigned int col; /* 0-based column of the first char, original source */
    unsigned int blank_before; /* count of fully-blank source lines immediately before this token */
    bool line_start; /* first non-whitespace token on its original source line */
} Tok;

typedef struct {
    Tok* items;
    unsigned int count, cap;
} TokList;

static void tl_push(TokList* tl, Tok t) {
    if (tl->count >= tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 256;
        tl->items = realloc(tl->items, sizeof(Tok) * tl->cap);
    }
    tl->items[tl->count++] = t;
}

static const char* const OPS3[] = {"//=", NULL};
static const char* const OPS2[] = {
    "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "//", "<<", ">>", "..", "|>", NULL};

static TokList tokenize(const char* src) {
    TokList tl = {0};
    const char* p = src;
    const char* line_begin = src;
    unsigned int line = 1;
    unsigned int blank_run = 0;
    bool seen_token_on_line = false;

    while (*p) {
        if (*p == '\n') {
            if (!seen_token_on_line) blank_run++;
            line++;
            p++;
            line_begin = p;
            seen_token_on_line = false;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }

        unsigned int col = (unsigned int)(p - line_begin);
        bool at_line_start = !seen_token_on_line;
        const char* start = p;
        TokType type;

        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            type = T_COMMENT;
        } else if (*p == '"') {
            if (p[1] == '"' && p[2] == '"') {
                p += 3;
                while (*p && !(p[0] == '"' && p[1] == '"' && p[2] == '"'))
                    p++;
                if (*p) p += 3;
            } else {
                p++;
                while (*p && *p != '"' && *p != '\n') {
                    if (*p == '\\' && p[1]) p++;
                    p++;
                }
                if (*p == '"') p++;
            }
            type = T_STRING;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            type = T_WORD;
        } else if (isdigit((unsigned char)*p)) {
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
                p += 2;
                while (isxdigit((unsigned char)*p))
                    p++;
            } else {
                while (isdigit((unsigned char)*p))
                    p++;
                if (*p == '.' && isdigit((unsigned char)p[1])) {
                    p++;
                    while (isdigit((unsigned char)*p))
                        p++;
                }
            }
            type = T_NUMBER;
        } else {
            unsigned int mlen = 1;
            for (int i = 0; OPS3[i] && mlen == 1; i++)
                if (!strncmp(p, OPS3[i], 3)) mlen = 3;
            if (mlen == 1)
                for (int i = 0; OPS2[i]; i++)
                    if (!strncmp(p, OPS2[i], 2)) {
                        mlen = 2;
                        break;
                    }
            p += mlen;
            type = T_OP;
        }

        Tok t = {type, start, (unsigned int)(p - start), line, col, blank_run, at_line_start};
        tl_push(&tl, t);
        blank_run = 0;
        seen_token_on_line = true;
    }
    Tok eof = {T_EOF, p, 0, line, 0, blank_run, true};
    tl_push(&tl, eof);
    return tl;
}

/* ------------------------------------------------------------------ */
/* Logical lines -- one statement, possibly spanning several source     */
/* lines while a ( [ { is still open, exactly like the real lexer's     */
/* own bracket_depth-gated newline handling.                            */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int start, end;
    unsigned int orig_col;
    unsigned int blank_before;
    int depth;
} LLine;
typedef struct {
    LLine* items;
    unsigned int count, cap;
} LLineList;

static void ll_push(LLineList* ll, LLine l) {
    if (ll->count >= ll->cap) {
        ll->cap = ll->cap ? ll->cap * 2 : 64;
        ll->items = realloc(ll->items, sizeof(LLine) * ll->cap);
    }
    ll->items[ll->count++] = l;
}

static bool op_is(const Tok* t, const char* s) {
    size_t n = strlen(s);
    return t->type == T_OP && t->len == n && !strncmp(t->text, s, n);
}
static bool is_open_bracket(const Tok* t) {
    return t->type == T_OP && t->len == 1 && strchr("([{", t->text[0]);
}
static bool is_close_bracket(const Tok* t) {
    return t->type == T_OP && t->len == 1 && strchr(")]}", t->text[0]);
}

static LLineList group_logical_lines(TokList* tl) {
    LLineList out = {0};
    int bracket_depth = 0;
    unsigned int i = 0;
    while (i < tl->count && tl->items[i].type != T_EOF) {
        unsigned int start = i;
        unsigned int orig_col = tl->items[i].col;
        unsigned int blank_before = tl->items[i].blank_before;
        while (i < tl->count && tl->items[i].type != T_EOF) {
            Tok* t = &tl->items[i];
            if (i > start && t->line != tl->items[i - 1].line && bracket_depth == 0) break;
            if (is_open_bracket(t))
                bracket_depth++;
            else if (is_close_bracket(t) && bracket_depth > 0)
                bracket_depth--;
            i++;
        }
        LLine l = {start, i, orig_col, blank_before, -1};
        ll_push(&out, l);
    }
    return out;
}

/* Mirrors lexer.c's own indent_stack comparison exactly (push on deeper column, pop-to-match on
   shallower) -- comment-only lines don't participate (same as the real lexer skipping blank/
   comment-only lines when computing INDENT/DEDENT), and instead take the depth of the next real
   code line (or the previous one, for a trailing comment run with no code after it). */
static void compute_depths(LLineList* lines, TokList* tl) {
    unsigned int stack[256];
    int sp = 0;
    stack[0] = 0;

    for (unsigned int li = 0; li < lines->count; li++) {
        LLine* l = &lines->items[li];
        bool comment_only = true;
        for (unsigned int k = l->start; k < l->end; k++) {
            if (tl->items[k].type != T_COMMENT) {
                comment_only = false;
                break;
            }
        }
        if (comment_only) continue;
        unsigned int col = l->orig_col;
        while (sp > 0 && col < stack[sp])
            sp--;
        if (col > stack[sp]) {
            sp++;
            stack[sp] = col;
        }
        l->depth = sp;
    }

    int prev_real = 0;
    for (unsigned int li = 0; li < lines->count; li++) {
        if (lines->items[li].depth >= 0)
            prev_real = lines->items[li].depth;
        else
            lines->items[li].depth = prev_real;
    }
    int next_real = prev_real;
    for (int li = (int)lines->count - 1; li >= 0; li--) {
        LLine* l = &lines->items[li];
        bool comment_only = true;
        for (unsigned int k = l->start; k < l->end; k++) {
            if (tl->items[k].type != T_COMMENT) {
                comment_only = false;
                break;
            }
        }
        if (!comment_only)
            next_real = l->depth;
        else
            l->depth = next_real;
    }
}

/* ------------------------------------------------------------------ */
/* Emission                                                             */
/* ------------------------------------------------------------------ */

/* Flow/structural keywords never leave a value behind for '-' to subtract from, so '-' right
   after one of these is unary negate, not binary subtract -- unlike true/false/null (real value
   tokens: "true - 1" is legitimate, if odd, binary subtraction) or a plain identifier/call result. */
static bool is_flow_keyword(const Tok* t) {
    static const char* const kws[] = {"if",      "else",  "for",      "struct", "function",  "return",
                                      "raise",   "break", "continue", "import", "in",        "as",
                                      "integer", "float", "boolean",  "array",  "hashtable", "and",
                                      "or",      "not",   NULL};
    if (t->type != T_WORD) return false;
    for (int i = 0; kws[i]; i++) {
        size_t n = strlen(kws[i]);
        if (t->len == n && !strncmp(t->text, kws[i], n)) return true;
    }
    return false;
}

/* '~' has no binary form in AER at all -- always unary. '-' is the only one that's genuinely
   context-sensitive (binary subtract vs. unary negate). */
static bool is_unary_here(Tok* op, Tok* prev) {
    if (op_is(op, "~")) return true;
    if (!prev) return true;
    if (prev->type == T_OP) return !is_close_bracket(prev);
    if (prev->type == T_WORD) return is_flow_keyword(prev);
    return false;
}

/* A call/declaration's argument list or an index's brackets are always tight against the name
   before them -- true for a preceding identifier/number/string or a closing bracket (chained
   calls/indexing, f()(), arr[0][1]), never for a keyword-like standalone '(' grouping. */
static bool is_call_or_index_open(Tok* prev, Tok* cur) {
    if (!is_open_bracket(cur) || (cur->len == 1 && cur->text[0] == '{')) return false;
    if (!prev) return false;
    return prev->type == T_WORD || prev->type == T_NUMBER || prev->type == T_STRING || is_close_bracket(prev);
}

/* Whitespace between two adjacent non-comment tokens on the same emitted line. */
static bool needs_space(Tok* prev, Tok* cur, bool prev_is_unary) {
    if (!prev) return false;
    if (op_is(cur, ",") || op_is(cur, ":") || op_is(cur, ".") || op_is(cur, "..")) return false;
    if (is_close_bracket(cur)) return false;
    if (is_open_bracket(prev)) return false;
    if (op_is(prev, ".") || op_is(prev, "..")) return false;
    if (prev_is_unary) return false;
    if (is_call_or_index_open(prev, cur)) return false;
    return true;
}

static void emit_comment(FILE* out, Tok* c, bool trailing) {
    if (trailing) fputs("  ", out);
    fwrite(c->text, 1, c->len, out);
}

static void format_file(const char* src, FILE* out) {
    TokList tl = tokenize(src);
    LLineList lines = group_logical_lines(&tl);
    compute_depths(&lines, &tl);

    bool at_file_start = true; /* suppress a leading blank line at file start */
    for (unsigned int li = 0; li < lines.count; li++) {
        LLine* l = &lines.items[li];
        if (l->blank_before > 0 && !at_file_start) fputc('\n', out);
        at_file_start =
            false; /* every line emits real content below -- the line just emitted is never blank */

        for (int k = 0; k < l->depth; k++)
            fputs("    ", out);

        Tok* prev = NULL;
        bool prev_is_unary = false;
        for (unsigned int k = l->start; k < l->end; k++) {
            Tok* t = &tl.items[k];
            if (t->type == T_COMMENT) {
                emit_comment(out, t, prev != NULL);
                continue;
            }
            if (prev && needs_space(prev, t, prev_is_unary)) fputc(' ', out);
            fwrite(t->text, 1, t->len, out);
            prev_is_unary = (t->type == T_OP && (op_is(t, "-") || op_is(t, "~")) && is_unary_here(t, prev));
            prev = t;
        }
        fputc('\n', out);
    }

    free(tl.items);
    free(lines.items);
}

/* ------------------------------------------------------------------ */

static char* read_whole_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "aer-fmt: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = malloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, fp);
    buf[n] = '\0';
    fclose(fp);

    /* Normalize CRLF -> LF up front, same reason source/compiler/lexer.c's own read_file() does
       this -- a stray '\r' would otherwise get captured as part of a comment token's text (the
       comment scanner only stops at '\n'), and outputting it again through a text-mode stdio
       stream (stdout's Windows default) would double it into '\r\r\n'. */
    char* w = buf;
    for (char* r = buf; *r; r++)
        if (*r != '\r') *w++ = *r;
    *w = '\0';
    return buf;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    /* Without this, Windows' text-mode stdout translates every '\n' this program writes into
       '\r\n' -- harmless on its own, but combined with any future change that (correctly) doesn't
       normalize every possible '\r' source could double up. Forcing binary mode makes stdout
       behave identically to the -w (write-to-file, already opened "wb") path: always plain LF,
       matching every other text file in this repo. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    bool write_in_place = false;
    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w"))
            write_in_place = true;
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: aer-fmt [-w] <file.aer>\n");
        return 1;
    }

    char* src = read_whole_file(path);

    if (write_in_place) {
        char tmp_path[4096];
        snprintf(tmp_path, sizeof(tmp_path), "%s.aerfmt-tmp", path);
        FILE* out = fopen(tmp_path, "wb");
        if (!out) {
            fprintf(stderr, "aer-fmt: cannot write '%s'\n", tmp_path);
            return 1;
        }
        format_file(src, out);
        fclose(out);
        remove(path);
        rename(tmp_path, path);
    } else {
        format_file(src, stdout);
    }

    free(src);
    return 0;
}
