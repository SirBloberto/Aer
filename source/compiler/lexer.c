#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "error.h"
#include "lexer.h"

typedef struct File {
    char* name;
    char* start;   /* immutable pointer to beginning of buffer */
    char* buffer;  /* advances as we lex */
    /* Absolute source line where this File's buffer begins -- 0 for a real file or REPL line
       (current_source_line's own newline-count already gives the right absolute number there). A
       span (lexer_begin_span) starts its OWN buffer at line 1 relative to itself, so this is the
       offset needed to recover the TRUE absolute line in the original file the span was cut from. */
    unsigned int line_base;
} File;

/* Array of File* (not File values) — nested imports save a raw File* across their own lex/parse/run cycle via lexer_save_state, so growing this array must never move an already-issued File's address. */
static File** files_storage  = NULL;
static int    file_capacity  = 0;
static int    file_index     = 0;
static File*  current;

/* Indentation state — reset before each parse */
static unsigned int indent_stack[64];
static int          indent_depth;
static int          pending_dedents;
static bool         at_line_start;

/* Nesting depth of unclosed (/[/{ — while > 0, newlines are whitespace instead of statement terminators, letting calls/literals span multiple lines. */
static int bracket_depth;

static void indent_reset() {
    indent_stack[0] = 0;
    indent_depth    = 1;
    pending_dedents = 0;
    bracket_depth   = 0;
    at_line_start   = true;
}

/* For error.c to print source context */
const char* current_source_start()  { return current->start; }
const char* current_source_cursor() { return current->buffer; }
const char* current_source_name()   { return current->name; }

/* Narrow escape hatch for reporting a deferred error at a saved position, not a general seek. */
void lexer_set_cursor(const char* pos) { current->buffer = (char*)pos; }

/* For parser.c to tag bytecode with its source line (Chunk.line_mark_offsets) — same scan as error_at(), just returning the number. */
unsigned int current_source_line() {
    unsigned int line = 1;
    for (const char* p = current->start; p < current->buffer; p++)
        if (*p == '\n') line++;
    return current->line_base + line;
}

struct LexerState {
    File*        file;
    unsigned int indent_stack[64];
    int          indent_depth;
    int          pending_dedents;
    bool         at_line_start;
    int          bracket_depth;
};

LexerState* lexer_save_state(void) {
    LexerState* s = xmalloc(sizeof(LexerState));
    s->file = current;
    memcpy(s->indent_stack, indent_stack, sizeof(indent_stack));
    s->indent_depth    = indent_depth;
    s->pending_dedents = pending_dedents;
    s->at_line_start   = at_line_start;
    s->bracket_depth   = bracket_depth;
    return s;
}

void lexer_restore_state(LexerState* s) {
    current = s->file;
    memcpy(indent_stack, s->indent_stack, sizeof(indent_stack));
    indent_depth    = s->indent_depth;
    pending_dedents = s->pending_dedents;
    bracket_depth   = s->bracket_depth;
    at_line_start    = s->at_line_start;
    free(s);
}

/* ------------------------------------------------------------------ */
/* File loading                                                         */
/* ------------------------------------------------------------------ */

