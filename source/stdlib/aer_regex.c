#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

/* A small backtracking engine for the practical common subset: literals, '.', character classes,
   '*'/'+'/'?' on atoms and groups, '^'/'$', '|', and non-capturing '(...)'. No backreferences,
   named groups, or lazy quantifiers. In-house rather than POSIX <regex.h>, which isn't reliably
   available on the MinGW target, matching this project's own PRNG and GC precedent. */

typedef enum { NODE_CHAR, NODE_ANY, NODE_CLASS, NODE_START, NODE_END, NODE_GROUP } NodeType;

typedef struct Group Group;

typedef struct {
    NodeType type;
    char ch; /* NODE_CHAR */
    unsigned char class_bits[32]; /* NODE_CLASS -- 256-bit set, negation already folded in at build time */
    Group* group; /* NODE_GROUP */
    char quant; /* 0, '*', '+', '?' */
} Node;

typedef struct {
    Node* nodes;
    int count;
    int cap;
} Seq;
struct Group {
    Seq* alts;
    int alt_count;
    int alt_cap;
};

/* ------------------------------------------------------------------ */
/* Parsing -- pattern -> a tree of Groups/Seqs/Nodes                    */
/* ------------------------------------------------------------------ */

static const char* p; /* parser cursor -- not reentrant, fine for this single-threaded VM */
static bool parse_failed;

static void class_set_range(unsigned char* bits, unsigned char lo, unsigned char hi) {
    for (int c = lo; c <= hi; c++)
        bits[c / 8] |= (unsigned char)(1 << (c % 8));
}
static bool class_test(const unsigned char* bits, unsigned char c) {
    return (bits[c / 8] & (1 << (c % 8))) != 0;
}

static void class_shorthand(unsigned char* bits, char letter) {
    switch (letter) {
        case 'd': class_set_range(bits, '0', '9'); break;
        case 'D':
            for (int c = 0; c < 256; c++)
                if (!(c >= '0' && c <= '9')) class_set_range(bits, (unsigned char)c, (unsigned char)c);
            break;
        case 'w':
            class_set_range(bits, 'a', 'z');
            class_set_range(bits, 'A', 'Z');
            class_set_range(bits, '0', '9');
            bits['_' / 8] |= (unsigned char)(1 << ('_' % 8));
            break;
        case 'W':
            for (int c = 0; c < 256; c++) {
                bool word =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                if (!word) class_set_range(bits, (unsigned char)c, (unsigned char)c);
            }
            break;
        case 's': {
            const char* ws = " \t\n\r\f\v";
            for (const char* w = ws; *w; w++)
                class_set_range(bits, (unsigned char)*w, (unsigned char)*w);
            break;
        }
        case 'S': {
            const char* ws = " \t\n\r\f\v";
            bool is_ws[256] = {0};
            for (const char* w = ws; *w; w++)
                is_ws[(unsigned char)*w] = true;
            for (int c = 0; c < 256; c++)
                if (!is_ws[c]) class_set_range(bits, (unsigned char)c, (unsigned char)c);
            break;
        }
        default: break;
    }
}

static Group* parse_alt(void);

/* Parses one '[...]' bracket expression, p already past the '['. */
static void parse_class(unsigned char* bits) {
    memset(bits, 0, 32);
    bool negate = false;
    if (*p == '^') {
        negate = true;
        p++;
    }
    bool any = false;
    while (*p && *p != ']') {
        if (*p == '\\' && p[1]) {
            p++;
            if (strchr("dDwWsS", *p)) {
                class_shorthand(bits, *p);
                p++;
                any = true;
                continue;
            }
            unsigned char lo = (unsigned char)*p++;
            if (*p == '-' && p[1] != ']' && p[1] != '\0') {
                p++;
                unsigned char hi = (unsigned char)*p++;
                class_set_range(bits, lo, hi);
            } else {
                class_set_range(bits, lo, lo);
            }
            any = true;
            continue;
        }
        unsigned char lo = (unsigned char)*p++;
        if (*p == '-' && p[1] != ']' && p[1] != '\0') {
            p++;
            unsigned char hi = (unsigned char)*p++;
            class_set_range(bits, lo, hi);
        } else {
            class_set_range(bits, lo, lo);
        }
        any = true;
    }
    if (*p != ']') {
        parse_failed = true;
        return;
    }
    p++;
    if (!any) parse_failed = true;
    if (negate)
        for (int i = 0; i < 32; i++)
            bits[i] = (unsigned char)~bits[i];
}

