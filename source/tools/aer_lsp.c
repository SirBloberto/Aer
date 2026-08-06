/* A language server linking the real compiler rather than reimplementing the grammar, so
   diagnostics are the actual parse errors with zero drift. Go-to-definition and completion are a
   separate coarse token scan -- the single-pass compiler retains no symbol table -- so neither is
   scope-aware. Transport is Content-Length-framed JSON-RPC over stdio, hand-rolled.
   `import` runs the imported file's top-level code at parse time, so every diagnostic pass sets
   aer_set_import_enabled(false) and reports file imports as unchecked instead of executing them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "aer.h"
#include "aer_stdlib.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"

Token token;
Mode mode;

/* ------------------------------------------------------------------ */
/* Minimal JSON -- only what the specific LSP methods below need        */
/* ------------------------------------------------------------------ */

/* Finds "key":"value" or "key":123 for a top-level-ish key anywhere in the text (a plain
   substring/bracket-depth-free scan, not a real parser) -- sufficient for the fixed, predictable
   shapes an LSP client actually sends; not a general JSON reader. */
static const char* json_find_key(const char* json, const char* key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Extracts a JSON string value (unescaping \" \\ \n \t \/ and \uXXXX as best-effort ASCII),
   returns a freshly xmalloc'd NUL-terminated buffer, or NULL. */
static char* json_get_string(const char* json, const char* key) {
    const char* p = json_find_key(json, key);
    if (!p || *p != '"') return NULL;
    p++;
    size_t cap = 256, len = 0;
    char* out = xmalloc(cap);
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'u': {
                    /* Best-effort: only handle the common ASCII-range \u00XX case, else emit '?'. */
                    unsigned int code = 0;
                    if (strlen(p) >= 5) sscanf(p + 1, "%4x", &code);
                    c = (code < 128) ? (char)code : '?';
                    p += 4;
                    break;
                }
                default: c = *p; break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        out[len++] = c;
        p++;
    }
    out[len] = '\0';
    return out;
}

static long json_get_int(const char* json, const char* key, long fallback) {
    const char* p = json_find_key(json, key);
    if (!p) return fallback;
    return strtol(p, NULL, 10);
}

/* ------------------------------------------------------------------ */
/* Content-Length framed stdio                                          */
/* ------------------------------------------------------------------ */

static char* read_message(void) {
    long content_length = -1;
    char header[512];
    for (;;) {
        if (!fgets(header, sizeof(header), stdin)) return NULL;
        if (!strcmp(header, "\r\n") || !strcmp(header, "\n")) break;
        long v;
        if (sscanf(header, "Content-Length: %ld", &v) == 1) content_length = v;
    }
    if (content_length < 0) return NULL;
    char* body = xmalloc((size_t)content_length + 1);
    size_t got = fread(body, 1, (size_t)content_length, stdin);
    body[got] = '\0';
    return body;
}

static void send_message(const char* json_body) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json_body), json_body);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Open-document table -- small, linear, fine for how many files a      */
/* human actually has open at once.                                     */
/* ------------------------------------------------------------------ */

#define MAX_OPEN_DOCS 64
typedef struct {
    char* uri;
    char* text;
} OpenDoc;
static OpenDoc open_docs[MAX_OPEN_DOCS];
static int open_doc_count = 0;

static void set_document(const char* uri, const char* text) {
    for (int i = 0; i < open_doc_count; i++) {
        if (!strcmp(open_docs[i].uri, uri)) {
            free(open_docs[i].text);
            open_docs[i].text = xstrdup(text);
            return;
        }
    }
    if (open_doc_count >= MAX_OPEN_DOCS) return;
    open_docs[open_doc_count].uri = xstrdup(uri);
    open_docs[open_doc_count].text = xstrdup(text);
    open_doc_count++;
}

static const char* get_document(const char* uri) {
    for (int i = 0; i < open_doc_count; i++)
        if (!strcmp(open_docs[i].uri, uri)) return open_docs[i].text;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Diagnostics -- the real compiler, import disabled, never run         */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int line, col;
    char message[512];
} Diag;
static Diag diag_buf[128];
static int diag_count;

static void collect_diag(unsigned int line, unsigned int col, const char* message, void* userdata) {
    (void)userdata;
    if (diag_count >= 128) return;
    Diag* d = &diag_buf[diag_count++];
    d->line = line;
    d->col = col;
    strncpy(d->message, message, sizeof(d->message) - 1);
    d->message[sizeof(d->message) - 1] = '\0';
}