void read_file(char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) { error("Cannot open file: %s", filename); return; }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0) { fclose(fp); error("Cannot determine size of file: %s", filename); return; }
    fseek(fp, 0, SEEK_SET);
    unsigned long length = (unsigned long)size;

    char* buf = malloc(length + 1);
    if (!buf) { fclose(fp); error("Out of memory reading file: %s", filename); return; }
    unsigned long nread = (unsigned long)fread(buf, 1, length, fp);
    fclose(fp);
    buf[nread] = '\0';

    /* An embedded NUL would make every '\0'-terminated scan below (indentation, the \r-strip
       pass, lex_string, ...) treat it as the end of the file, silently truncating everything
       after it instead of reporting an error -- found via tests/fuzz.py's byte-flip mutator
       turning a mid-file space into a NUL, which truncated a loop body and made it infinite. */
    if (strlen(buf) != nread) {
        unsigned long at = (unsigned long)strlen(buf);
        free(buf);
        error("Source file '%s' contains an embedded NUL byte at offset %lu -- not a valid AER source file", filename, at);
        return;
    }

    /* Normalize Windows line endings: strip \r in-place */
    char* dst = buf; char* src = buf;
    while (*src) { if (*src != '\r') *dst++ = *src; src++; }
    *dst = '\0';

    if (file_index >= file_capacity) {
        file_capacity = file_capacity ? file_capacity * 2 : 8;
        files_storage = xrealloc(files_storage, sizeof(File*) * file_capacity);
    }
    File* file    = xmalloc(sizeof(File));
    file->name    = filename;
    file->start   = buf;
    file->buffer  = buf;
    file->line_base = 0;
    files_storage[file_index++] = file;
    current = file;
    indent_reset();
}

void shell(char* line) {
    /* Use a static slot so we can free the previous line on the next call */
    static File shell_file;
    static char* previous = NULL;
    free(previous);

    previous = strdup(line);
    if (!previous) error("Out of memory in shell");
    /* Strip \r from pasted Windows-style input */
    char* r = previous; char* w = previous;
    while (*r) { if (*r != '\r') *w++ = *r; r++; }
    *w = '\0';
    shell_file.name   = "shell";
    shell_file.start  = previous;
    shell_file.buffer = previous;
    current = &shell_file;
    indent_reset();
}

/* Fresh, independent text span (string interpolation's `{expr}` body, or a shape-specialization
   recompile's retained function source). Caller must bracket with lexer_save_state()/
   lexer_restore_state(). Uses its own heap File, freed on restore.

   start_line is the ABSOLUTE line (in whatever file the span was cut from) that the span's own
   first character sits on -- stored as line_base = start_line - 1 so current_source_line()'s
   existing "starts counting at 1" formula lands on start_line at span position 0, instead of
   reporting 1 (relative to the span's own start) the way it did before this parameter existed.
   That was a real bug: a specialization recompile's error/debug line numbers were off by however
   many lines precede the function in its source file. */
void lexer_begin_span(const char* text, unsigned int len, unsigned int start_line) {
    char* buf = xmalloc((size_t)len + 1);
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (file_index >= file_capacity) {
        file_capacity = file_capacity ? file_capacity * 2 : 8;
        files_storage = xrealloc(files_storage, sizeof(File*) * file_capacity);
    }
    File* file    = xmalloc(sizeof(File));
    file->name    = "<interpolation>";
    file->start   = buf;
    file->buffer  = buf;
    file->line_base = start_line - 1;
    files_storage[file_index++] = file;
    current = file;
    indent_reset();
    /* A span is one inline expression, never a block — at_line_start's default true would misread
       leading whitespace as an indent level and emit a spurious TOKEN_INDENT. */
    at_line_start = false;
}

/* ------------------------------------------------------------------ */
/* Token helpers                                                        */
/* ------------------------------------------------------------------ */

static void emit(TokenType type, unsigned int length) {
    token.type    = type;
    current->buffer += length;
}

static void emit_integer(int64_t integer, unsigned int length) {
    token.value = aer_int(integer);
    token.narrow = false;
    emit(TOKEN_INTEGER, length);
}

static void emit_real(double real, unsigned int length) {
    token.value = aer_real(real);
    token.narrow = false;
    emit(TOKEN_REAL, length);
}

static void emit_string_token(TokenType type, unsigned int length) {
    /* Copies into owned memory (AerString always owns its data) — shell() (REPL mode) frees the previous line's buffer on every call, which would otherwise dangle earlier tokens. */
    char* buf = xmalloc(length + 1);
    memcpy(buf, current->buffer, length);
    buf[length] = '\0';
    token.value = aer_make_string(buf, length);
    emit(type, length);
}