static void seq_push(Seq* seq, Node node) {
    if (seq->count >= seq->cap) {
        seq->cap = seq->cap ? seq->cap * 2 : 4;
        seq->nodes = xrealloc(seq->nodes, sizeof(Node) * (size_t)seq->cap);
    }
    seq->nodes[seq->count++] = node;
}

static void group_push(Group* g, Seq seq) {
    if (g->alt_count >= g->alt_cap) {
        g->alt_cap = g->alt_cap ? g->alt_cap * 2 : 2;
        g->alts = xrealloc(g->alts, sizeof(Seq) * (size_t)g->alt_cap);
    }
    g->alts[g->alt_count++] = seq;
}

static bool parse_atom(Node* out) {
    memset(out, 0, sizeof(*out));
    if (*p == '(') {
        p++;
        Group* g = parse_alt();
        if (*p != ')') {
            parse_failed = true;
            return false;
        }
        p++;
        out->type = NODE_GROUP;
        out->group = g;
        return true;
    }
    if (*p == '.') {
        p++;
        out->type = NODE_ANY;
        return true;
    }
    if (*p == '^') {
        p++;
        out->type = NODE_START;
        return true;
    }
    if (*p == '$') {
        p++;
        out->type = NODE_END;
        return true;
    }
    if (*p == '[') {
        p++;
        out->type = NODE_CLASS;
        parse_class(out->class_bits);
        return !parse_failed;
    }
    if (*p == '\\' && p[1]) {
        p++;
        if (strchr("dDwWsS", *p)) {
            out->type = NODE_CLASS;
            memset(out->class_bits, 0, 32);
            class_shorthand(out->class_bits, *p);
            p++;
            return true;
        }
        out->type = NODE_CHAR;
        out->ch = *p++;
        return true;
    }
    if (*p == '\0' || *p == '|' || *p == ')') {
        parse_failed = true;
        return false;
    }
    out->type = NODE_CHAR;
    out->ch = *p++;
    return true;
}

/* One '|'-separated alternative: (Atom Quant?)* */
static Seq parse_seq(void) {
    Seq seq = {0};
    while (*p && *p != '|' && *p != ')' && !parse_failed) {
        Node node;
        if (!parse_atom(&node)) break;
        if (*p == '*' || *p == '+' || *p == '?') {
            node.quant = *p;
            p++;
        }
        seq_push(&seq, node);
    }
    return seq;
}

/* Seq ('|' Seq)* -- the whole pattern, or the inside of one '(...)'. */
static Group* parse_alt(void) {
    Group* g = xmalloc(sizeof(Group));
    memset(g, 0, sizeof(*g));
    group_push(g, parse_seq());
    while (*p == '|' && !parse_failed) {
        p++;
        group_push(g, parse_seq());
    }
    return g;
}

static Group* regex_compile(const char* pattern) {
    p = pattern;
    parse_failed = false;
    Group* g = parse_alt();
    if (parse_failed || *p != '\0') return NULL;
    return g;
}

static void free_seq(Seq* seq);
static void free_group(Group* g) {
    if (!g) return;
    for (int i = 0; i < g->alt_count; i++)
        free_seq(&g->alts[i]);
    free(g->alts);
    free(g);
}
static void free_seq(Seq* seq) {
    for (int i = 0; i < seq->count; i++)
        if (seq->nodes[i].type == NODE_GROUP) free_group(seq->nodes[i].group);
    free(seq->nodes);
}