static void run_diagnostics(const char* text) {
    diag_count = 0;
    aer_set_diagnostic_callback(collect_diag, NULL);
    aer_set_import_enabled(
        false); /* see this file's own top comment -- never execute a helper module's code just to lint */

    Chunk chunk;
    VM vm;
    chunk_init(&chunk);
    vm_init(&vm, &chunk);
    mode = MODE_RUN;

    aer_clear_error();
    shell((char*)text);
    lex();
    parse(&chunk);

    chunk_free(&chunk);
    vm_free(&vm);
    aer_set_import_enabled(true);
    aer_set_diagnostic_callback(NULL, NULL);
}

/* Appends s into *buf (growing it as needed) with JSON string-escaping, no surrounding quotes. */
static void buf_append_escaped(char** buf, size_t* cap, size_t* len, const char* s) {
    for (const char* p = s; *p; p++) {
        const char* esc = NULL;
        char single = *p;
        switch (*p) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default: break;
        }
        size_t need = esc ? strlen(esc) : 1;
        if (*len + need + 1 >= *cap) {
            *cap = (*cap + need) * 2;
            *buf = xrealloc(*buf, *cap);
        }
        if (esc) {
            memcpy(*buf + *len, esc, need);
            *len += need;
        } else
            (*buf)[(*len)++] = single;
    }
}

static void publish_diagnostics(const char* uri) {
    /* Built in memory first so the Content-Length header can be exact. */
    char* buf = xmalloc(65536);
    size_t cap = 65536, len = 0;
#define APPEND(...)                                                                                          \
    do {                                                                                                     \
        int n = snprintf(buf + len, cap - len, __VA_ARGS__);                                                 \
        if (n > 0 && (size_t)n >= cap - len) {                                                               \
            cap = (len + (size_t)n) * 2;                                                                     \
            buf = xrealloc(buf, cap);                                                                        \
            n = snprintf(buf + len, cap - len, __VA_ARGS__);                                                 \
        }                                                                                                    \
        if (n > 0) len += (size_t)n;                                                                         \
    } while (0)

    APPEND("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
           "publishDiagnostics\",\"params\":{\"uri\":\"%s\",\"diagnostics\":[",
           uri);
    for (int i = 0; i < diag_count; i++) {
        Diag* d = &diag_buf[i];
        unsigned int line0 = d->line > 0 ? d->line - 1 : 0;
        unsigned int col0 = d->col > 0 ? d->col - 1 : 0;
        if (i > 0) APPEND(",");
        APPEND("{\"range\":{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}},"
               "\"severity\":1,\"source\":\"aer\",\"message\":\"",
               line0, col0, line0, col0 + 1);
        buf_append_escaped(&buf, &cap, &len, d->message);
        APPEND("\"}");
    }
    APPEND("]}}");

    printf("Content-Length: %zu\r\n\r\n%s", len, buf);
    fflush(stdout);
    free(buf);
#undef APPEND
}

/* ------------------------------------------------------------------ */
/* Symbol scan -- coarse, token-based, reuses the real lexer directly   */
/* (comments/whitespace don't matter here, unlike aer_fmt.c)            */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[128];
    unsigned int line;
    bool is_struct;
    bool may_fail;
} Symbol;
static Symbol symbols[512];
static int symbol_count;

/* Named functions can't nest (parser.c), so the next function/struct declaration always marks the
   end of the current one's body -- a raise anywhere between one declaration and the next is
   attributed to it. Coarse and token-based like the rest of this scan, not a real symbol table. */
static void scan_symbols(const char* text) {
    symbol_count = 0;
    shell((char*)text);
    lex();
    TokenType prev_type = TOKEN_END_OF_FILE;
    int current_fn_index = -1;
    while (token.type != TOKEN_END_OF_FILE && symbol_count < 512) {
        if ((prev_type == TOKEN_FUNCTION || prev_type == TOKEN_STRUCT) && token.type == TOKEN_IDENTIFIER) {
            AerString* s = aer_as_string(token.value);
            unsigned int n =
                s->length < sizeof(symbols[0].name) - 1 ? s->length : sizeof(symbols[0].name) - 1;
            memcpy(symbols[symbol_count].name, s->data, n);
            symbols[symbol_count].name[n] = '\0';
            symbols[symbol_count].line = current_source_line();
            symbols[symbol_count].is_struct = (prev_type == TOKEN_STRUCT);
            symbols[symbol_count].may_fail = false;
            current_fn_index = symbols[symbol_count].is_struct ? -1 : symbol_count;
            symbol_count++;
        } else if (token.type == TOKEN_RAISE && current_fn_index >= 0) {
            symbols[current_fn_index].may_fail = true;
        }
        prev_type = token.type;
        lex();
    }
}