static void emit_boolean(TokenType type) {
    token.value = aer_bool(type == TOKEN_TRUE);
    emit(type, type == TOKEN_TRUE ? sizeof("true") - 1 : sizeof("false") - 1);
}

/* ------------------------------------------------------------------ */
/* Lexing helpers                                                       */
/* ------------------------------------------------------------------ */

static void skip_whitespace() {
    while (*current->buffer == ' ' || *current->buffer == '\t')
        current->buffer++;
}

static void skip_comment() {
    while (*current->buffer != '\n' && *current->buffer != '\0')
        current->buffer++;
}

static void skip_whitespace_and_comments() {
    while (true) {
        skip_whitespace();
        /* Inside an unclosed bracket a newline is just whitespace — swallow before the indent scan. */
        if (*current->buffer == '\n' && bracket_depth > 0) { current->buffer++; continue; }
        if (*current->buffer != '#') return;
        skip_comment();
    }
}

static void lex_number() {
    char* buf = current->buffer;

    /* Hexadecimal literal: 0x... */
    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        if (!isxdigit((unsigned char)buf[2])) {
            error_at("Expected hex digits after '0x'");
            /* Consume "0x" ourselves — whether strtoll backs off to just "0" here is libc-defined, not something to rely on for forward progress. */
            emit(TOKEN_ERROR, 2);
            return;
        }
        char* end; errno = 0;
        int64_t val = strtoll(buf, &end, 16);
        if (errno == ERANGE) error_at("Integer literal overflow");
        if (isalpha((unsigned char)*end) || *end == '_')
            error_at("Invalid character after integer literal");
        emit_integer(val, (unsigned int)(end - buf));
        return;
    }

    /* lex() only calls lex_number() on a digit, so int_len is always >= 1 — safe to fall through from error_at() below without an explicit return. */
    char* end; errno = 0;
    int64_t int_val = strtoll(buf, &end, 10);
    if (errno == ERANGE) error_at("Integer literal overflow");
    unsigned int int_len = (unsigned int)(end - buf);

    /* Not a float, or range operator (..) follows */
    if (*end != '.' || end[1] == '.') {
        /* `i` immediately after an integer literal (`42i`), not followed by another identifier
           character (so `42if` etc. still falls through to the ordinary error below) -- selects
           narrow (int32) storage in a repeat-literal array or struct field default. Parse-time-only:
           the emitted AerVal is an ordinary aer_int either way, `narrow` is a sibling marker. */
        if (*end == 'i' && !(isalnum((unsigned char)end[1]) || end[1] == '_')) {
            emit_integer(int_val, int_len + 1);
            token.narrow = true;
            return;
        }
        if (isalpha((unsigned char)*end) || *end == '_')
            error_at("Invalid character after integer literal");
        emit_integer(int_val, int_len);
        return;
    }

    /* Float: use strtod from original start for correct precision */
    double real_val = strtod(buf, &end);
    unsigned int len = (unsigned int)(end - buf);
    if (len <= int_len + 1) error_at("Expected digit after '.'");
    /* `f` immediately after a float literal (`0.0f`) -- same narrow-marker convention as `i` above,
       selecting float32 storage instead of int32. */
    if (*end == 'f' && !(isalnum((unsigned char)end[1]) || end[1] == '_')) {
        emit_real(real_val, len + 1);
        token.narrow = true;
        return;
    }
    if (isalpha((unsigned char)*end) || *end == '_')
        error_at("Invalid character after float literal");
    emit_real(real_val, len);
}