/* ------------------------------------------------------------------ */
/* Matching -- continuation-passing backtracker                        */
/* ------------------------------------------------------------------ */

/* Every match function threads an `end` out-parameter through: on success it's set to the text
   position where the whole chain finally bottomed out (frame == NULL), which is the match's real
   end -- the engine is otherwise a pure yes/no backtracker with no notion of "how much matched". */

typedef enum { FRAME_SEQ, FRAME_GROUP_REPEAT } FrameKind;
typedef struct Frame {
    FrameKind kind;
    Seq* seq; /* FRAME_SEQ */
    int idx; /* FRAME_SEQ */
    Group* group; /* FRAME_GROUP_REPEAT */
    const char* prev_text; /* FRAME_GROUP_REPEAT -- zero-width-loop guard */
    struct Frame* parent;
} Frame;

static const char* subject_start; /* for '^' */

/* Nested unbounded quantifiers ('(a*)*b' against a long non-matching run of 'a's, the classic
   catastrophic-backtracking shape) can blow up exponentially in ANY naive backtracker, this one
   included -- rolling an NFA/DFA engine to avoid that entirely is a much bigger undertaking than
   "compact". A step budget instead bounds worst-case time: past it, deeper attempts just report
   failure early rather than churning for an unbounded amount of time. */
#define REGEX_STEP_BUDGET 2000000
static long steps_left;

static bool match_frame(Frame* frame, const char* text, const char** end);

static bool node_matches_char(Node* node, char c) {
    if (c == '\0') return false;
    switch (node->type) {
        case NODE_CHAR: return c == node->ch;
        case NODE_ANY: return c != '\n';
        case NODE_CLASS: return class_test(node->class_bits, (unsigned char)c);
        default: return false;
    }
}

/* Greedy repetition of a single-char atom: consume as many as possible, then backtrack fewer
   until `after` succeeds. */
static bool match_atom_star(Node* node, Frame* after, const char* text, const char** end) {
    if (node_matches_char(node, *text) && match_atom_star(node, after, text + 1, end)) return true;
    return match_frame(after, text, end);
}

static bool match_group_once(Group* g, Frame* after, const char* text, const char** end) {
    for (int i = 0; i < g->alt_count; i++) {
        Frame alt_frame = {FRAME_SEQ, &g->alts[i], 0, NULL, NULL, after};
        if (match_frame(&alt_frame, text, end)) return true;
    }
    return false;
}

static bool match_frame(Frame* frame, const char* text, const char** end) {
    if (--steps_left <= 0) return false;
    if (!frame) {
        *end = text;
        return true;
    }

    if (frame->kind == FRAME_GROUP_REPEAT) {
        if (text == frame->prev_text) return match_frame(frame->parent, text, end); /* zero-width guard */
        Frame repeat_again = {FRAME_GROUP_REPEAT, NULL, 0, frame->group, text, frame->parent};
        if (match_group_once(frame->group, &repeat_again, text, end)) return true;
        return match_frame(frame->parent, text, end);
    }

    if (frame->idx >= frame->seq->count) return match_frame(frame->parent, text, end);
    Node* node = &frame->seq->nodes[frame->idx];
    Frame next = {FRAME_SEQ, frame->seq, frame->idx + 1, NULL, NULL, frame->parent};

    if (node->type == NODE_START) return (text == subject_start) && match_frame(&next, text, end);
    if (node->type == NODE_END) return (*text == '\0') && match_frame(&next, text, end);

    if (node->type == NODE_GROUP) {
        if (node->quant == '\0') return match_group_once(node->group, &next, text, end);
        if (node->quant == '?')
            return match_group_once(node->group, &next, text, end) || match_frame(&next, text, end);
        /* '*' or '+' */
        Frame repeat = {FRAME_GROUP_REPEAT, NULL, 0, node->group, text, &next};
        bool matched_once = match_group_once(node->group, &repeat, text, end);
        if (node->quant == '+') return matched_once;
        return matched_once || match_frame(&next, text, end);
    }

    /* single-char atom: CHAR/ANY/CLASS */
    if (node->quant == '\0') return node_matches_char(node, *text) && match_frame(&next, text + 1, end);
    if (node->quant == '?')
        return (node_matches_char(node, *text) && match_frame(&next, text + 1, end)) ||
               match_frame(&next, text, end);
    if (node->quant == '+')
        return node_matches_char(node, *text) && match_atom_star(node, &next, text + 1, end);
    return match_atom_star(node, &next, text, end); /* '*' */
}