/* Mirrors parser.c's own module_call_id/module_fn_id tables (kept in sync by hand -- a small,
   rarely-changing list, not the kind of thing worth a shared header for). */
typedef struct {
    const char* module;
    const char* fns[16];
} ModuleFns;
static const ModuleFns MODULE_FNS[] = {
    {"math",
     {"sqrt", "pow", "floor", "ceil", "abs", "min", "max", "sin", "cos", "log", "log2", "log10", "pi",
      "round", "tan", "exp"}},
    {"random", {"random", "randint", "seed", "choice", "shuffle", NULL}},
    {"string",
     {"upper", "lower", "trim", "contains", "split", "starts_with", "ends_with", "repeat", "replace", "join",
      "index_of", NULL}},
    {"time", {"now", "strftime", "sleep", "parse", NULL}},
    {"json", {"encode", "decode", NULL}},
    {"collection", {"append", "delete", "copy", "insert", "index_of", "keys", "sort", NULL}},
    {"net", {"connect", "send", "recv", "close", NULL}},
    {"regex", {"match", "find", "replace", NULL}},
    {"actor", {"spawn", "send", "receive", "call", NULL}},
    {"scheduler", {"add", "run", NULL}},
    {"io", {"read", "write", "append", "exists", "remove", "stdin", "args", NULL}},
    {NULL, {NULL}}};

/* ------------------------------------------------------------------ */
/* Method handlers                                                      */
/* ------------------------------------------------------------------ */

static void handle_initialize(const char* msg) {
    long id = json_get_int(msg, "id", 0);
    char resp[1024];
    snprintf(resp, sizeof(resp),
             "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"capabilities\":{"
             "\"textDocumentSync\":1,"
             "\"definitionProvider\":true,"
             "\"completionProvider\":{\"triggerCharacters\":[\".\"]}"
             "}}}",
             id);
    send_message(resp);
}

/* didOpen's params.textDocument.text and didChange's params.contentChanges[0].text (full-sync
   mode, TextDocumentSyncKind.Full -- see handle_initialize) are both simply keyed "text", and
   json_get_string does an unstructured find-anywhere scan, so one call handles both shapes. */