static bool lex_keyword(unsigned int length) {
    char* b = current->buffer;

    /* Booleans handled inline — not in the keyword table */
    if (length == sizeof("true")  - 1 && strncmp(b, "true",  sizeof("true")  - 1) == 0) { emit_boolean(TOKEN_TRUE);  return true; }
    if (length == sizeof("false") - 1 && strncmp(b, "false", sizeof("false") - 1) == 0) { emit_boolean(TOKEN_FALSE); return true; }

    static const struct { const char* word; unsigned int len; TokenType type; } keywords[] = {
        { "if",       sizeof("if")       - 1, TOKEN_IF       },
        { "else",     sizeof("else")     - 1, TOKEN_ELSE     },
        { "for",      sizeof("for")      - 1, TOKEN_FOR      },
        { "in",       sizeof("in")       - 1, TOKEN_IN       },
        { "struct",   sizeof("struct")   - 1, TOKEN_STRUCT   },
        { "function", sizeof("function") - 1, TOKEN_FUNCTION },
        { "return",   sizeof("return")   - 1, TOKEN_RETURN   },
        { "raise",    sizeof("raise")    - 1, TOKEN_RAISE    },
        { "break",    sizeof("break")    - 1, TOKEN_BREAK    },
        { "continue", sizeof("continue") - 1, TOKEN_CONTINUE },
        { "null",     sizeof("null")     - 1, TOKEN_NULL     },
        { "import",   sizeof("import")   - 1, TOKEN_IMPORT   },
        { "integer",   sizeof("integer")   - 1, TOKEN_TYPE_INTEGER   },
        { "float",     sizeof("float")     - 1, TOKEN_TYPE_FLOAT     },
        { "boolean",   sizeof("boolean")   - 1, TOKEN_TYPE_BOOLEAN   },
        { "array",     sizeof("array")     - 1, TOKEN_TYPE_ARRAY     },
        { "hashtable", sizeof("hashtable") - 1, TOKEN_TYPE_HASHTABLE },
        { "and",       sizeof("and")       - 1, TOKEN_AND            },
        { "or",        sizeof("or")        - 1, TOKEN_OR             },
        { "not",       sizeof("not")       - 1, TOKEN_NOT            },
    };
    static const int keyword_count = sizeof(keywords) / sizeof(*keywords);

    for (int i = 0; i < keyword_count; i++) {
        if (keywords[i].len == length && strncmp(b, keywords[i].word, length) == 0) {
            emit(keywords[i].type, length);
            return true;
        }
    }
    return false;
}

static void lex_string() {
    current->buffer++; /* skip opening " */
    char* start = current->buffer;
    /* Brace-depth aware so a '"' inside an active {expr} (e.g. "{result["total"]}") isn't
       mistaken for the string's own closing quote -- parse_string_literal's later interpolation
       scan already tracks this same depth; this just makes the token-boundary scan agree with it. */
    int brace_depth = 0;
    while (*current->buffer != '\0' && *current->buffer != '\n' &&
           !(*current->buffer == '"' && brace_depth == 0)) {
        if (*current->buffer == '\\' && *(current->buffer + 1) != '\0') {
            current->buffer += 2;  /* skip escaped char (e.g. \" \{) so it's never treated specially */
            continue;
        }
        if (*current->buffer == '{') brace_depth++;
        else if (*current->buffer == '}' && brace_depth > 0) brace_depth--;
        current->buffer++;
    }
    if (*current->buffer != '"') { error_at("Unterminated string"); return; }
    unsigned int length = (unsigned int)(current->buffer - start);
    /* Copies — see emit_string_token's comment on why AerString always owns its data. */
    char* buf = xmalloc(length + 1);
    memcpy(buf, start, length);
    buf[length] = '\0';
    token.value = aer_make_string(buf, length);
    token.type = TOKEN_STRING;
    current->buffer++; /* skip closing " */
}

/* """triple-quoted""" strings: unlike lex_string(), raw newlines are allowed and only a run of three quotes ends it; escapes/interpolation are handled later, same as lex_string(). */
static void lex_multiline_string() {
    current->buffer += 3; /* skip opening """ */
    char* start = current->buffer;
    while (*current->buffer != '\0' &&
           !(current->buffer[0] == '"' && current->buffer[1] == '"' && current->buffer[2] == '"'))
        current->buffer++;
    if (*current->buffer != '"') { error_at("Unterminated multi-line string"); return; }
    unsigned int length = (unsigned int)(current->buffer - start);
    /* Copies — see emit_string_token's comment on why AerString always owns its data. */
    char* buf = xmalloc(length + 1);
    memcpy(buf, start, length);
    buf[length] = '\0';
    token.value = aer_make_string(buf, length);
    token.type = TOKEN_STRING;
    current->buffer += 3; /* skip closing """ */
}