/* Tries the whole compiled pattern anchored exactly at `text`; on success *end is the match's end. */
static bool regex_match_at(Group* g, const char* text, const char** end) {
    steps_left = REGEX_STEP_BUDGET;
    for (int i = 0; i < g->alt_count; i++) {
        Frame top = {FRAME_SEQ, &g->alts[i], 0, NULL, NULL, NULL};
        if (match_frame(&top, text, end)) return true;
    }
    return false;
}

/* Scans for the first position with a match, starting from scan_from. subject_true_start is the
   real beginning of the whole subject -- kept separate from scan_from so '^' stays anchored to it
   even when a caller (replace()'s loop) scans repeatedly from an advancing cursor. *match_len is
   set to the matched span's length. */
static const char* regex_search(Group* g, const char* subject_true_start, const char* scan_from,
                                int* match_len) {
    subject_start = subject_true_start;
    for (const char* s = scan_from;; s++) {
        const char* end;
        if (regex_match_at(g, s, &end)) {
            *match_len = (int)(end - s);
            return s;
        }
        if (*s == '\0') return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Public API -- regex.match/find/replace                              */
/* ------------------------------------------------------------------ */

bool aer_regex_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_REGEX_MATCH && arg_count == 2) {
        AerVal pat_v = vm_stack_pop(vm);
        AerVal str_v = vm_stack_pop(vm);
        if (aer_type(str_v) != TYPE_STRING || aer_type(pat_v) != TYPE_STRING) {
            error("regex.match() requires a string and a pattern string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Group* g = regex_compile(aer_as_string(pat_v)->data);
        if (!g) {
            error("regex.match(): invalid pattern '%s'", aer_as_string(pat_v)->data);
            vm_stack_push(vm, aer_null());
            return true;
        }
        int len;
        const char* subject = aer_as_string(str_v)->data;
        bool found = regex_search(g, subject, subject, &len) != NULL;
        free_group(g);
        vm_stack_push(vm, aer_bool(found));
        return true;
    }

    if (fn_id == FN_REGEX_FIND && arg_count == 2) {
        AerVal pat_v = vm_stack_pop(vm);
        AerVal str_v = vm_stack_pop(vm);
        if (aer_type(str_v) != TYPE_STRING || aer_type(pat_v) != TYPE_STRING) {
            error("regex.find() requires a string and a pattern string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Group* g = regex_compile(aer_as_string(pat_v)->data);
        if (!g) {
            error("regex.find(): invalid pattern '%s'", aer_as_string(pat_v)->data);
            vm_stack_push(vm, aer_null());
            return true;
        }
        int len;
        const char* subject = aer_as_string(str_v)->data;
        const char* at = regex_search(g, subject, subject, &len);
        free_group(g);
        if (!at) {
            vm_stack_push(vm, aer_null());
            return true;
        }
        char* buf = xmalloc((size_t)len + 1);
        memcpy(buf, at, (size_t)len);
        buf[len] = '\0';
        vm_stack_push(vm, aer_make_string(buf, (unsigned int)len));
        return true;
    }

    if (fn_id == FN_REGEX_REPLACE && arg_count == 3) {
        AerVal repl_v = vm_stack_pop(vm);
        AerVal pat_v = vm_stack_pop(vm);
        AerVal str_v = vm_stack_pop(vm);
        if (aer_type(str_v) != TYPE_STRING || aer_type(pat_v) != TYPE_STRING ||
            aer_type(repl_v) != TYPE_STRING) {
            error("regex.replace() requires a string, a pattern string, and a replacement string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Group* g = regex_compile(aer_as_string(pat_v)->data);
        if (!g) {
            error("regex.replace(): invalid pattern '%s'", aer_as_string(pat_v)->data);
            vm_stack_push(vm, aer_null());
            return true;
        }

        AerString* subject = aer_as_string(str_v);
        AerString* repl = aer_as_string(repl_v);
        size_t cap = subject->length + 1, len = 0;
        char* out = xmalloc(cap);
        const char* cursor = subject->data;
        for (;;) {
            int mlen;
            const char* at = regex_search(g, subject->data, cursor, &mlen);
            size_t chunk = at ? (size_t)(at - cursor) : strlen(cursor);
            if (len + chunk + repl->length + 1 > cap) {
                while (len + chunk + repl->length + 1 > cap)
                    cap *= 2;
                out = xrealloc(out, cap);
            }
            memcpy(out + len, cursor, chunk);
            len += chunk;
            if (!at) break;
            memcpy(out + len, repl->data, repl->length);
            len += repl->length;
            /* A zero-width match (e.g. pattern "a*" against "bbb") must still advance past one
               real character, or replacement never terminates. */
            if (mlen == 0) {
                if (at[0] == '\0') break;
                if (len + 1 >= cap) {
                    cap *= 2;
                    out = xrealloc(out, cap);
                }
                out[len++] = at[0];
                cursor = at + 1;
            } else {
                cursor = at + mlen;
            }
        }
        free_group(g);
        out = xrealloc(out, len + 1);
        out[len] = '\0';
        vm_stack_push(vm, aer_make_string(out, (unsigned int)len));
        return true;
    }

    if (fn_id == FN_REGEX_FIND_ALL && arg_count == 2) {
        AerVal pat_v = vm_stack_pop(vm);
        AerVal str_v = vm_stack_pop(vm);
        if (aer_type(str_v) != TYPE_STRING || aer_type(pat_v) != TYPE_STRING) {
            error("regex.find_all() requires a string and a pattern string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Group* g = regex_compile(aer_as_string(pat_v)->data);
        if (!g) {
            error("regex.find_all(): invalid pattern '%s'", aer_as_string(pat_v)->data);
            vm_stack_push(vm, aer_null());
            return true;
        }

        AerString* subject = aer_as_string(str_v);
        AerArray* r = vm_new_array();
        r->count = 0;
        r->capacity = 4;
        r->items = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape = NULL;
        r->generation = 0;

        const char* cursor = subject->data;
        for (;;) {
            int mlen;
            const char* at = regex_search(g, subject->data, cursor, &mlen);
            if (!at) break;
            char* buf = xmalloc((size_t)mlen + 1);
            memcpy(buf, at, (size_t)mlen);
            buf[mlen] = '\0';
            if (r->count >= r->capacity) {
                r->capacity *= 2;
                r->items = xrealloc(r->items, sizeof(AerVal) * r->capacity);
            }
            r->items[r->count++] = aer_make_string(buf, (unsigned int)mlen);
            /* A zero-width match (e.g. pattern "a*" against "bbb") must still advance past one
               real character, or this loop never terminates -- same guard replace() uses. */
            if (mlen == 0) {
                if (at[0] == '\0') break;
                cursor = at + 1;
            } else {
                cursor = at + mlen;
            }
        }
        free_group(g);
        vm_stack_push(vm, aer_array_val(r));
        return true;
    }

    return false;
}