static void handle_did_open_or_change(const char* msg) {
    char* uri = json_get_string(msg, "uri");
    char* text = json_get_string(msg, "text");
    if (!uri || !text) {
        free(uri);
        free(text);
        return;
    }

    set_document(uri, text);
    run_diagnostics(text);
    publish_diagnostics(uri);

    free(uri);
    free(text);
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Walks to line `line` (0-based, LSP convention) in text, then expands left/right from column
   `character` over identifier characters to recover the word the cursor is sitting on/in. */
static bool find_word_at(const char* text, long line, long character, char* out, size_t out_size) {
    const char* p = text;
    for (long l = 0; l < line && *p; l++) {
        while (*p && *p != '\n')
            p++;
        if (*p == '\n') p++;
    }
    const char* line_start = p;
    const char* line_end = p;
    while (*line_end && *line_end != '\n')
        line_end++;

    long len = (long)(line_end - line_start);
    long col = character < len ? character : len - 1;
    if (col < 0) return false;
    const char* at = line_start + col;
    if (!is_word_char(*at) && at > line_start) at--;
    if (!is_word_char(*at)) return false;

    const char* start = at;
    while (start > line_start && is_word_char(start[-1]))
        start--;
    const char* end = at;
    while (end < line_end && is_word_char(*end))
        end++;

    size_t n = (size_t)(end - start);
    if (n == 0 || n >= out_size) return false;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static void handle_definition(const char* msg) {
    long id = json_get_int(msg, "id", 0);
    char* uri = json_get_string(msg, "uri");
    const char* text = uri ? get_document(uri) : NULL;
    long line = json_get_int(msg, "line", -1);
    long character = json_get_int(msg, "character", -1);

    char word[128];
    const Symbol* found = NULL;
    if (text && line >= 0 && character >= 0 && find_word_at(text, line, character, word, sizeof(word))) {
        scan_symbols(text);
        for (int i = 0; i < symbol_count; i++) {
            if (!strcmp(symbols[i].name, word)) {
                found = &symbols[i];
                break;
            }
        }
    }

    char resp[512];
    if (found) {
        unsigned int line0 = found->line > 0 ? found->line - 1 : 0;
        snprintf(resp, sizeof(resp),
                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%"
                 "u,\"character\":0},\"end\":{\"line\":%u,\"character\":0}}}}",
                 id, uri ? uri : "", line0, line0);
    } else {
        snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id);
    }
    send_message(resp);
    free(uri);
}

static void handle_completion(const char* msg) {
    long id = json_get_int(msg, "id", 0);
    char* uri = json_get_string(msg, "uri");
    const char* text = uri ? get_document(uri) : NULL;
    if (text) scan_symbols(text);

    char* buf = xmalloc(16384);
    size_t cap = 16384, len = 0;
#define APPEND(...)                                                                                          \
    do {                                                                                                     \
        int n = snprintf(buf + len, cap - len, __VA_ARGS__);                                                 \
        if (n > 0 && (size_t)n >= cap - len) {                                                               \
            cap = (len + (size_t)n) * 2;                                                                     \
            buf = xrealloc(buf, cap);                                                                        \
            n = snprintf(buf + len, cap - len, __VA_ARGS__);                                                 \
        }                                                                                                    \
        if (n > 0) len += (size_t)n;                                                                         \
    } while (0)

    APPEND("{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":[", id);
    bool first = true;
    for (int m = 0; MODULE_FNS[m].module; m++) {
        for (int f = 0; MODULE_FNS[m].fns[f]; f++) {
            if (!first) APPEND(",");
            first = false;
            APPEND("{\"label\":\"%s\",\"kind\":3}", MODULE_FNS[m].fns[f]);
        }
    }
    if (text) {
        for (int i = 0; i < symbol_count; i++) {
            if (!first) APPEND(",");
            first = false;
            if (symbols[i].may_fail)
                APPEND("{\"label\":\"%s\",\"kind\":%d,\"detail\":\"may fail\"}", symbols[i].name,
                       symbols[i].is_struct ? 7 : 3);
            else
                APPEND("{\"label\":\"%s\",\"kind\":%d}", symbols[i].name, symbols[i].is_struct ? 7 : 3);
        }
    }
    APPEND("]}");
    send_message(buf);
#undef APPEND
    free(buf);
    free(uri);
}

/* ------------------------------------------------------------------ */

/* Every diagnostic already reaches the client structured, via aer_set_diagnostic_callback in
   run_diagnostics() -- without registering this too, the plain-text sink's default (stderr) would
   print the same parse error a second time, as raw text, on every keystroke that has one. */
static void discard_plain_error(const char* message, void* userdata) {
    (void)message;
    (void)userdata;
}

int main(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    aer_set_error_callback(discard_plain_error, NULL);
    parser_reset();

    for (;;) {
        char* msg = read_message();
        if (!msg) break;

        char* method = json_get_string(msg, "method");
        if (!method) {
            free(msg);
            continue;
        }

        if (!strcmp(method, "initialize")) {
            handle_initialize(msg);
        } else if (!strcmp(method, "textDocument/didOpen")) {
            handle_did_open_or_change(msg);
        } else if (!strcmp(method, "textDocument/didChange")) {
            handle_did_open_or_change(msg);
        } else if (!strcmp(method, "textDocument/definition")) {
            handle_definition(msg);
        } else if (!strcmp(method, "textDocument/completion")) {
            handle_completion(msg);
        } else if (!strcmp(method, "shutdown")) {
            long id = json_get_int(msg, "id", 0);
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id);
            send_message(resp);
        } else if (!strcmp(method, "exit")) {
            free(method);
            free(msg);
            return 0;
        }
        /* Unrecognized methods/notifications are silently ignored -- correct per the LSP spec for
           anything not in this server's declared capabilities. */

        free(method);
        free(msg);
    }
    return 0;
}