static void lex_identifier() {
    char* buf = current->buffer;
    unsigned int length = 0;
    while (isalpha((unsigned char)*buf) || isdigit((unsigned char)*buf) || *buf == '_') {
        buf++;
        length++;
    }
    if (!lex_keyword(length))
        emit_string_token(TOKEN_IDENTIFIER, length);
}

/* ------------------------------------------------------------------ */
/* Main lex function                                                    */
/* ------------------------------------------------------------------ */

void lex() {
    /* Emit queued DEDENTs before anything else */
    if (pending_dedents > 0) {
        pending_dedents--;
        token.type = TOKEN_DEDENT;
        return;
    }

    /* At the start of a line: measure indentation */
    if (at_line_start) {
        at_line_start = false;

        /* Skip blank and comment-only lines */
        for (;;) {
            char* p = current->buffer;
            while (*p == ' ') p++;
            if (*p == '#') while (*p != '\n' && *p != '\0') p++;
            if (*p != '\n') break;
            current->buffer = p + 1;
        }

        /* Count leading spaces; error on tabs */
        char* p = current->buffer;
        unsigned int spaces = 0;
        while (*p == ' ') { spaces++; p++; }
        if (*p == '\t') { error_at("Tabs not allowed for indentation"); return; }

        unsigned int top = indent_stack[indent_depth - 1];

        if (*p != '\0' && spaces > top) {
            if (indent_depth >= 64) { error("Indentation too deep"); return; }
            indent_stack[indent_depth++] = spaces;
            current->buffer = p;
            token.type = TOKEN_INDENT;
            return;
        } else if (spaces < top || (*p == '\0' && indent_depth > 1)) {
            current->buffer = p;
            int pops = 0;
            while (indent_depth > 1 && indent_stack[indent_depth - 1] > spaces) {
                indent_depth--;
                pops++;
            }
            if (*p != '\0' && indent_depth > 1 && indent_stack[indent_depth - 1] != spaces) {
                error_at("Indentation does not match any outer level");
                return;
            }
            if (*p == '\0') { indent_depth = 1; }
            pending_dedents = pops - 1;
            token.type = TOKEN_DEDENT;
            return;
        } else {
            current->buffer = p;  /* same level: skip leading spaces */
        }
    }

    skip_whitespace_and_comments();

    char* b = current->buffer;

    if (isdigit((unsigned char)*b))  { lex_number();     return; }
    if (isalpha((unsigned char)*b) || *b == '_') { lex_identifier(); return; }
    if (*b == '"') {
        if (b[1] == '"' && b[2] == '"') { lex_multiline_string(); return; }
        lex_string();
        return;
    }

    switch (*b) {
        case ',':  emit(TOKEN_COMMA,              1); return;
        case ';':  emit(TOKEN_SEMICOLON,          1); return;
        case '\n': at_line_start = true; emit(TOKEN_NEW_LINE, 1); return;
        case '\0':
            /* No trailing newline — flush remaining indent levels */
            if (indent_depth > 1) {
                indent_depth--;
                pending_dedents = indent_depth - 1;
                indent_depth = 1;
                token.type = TOKEN_DEDENT;
                return;
            }
            emit(TOKEN_END_OF_FILE, 0); return;
        case '(':  bracket_depth++; emit(TOKEN_OPEN_PARENTHESE,    1); return;
        case ')':  if (bracket_depth > 0) bracket_depth--; emit(TOKEN_CLOSE_PARENTHESE, 1); return;
        case '[':  bracket_depth++; emit(TOKEN_OPEN_BRACKET,       1); return;
        case ']':  if (bracket_depth > 0) bracket_depth--; emit(TOKEN_CLOSE_BRACKET,    1); return;
        case '{':  bracket_depth++; emit(TOKEN_OPEN_BRACE,         1); return;
        case '}':  if (bracket_depth > 0) bracket_depth--; emit(TOKEN_CLOSE_BRACE,      1); return;
        case ':':  emit(TOKEN_COLON,              1); return;
        case '.':  if (b[1]=='.') { emit(TOKEN_DOT_DOT, 2); return; }
                   emit(TOKEN_DOT, 1); return;
        case '~':  emit(TOKEN_BITWISE_NOT,        1); return;
        case '*':  if (b[1]=='=') { emit(TOKEN_MULTIPLY_ASSIGN, 2); return; }
                   emit(TOKEN_MULTIPLY, 1); return;
        case '/':  if (b[1]=='/') {
                       if (b[2]=='=') { emit(TOKEN_FLOOR_DIVIDE_ASSIGN, 3); return; }
                       emit(TOKEN_FLOOR_DIVIDE, 2); return;
                   }
                   if (b[1]=='=') { emit(TOKEN_DIVIDE_ASSIGN, 2); return; }
                   emit(TOKEN_DIVIDE, 1); return;
        case '%':  if (b[1]=='=') { emit(TOKEN_MODULO_ASSIGN,   2); return; }
                   emit(TOKEN_MODULO,   1); return;
        case '+':  if (b[1]=='=') { emit(TOKEN_ADD_ASSIGN,      2); return; }
                   emit(TOKEN_ADD,      1); return;
        case '-':  if (b[1]=='=') { emit(TOKEN_SUBTRACT_ASSIGN, 2); return; }
                   emit(TOKEN_SUBTRACT, 1); return;
        case '!':  if (b[1]=='=') { emit(TOKEN_NOT_EQUAL,       2); return; }
                   error_at("'!' is not an operator -- use 'not'"); emit(TOKEN_ERROR, 1); return;
        case '=':  if (b[1]=='=') { emit(TOKEN_EQUAL,           2); return; }
                   emit(TOKEN_ASSIGN,   1); return;
        case '<':  if (b[1]=='<') { emit(TOKEN_LEFT_SHIFT, 2); return; }
                   if (b[1]=='=') { emit(TOKEN_LESS_EQUAL, 2); return; }
                   emit(TOKEN_LESS, 1); return;
        case '>':  if (b[1]=='>') { emit(TOKEN_RIGHT_SHIFT, 2); return; }
                   if (b[1]=='=') { emit(TOKEN_GREATER_EQUAL, 2); return; }
                   emit(TOKEN_GREATER, 1); return;
        case '&':  if (b[1]=='&') { error_at("'&&' is not an operator -- use 'and'"); emit(TOKEN_ERROR, 2); return; }
                   emit(TOKEN_BITWISE_AND, 1); return;
        case '|':  if (b[1]=='|') { error_at("'||' is not an operator -- use 'or'"); emit(TOKEN_ERROR, 2); return; }
                   if (b[1]=='>') { emit(TOKEN_PIPE,        2); return; }
                   emit(TOKEN_BITWISE_OR, 1); return;
        case '^':  emit(TOKEN_BITWISE_XOR, 1); return;
    }

    /* Unrecognized byte: error_at() doesn't exit in MODE_SHELL, so it must still be consumed here or parser.c's error-recovery loop spins forever re-lexing it. */
    error_at("Unknown character: '%c'", *b);
    emit(TOKEN_ERROR, 1);
}

/* Token matching utilities */
/* ------------------------------------------------------------------ */

bool equal(TokenType match) {
    return token.type == match;
}

void require(TokenType match, const char* msg) {
    if (!equal(match))
        error_at("%s", msg);
    lex();
}

bool consume(TokenType match) {
    if (!equal(match))
        return false;
    lex();
    return true;
}
