#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "value_box.h"

/* Operator tables, indexed by (token_type - TOKEN_OR) — matches the contiguous binary op block in lexer.h. */

static const unsigned int BINARY_OP_START = TOKEN_OR;
static const unsigned int binary_precedence[] = {
    1, 1,           /* OR, PIPE          */
    2,              /* AND               */
    3, 4, 5,        /* BIT_OR, XOR, AND  */
    6, 6,           /* EQ, NEQ           */
    7, 7, 7, 7, 7,  /* LT, GT, LTE, GTE, IN */
    8, 8,           /* LSHIFT, RSHIFT    */
    9, 9,           /* ADD, SUB          */
    10, 10, 10, 10, /* MUL, DIV, MOD, FLOOR_DIV */
    11,             /* AS — binds tighter than everything else: `x as integer + 1` is `(x as integer) + 1` */
};
static const Opcode binary_ops[] = {
    OP_OR,  OP_PIPE, OP_AND,
    OP_BITWISE_OR, OP_BITWISE_XOR, OP_BITWISE_AND,
    OP_EQ,  OP_NEQ,
    OP_LT,  OP_GT, OP_LTE, OP_GTE, OP_IN,
    OP_LSHIFT, OP_RSHIFT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_FLOOR_DIV,
    OP_CAST,
};
static const unsigned int BINARY_OP_COUNT =
    sizeof(binary_precedence) / sizeof(*binary_precedence);

/* Loop context — for break and continue backpatching. */

#define LOOP_MAX       16
#define BREAK_MAX      64
#define EXPR_DEPTH_MAX 200  /* max nesting of parens/unary/literals/call-args/inline-if */

typedef struct {
    unsigned int top;                 /* loop condition address (continue target) */
    unsigned int patches[BREAK_MAX];  /* break OP_JUMP operand positions          */
    unsigned int patch_count;
    int scope_depth;                  /* compile_scope_depth before loop's scope  */
    int iter_slots;                   /* extra value-stack slots held by iterator (0=while, 2=for-each) */
} LoopContext;

static LoopContext loop_stack[LOOP_MAX];
static int loop_depth          = 0;   /* total loops on stack                   */
static int loop_floor          = 0;   /* function-relative floor for break/cont */
static int compile_scope_depth = 0;   /* mirrors PUSH_SCOPE / POP_SCOPE emits   */
static int function_depth      = 0;   /* nesting depth of function definitions  */

/* Set just before parse_compound() returns (it always exits at a fresh statement
   boundary: EOF/ELSE/DEDENT). Callers' skip-loops check this first so they can tell
   "still mid-statement" from "a nested block already recovered here" — without it,
   recovery would skip past the boundary into the next statement and discard it. */
static bool recovered_at_boundary = false;

/* Called by both recovery sites after their skip-to-boundary loop stops at a plain
   NEWLINE (not already a DEDENT/EOF boundary). A statement that failed to properly
   introduce a block — e.g. a mistyped 'while' ('for' doubles as while here) — leaves
   an orphaned indented block that parse_statement() has no case for; left alone it
   cascades into one "Expected a statement" error per line instead of just the one
   error that explains the actual mistake. This swallows the whole block as junk. */
static void skip_orphaned_block(void) {
    if (!equal(TOKEN_NEW_LINE)) return;
    lex();
    if (!equal(TOKEN_INDENT)) return;
    int depth = 0;
    do {
        if (equal(TOKEN_INDENT)) depth++;
        else if (equal(TOKEN_DEDENT)) depth--;
        lex();
    } while (depth > 0 && !equal(TOKEN_END_OF_FILE));
}

/* Set only around a pipe target's arg-list parse; checked at every call site so
   `x |> f(g(1))` is rejected regardless of nesting depth. */
static bool pipe_forbid_calls = false;

/* Set by parse_call_args to c->count right after emitting a bare OP_CALL; checked by
   parse_return to detect a true tail call. c->count only grows, and anything that could
   wrap a call (operator, unary, `as T`, multi-return comma) emits more words after —
   so last_bare_call_end == c->count is a robust proof the whole return expr is one bare call. */
static unsigned int last_bare_call_end = (unsigned int)-1;

/* Closure capture analysis: no compile-time symbol table, so this uses a single-pass
   rule (first occurrence of a name decides captured vs. local) instead of Python-style
   whole-function hoisting. A first-occurrence write is ambiguous alone (looks like a
   fresh local); outer_locals below resolves it by checking if the enclosing function
   already assigned the name. Deliberately narrow: only one nesting level can capture
   (see parse_function_expr's capture_supported), so this never needs to be a stack. */
#define MAX_TRACKED_LOCALS 32

typedef struct {
    unsigned int local_names[MAX_TRACKED_LOCALS];
    unsigned int local_count;
    unsigned int captured_names[MAX_CAPTURES];   /* in first-capture order == upvalue slot order */
    unsigned int capture_count;
} CaptureCtx;

static CaptureCtx capture_ctx;
static bool        capturing = false;   /* true only while compiling a capturing function's body */

/* Names assigned so far in the current top-level function's body (function_depth==1) —
   resolves the setter ambiguity above; populated by emit_store/emit_define, reset per
   function in parse_function_body. */
static unsigned int outer_locals[MAX_TRACKED_LOCALS];
static unsigned int outer_local_count = 0;

static bool outer_locals_has(unsigned int name_idx) {
    for (unsigned int i = 0; i < outer_local_count; i++)
        if (outer_locals[i] == name_idx) return true;
    return false;
}

static void outer_locals_add(unsigned int name_idx) {
    if (!outer_locals_has(name_idx) && outer_local_count < MAX_TRACKED_LOCALS)
        outer_locals[outer_local_count++] = name_idx;
}

static bool capture_is_local(unsigned int name_idx) {
    for (unsigned int i = 0; i < capture_ctx.local_count; i++)
        if (capture_ctx.local_names[i] == name_idx) return true;
    return false;
}

static bool capture_is_captured(unsigned int name_idx) {
    for (unsigned int i = 0; i < capture_ctx.capture_count; i++)
        if (capture_ctx.captured_names[i] == name_idx) return true;
    return false;
}

/* Returns the upvalue slot index for an already-captured name; only valid once capture_is_captured(name_idx) is true. */
static unsigned int capture_slot(unsigned int name_idx) {
    unsigned int i = 0;
    for (; i < capture_ctx.capture_count; i++)
        if (capture_ctx.captured_names[i] == name_idx) break;
    return i;
}

/* Classifies name_idx on first encounter (see capture rule above); already-classified names report their existing classification. */
static bool capture_classify(unsigned int name_idx, bool as_write) {
    if (capture_is_local(name_idx))    return false;
    if (capture_is_captured(name_idx)) return true;
    if (as_write && !outer_locals_has(name_idx)) {
        if (capture_ctx.local_count < MAX_TRACKED_LOCALS)
            capture_ctx.local_names[capture_ctx.local_count++] = name_idx;
        return false;
    }
    if (capture_ctx.capture_count < MAX_CAPTURES) {
        capture_ctx.captured_names[capture_ctx.capture_count++] = name_idx;
    } else {
        error_at("Too many captured variables in one closure (max %d)", MAX_CAPTURES);
    }
    return true;
}

/* Shares vm.h's SCOPE_SLOT_MAX (not independent) — slots this table hands out are written into ScopeSlot[] with no runtime bounds check. */
#define MAX_PARAMS   SCOPE_SLOT_MAX

/* Parameters of the function CURRENTLY being compiled — lets emit_load/emit_store use
   the fast OP_LOAD_LOCAL/OP_STORE_LOCAL/OP_DEFINE_LOCAL slot opcodes (CPython's LOAD_FAST)
   instead of name-based ones. Deliberately not extended to ordinary body locals: a
   "hybrid scopes" idea was tried and reverted (see git history) because a plain
   `x = value` in a function body must walk up and mutate an existing outer/global `x`
   per AER's no-implicit-shadowing rule — treating it as an unconditional fresh local
   broke that, caught by tests/test.aer's `mutate_outer` case. A parameter has no such
   ambiguity: it's always a fresh binding, never a walk-up candidate. Saved/restored
   around a nested function body exactly like capturing/capture_ctx in parse_function. */
static unsigned int current_params[MAX_PARAMS];
static unsigned int current_param_count = 0;

/* Returns name_idx's slot if it's a parameter of the current function, else -1 (emit_load/emit_store fall back to name-based opcodes). */
static int current_param_slot(unsigned int name_idx) {
    for (unsigned int i = 0; i < current_param_count; i++)
        if (current_params[i] == name_idx) return (int)i;
    return -1;
}

/* Replaces direct `chunk_emit(c, OP_LOAD); chunk_emit(c, name_idx);` at every read site; identical behavior outside a capturing function. */
static void emit_load(Chunk* c, unsigned int name_idx) {
    if (capturing && capture_classify(name_idx, false)) {
        chunk_emit(c, OP_LOAD_UPVALUE);
        chunk_emit(c, (int)capture_slot(name_idx));
        return;
    }
    int slot = current_param_slot(name_idx);
    if (slot >= 0) {
        chunk_emit(c, OP_LOAD_LOCAL);
        chunk_emit(c, slot);
        return;
    }
    chunk_emit(c, OP_LOAD);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)chunk_add_addr_cache(c));
}

/* Replaces direct OP_STORE emission at plain-assignment write sites (not params/loop
   vars — see emit_define); same behavior outside capturing functions. */
static void emit_store(Chunk* c, unsigned int name_idx) {
    if (function_depth == 1) outer_locals_add(name_idx);
    if (capturing) {
        bool already_classified = capture_is_local(name_idx) || capture_is_captured(name_idx);
        bool captured = capture_classify(name_idx, true);
        if (captured) {
            chunk_emit(c, OP_STORE_UPVALUE);
            chunk_emit(c, (int)capture_slot(name_idx));
            return;
        }
        if (!already_classified) {
            /* First occurrence, now classified local: OP_DEFINE guarantees a fresh local instead of risking OP_STORE walking up to an unrelated outer binding. */
            chunk_emit(c, OP_DEFINE);
            chunk_emit(c, (int)name_idx);
            return;
        }
    }
    int slot = current_param_slot(name_idx);
    if (slot >= 0) {
        chunk_emit(c, OP_STORE_LOCAL);
        chunk_emit(c, slot);
        return;
    }
    chunk_emit(c, OP_STORE);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)chunk_add_addr_cache(c));
}

/* Opcode-fusion support (see OP_COMPOUND_*'s comment in vm.h, and the array-index-get
   family). Only ever applied to bytecode the parser itself just emitted, inspected
   within the same statement's compilation — never a post-hoc peephole pass, which is
   what makes discard-and-re-emit safe against AER's absolute-offset jump patching. */
typedef enum { OPERAND_NONE, OPERAND_LOCAL, OPERAND_NAME, OPERAND_CONST } OperandKind;

typedef struct {
    OperandKind  kind;
    int a, b;   /* LOCAL: a=slot. NAME: a=name_idx, b=cache_idx. CONST: a=pool_idx. */
} Operand;

/* Classifies c->code[start,end) as a "simple" shape a fused opcode can consume directly
   (matching emit_load's LOCAL/NAME or a bare CONST literal). Checks exact word count,
   not just the leading opcode — `x += -1` emits 3 words (OP_PUSH, pool_idx, OP_NEGATE),
   which must not be misread as a 2-word CONST shape, silently dropping the OP_NEGATE. */
static Operand classify_operand(Chunk* c, unsigned int start, unsigned int end) {
    unsigned int len = end - start;
    if (len == 2 && c->code[start] == OP_LOAD_LOCAL) {
        Operand o = { OPERAND_LOCAL, c->code[start + 1], 0 };
        return o;
    }
    if (len == 3 && c->code[start] == OP_LOAD) {
        Operand o = { OPERAND_NAME, c->code[start + 1], c->code[start + 2] };
        return o;
    }
    if (len == 2 && c->code[start] == OP_PUSH) {
        Operand o = { OPERAND_CONST, c->code[start + 1], 0 };
        return o;
    }
    Operand none = { OPERAND_NONE, 0, 0 };
    return none;
}

/* For loop variables — always unambiguously local, so this just registers the name and
   emits plain OP_DEFINE. Parameters use emit_define_param's fast slot path instead; loop
   vars can't, since a fresh scope is pushed every iteration with no call-lifetime slot. */
static void emit_define(Chunk* c, unsigned int name_idx) {
    if (function_depth == 1) outer_locals_add(name_idx);
    if (capturing && !capture_is_local(name_idx) && !capture_is_captured(name_idx)
        && capture_ctx.local_count < MAX_TRACKED_LOCALS) {
        capture_ctx.local_names[capture_ctx.local_count++] = name_idx;
    }
    chunk_emit(c, OP_DEFINE);
    chunk_emit(c, (int)name_idx);
}

/* One parameter of the function being compiled — mirrors emit_define's bookkeeping
   (so later reassignment or closure capture still classifies correctly), but also
   records (name_idx -> slot) in current_params so emit_load/emit_store later resolve
   it to OP_LOAD_LOCAL/OP_STORE_LOCAL instead of a name-based lookup. */
static void emit_define_param(Chunk* c, unsigned int name_idx) {
    if (function_depth == 1) outer_locals_add(name_idx);
    if (capturing && !capture_is_local(name_idx) && !capture_is_captured(name_idx)
        && capture_ctx.local_count < MAX_TRACKED_LOCALS) {
        capture_ctx.local_names[capture_ctx.local_count++] = name_idx;
    }
    unsigned int slot = current_param_count;
    if (current_param_count < MAX_PARAMS) current_params[current_param_count++] = name_idx;
    chunk_emit(c, OP_DEFINE_LOCAL);
    chunk_emit(c, (int)slot);
    chunk_emit(c, (int)name_idx);
}

/* Recursive-descent has no implicit recursion limit — deeply nested source would exhaust the C stack; expr_depth bounds it via expr_depth_enter() below. */
static int expr_depth = 0;

/* Returns true (and increments expr_depth) within the limit; on false, error_at() already reported it and the caller must return without recursing. */
static bool expr_depth_enter(void) {
    if (expr_depth >= EXPR_DEPTH_MAX) {
        error_at("Expression nested too deeply (max %d levels)", EXPR_DEPTH_MAX);
        return false;
    }
    expr_depth++;
    return true;
}

#define MAX_DESTRUCT 16

/* Forward declarations */

static void parse_statement(Chunk* c);
static void parse_assignment(Chunk* c);
static void parse_binary(Chunk* c, unsigned int min_prec);
static void parse_binary_ops(Chunk* c, unsigned int min_prec);
static void parse_unary(Chunk* c);
static void parse_primary(Chunk* c);
static void parse_primary_inner(Chunk* c);
static void parse_index_or_slice(Chunk* c);
static unsigned int parse_arg_list(Chunk* c);
static void parse_call_args(Chunk* c, unsigned int name_idx);
static void parse_compound(Chunk* c);
static void parse_if(Chunk* c);
static void parse_if_expr(Chunk* c);
static void parse_for(Chunk* c);
static void parse_struct(Chunk* c);
static void parse_function(Chunk* c);
static void parse_function_expr(Chunk* c);
static void parse_return(Chunk* c);
static void parse_break(Chunk* c);
static void parse_continue(Chunk* c);
static void parse_defer(Chunk* c);
static void parse_import(Chunk* c);
static bool at_module_name(Chunk* c);
static void parse_module_call(Chunk* c);

/* ------------------------------------------------------------------ */
/* Entry point — emits bytecode for all statements until EOF           */
/* ------------------------------------------------------------------ */

void parse(Chunk* c) {
    /* parse_had_error resets per-statement below so recovery can tell if *this*
       statement failed, else an earlier failure would be lost once a later statement
       succeeds; any_error accumulates file-wide for callers like aer_module.c's
       `ok = !parse_had_error` check after parse() returns. */
    bool any_error = false;
    while (!equal(TOKEN_END_OF_FILE)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (consume(TOKEN_DEDENT))   continue;
        parse_had_error       = false;
        recovered_at_boundary = false;
        unsigned int saved       = c->count;             /* rollback point */
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        /* Error recovery: roll back partial bytecode and skip to the next line — unless
           a nested parse_compound() already recovered to a boundary, in which case the
           current token starts the next statement, not leftover junk (see recovered_at_boundary above). */
        if (parse_had_error) {
            any_error = true;
            c->count           = saved;
            c->line_mark_count = saved_marks;
            if (!recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) &&
                       !equal(TOKEN_NEW_LINE)    &&
                       !equal(TOKEN_DEDENT))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    if (any_error) parse_had_error = true;
}

/* ------------------------------------------------------------------ */
/* Statements                                                           */
/* ------------------------------------------------------------------ */

static void parse_statement(Chunk* c) {
    if (consume(TOKEN_IF))           { parse_if(c);                                 return; }
    if (consume(TOKEN_FOR))          { parse_for(c);                                return; }
    if (consume(TOKEN_STRUCT))       { parse_struct(c);                             return; }
    if (consume(TOKEN_FUNCTION))     { parse_function(c);                           return; }
    if (consume(TOKEN_RETURN))       { parse_return(c);                             return; }
    if (consume(TOKEN_BREAK))        { parse_break(c);                              return; }
    if (consume(TOKEN_CONTINUE))     { parse_continue(c);                           return; }
    if (consume(TOKEN_DEFER))        { parse_defer(c);                              return; }
    if (consume(TOKEN_IMPORT))       { parse_import(c);                             return; }
    if (at_module_name(c)) {
        parse_module_call(c);
        chunk_emit(c, mode == MODE_SHELL ? OP_PRINT_REPL : OP_POP);
        return;
    }
    if (equal(TOKEN_IDENTIFIER))     { parse_assignment(c);                         return; }
    if (equal(TOKEN_IN))             { error_at("'in' cannot start a statement; use it inside an expression, e.g. 'if x in collection:' or 'for x in collection:'"); return; }
    error_at("Expected a statement");
}

static void parse_assignment(Chunk* c) {
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    if (consume(TOKEN_OPEN_PARENTHESE)) {
        parse_call_args(c, name_idx);
        chunk_emit(c, mode == MODE_SHELL ? OP_PRINT_REPL : OP_POP);
        return;
    }

    if (equal(TOKEN_OPEN_BRACKET) || equal(TOKEN_DOT)) {
        /* a[i], a.x, and mixed chains (a.b[0].c = ...) all land here — the pending step resolves as a GET until another step follows, and as the final SET once '=' is reached. */
        emit_load(c, name_idx);

        bool pending_is_field = false;
        unsigned int pending_field_idx = 0;

        if (consume(TOKEN_OPEN_BRACKET)) {
            parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
        } else {
            consume(TOKEN_DOT);
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
            pending_field_idx = chunk_add_pool(c, token.value);
            pending_is_field  = true;
            lex();
        }

        while (equal(TOKEN_OPEN_BRACKET) || equal(TOKEN_DOT)) {
            if (pending_is_field) { chunk_emit(c, OP_FIELD_GET); chunk_emit(c, (int)pending_field_idx); }
            else                    chunk_emit(c, OP_INDEX_GET);

            if (consume(TOKEN_OPEN_BRACKET)) {
                parse_binary(c, 0);
                require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
                pending_is_field = false;
            } else {
                consume(TOKEN_DOT);
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
                pending_field_idx = chunk_add_pool(c, token.value);
                pending_is_field  = true;
                lex();
            }
        }

        if (equal(TOKEN_ASSIGN)) {
            consume(TOKEN_ASSIGN);
            parse_binary(c, 0);          /* value */
            if (pending_is_field) { chunk_emit(c, OP_FIELD_SET); chunk_emit(c, (int)pending_field_idx); }
            else                    chunk_emit(c, OP_INDEX_SET);
            return;
        }

        /* Not a plain assignment — finish the pending step as a GET and fall through to a
           general expression statement: a call on the retrieved value (queue[i](), obj.method()),
           further chaining, and/or a trailing pipe (b.neighbors |> append(a)), discarded like
           any other expression statement. Anything else is the genuine "compound assignment
           to an indexed/field value" error this used to always report unconditionally. */
        if (!equal(TOKEN_OPEN_PARENTHESE) && !equal(TOKEN_PIPE)) {
            error_at("Compound assignment to an indexed or field value is not supported; use 'a.x = a.x + 1'");
            return;
        }
        if (pending_is_field) { chunk_emit(c, OP_FIELD_GET); chunk_emit(c, (int)pending_field_idx); }
        else                    chunk_emit(c, OP_INDEX_GET);
        for (;;) {
            if (consume(TOKEN_OPEN_BRACKET)) {
                parse_index_or_slice(c);
            } else if (consume(TOKEN_OPEN_PARENTHESE)) {
                unsigned int n = parse_arg_list(c);
                if (pipe_forbid_calls) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
                chunk_emit(c, OP_CALL_VALUE);
                chunk_emit(c, (int)n);
            } else if (consume(TOKEN_DOT)) {
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
                unsigned int field_idx = chunk_add_pool(c, token.value);
                lex();
                chunk_emit(c, OP_FIELD_GET);
                chunk_emit(c, (int)field_idx);
            } else {
                break;
            }
        }
        parse_binary_ops(c, 0);   /* optional trailing pipe/operator chain */
        chunk_emit(c, mode == MODE_SHELL ? OP_PRINT_REPL : OP_POP);
        return;
    }

    if (consume(TOKEN_COMMA)) {
        /* Destructuring: a, b, c = expr  or  a, b = x, y (implicit array RHS) */
        unsigned int names[MAX_DESTRUCT];
        names[0] = name_idx;
        unsigned int count = 1;
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected identifier in destructuring"); return; }
            if (count >= MAX_DESTRUCT)    { error_at("Too many destructuring targets");       return; }
            names[count++] = chunk_add_pool(c, token.value);
            lex();
        } while (consume(TOKEN_COMMA));
        require(TOKEN_ASSIGN, "expected '=' after destructuring targets");

        /* RHS: one expression, or comma-separated list packed into an implicit array */
        parse_binary(c, 0);
        unsigned int rhs = 1;
        while (consume(TOKEN_COMMA)) { parse_binary(c, 0); rhs++; }
        if (rhs > 1) { chunk_emit(c, OP_ARRAY_NEW); chunk_emit(c, (int)rhs); }

        /* Unpack each target from TOS (array stays on stack until all stores done) */
        for (unsigned int i = 0; i < count; i++) {
            chunk_emit(c, OP_UNPACK);
            chunk_emit(c, (int)i);
            emit_store(c, names[i]);
        }
        chunk_emit(c, OP_POP);   /* discard the array */
        return;
    }

    if (consume(TOKEN_ASSIGN)) {
        parse_binary(c, 0);
        emit_store(c, name_idx);
        return;
    }

    static const struct { TokenType tok; Opcode op; } ops[] = {
        { TOKEN_ADD_ASSIGN,          OP_ADD         },
        { TOKEN_SUBTRACT_ASSIGN,     OP_SUB         },
        { TOKEN_MULTIPLY_ASSIGN,     OP_MUL         },
        { TOKEN_DIVIDE_ASSIGN,       OP_DIV         },
        { TOKEN_MODULO_ASSIGN,       OP_MOD         },
        { TOKEN_LEFT_SHIFT_ASSIGN,   OP_LSHIFT      },
        { TOKEN_RIGHT_SHIFT_ASSIGN,  OP_RSHIFT      },
        { TOKEN_AND_ASSIGN,          OP_BITWISE_AND },
        { TOKEN_OR_ASSIGN,           OP_BITWISE_OR  },
        { TOKEN_XOR_ASSIGN,          OP_BITWISE_XOR },
        { TOKEN_FLOOR_DIVIDE_ASSIGN, OP_FLOOR_DIV   },
    };
    for (int i = 0; i < (int)(sizeof(ops) / sizeof(*ops)); i++) {
        if (consume(ops[i].tok)) {
            unsigned int lhs_start = c->count;
            emit_load(c, name_idx);
            unsigned int rhs_start = c->count;
            parse_binary(c, 0);
            Operand lhs = classify_operand(c, lhs_start, rhs_start);
            Operand rhs = classify_operand(c, rhs_start, c->count);
            /* All 6 LHS x RHS shapes fuse now (Phase 1: NAME_CONST/NAME_NAME; Phase 2: the
               LOCAL-involving ones — see OP_COMPOUND_*'s comment in vm.h). A LOCAL (parameter)
               LHS/RHS goes through vm_read_local_slot/vm_write_local_slot's closure-box check,
               matching lbl_load_local/lbl_store_local exactly. Reuses lhs/rhs's already-allocated
               cache slots (from the emit_load calls just made) for any NAME operand. */
            if ((lhs.kind == OPERAND_LOCAL || lhs.kind == OPERAND_NAME) &&
                (rhs.kind == OPERAND_CONST || rhs.kind == OPERAND_LOCAL || rhs.kind == OPERAND_NAME)) {
                c->count = lhs_start;   /* discard the LOAD+RHS just emitted */
                if (lhs.kind == OPERAND_LOCAL) {
                    if (rhs.kind == OPERAND_CONST) {
                        chunk_emit(c, OP_COMPOUND_LOCAL_CONST);
                        chunk_emit(c, lhs.a);
                        chunk_emit(c, (int)ops[i].op);
                        chunk_emit(c, rhs.a);
                    } else if (rhs.kind == OPERAND_LOCAL) {
                        chunk_emit(c, OP_COMPOUND_LOCAL_LOCAL);
                        chunk_emit(c, lhs.a);
                        chunk_emit(c, (int)ops[i].op);
                        chunk_emit(c, rhs.a);
                    } else {
                        chunk_emit(c, OP_COMPOUND_LOCAL_NAME);
                        chunk_emit(c, lhs.a);
                        chunk_emit(c, (int)ops[i].op);
                        chunk_emit(c, rhs.a); chunk_emit(c, rhs.b);
                    }
                } else {
                    if (rhs.kind == OPERAND_CONST) {
                        chunk_emit(c, OP_COMPOUND_NAME_CONST);
                        chunk_emit(c, lhs.a); chunk_emit(c, lhs.b);
                        chunk_emit(c, (int)ops[i].op);
                        chunk_emit(c, rhs.a);
                    } else if (rhs.kind == OPERAND_LOCAL) {
                        chunk_emit(c, OP_COMPOUND_NAME_LOCAL);
                        chunk_emit(c, lhs.a); chunk_emit(c, lhs.b);
                        chunk_emit(c, (int)ops[i].op);
                        chunk_emit(c, rhs.a);
                    } else {
                        chunk_emit(c, OP_COMPOUND_NAME_NAME);
                        chunk_emit(c, lhs.a); chunk_emit(c, lhs.b);
                        chunk_emit(c, (int)ops[i].op);
                        chunk_emit(c, rhs.a); chunk_emit(c, rhs.b);
                    }
                }
                return;
            }
            /* Not fusable — a non-simple RHS (upvalue or compound expression); byte-for-byte what this code already did. */
            chunk_emit(c, (int)ops[i].op);
            emit_store(c, name_idx);
            return;
        }
    }

    if (equal(TOKEN_PIPE)) {
        /* A pipe chain starting from a bare name, used as a statement, e.g. 'x |> length()' — same expression-statement treatment as the field/index-chain case above. */
        emit_load(c, name_idx);
        parse_binary_ops(c, 0);
        chunk_emit(c, mode == MODE_SHELL ? OP_PRINT_REPL : OP_POP);
        return;
    }

    error_at("Expected assignment operator after identifier");
}

/* ------------------------------------------------------------------ */
/* Expressions                                                          */
/* ------------------------------------------------------------------ */

/* Operator-climb from current token; LHS must already be on the stack. */
static void parse_binary_ops(Chunk* c, unsigned int min_prec) {
    while (1) {
        unsigned int idx = (unsigned int)token.type - BINARY_OP_START;
        if (idx >= BINARY_OP_COUNT || binary_precedence[idx] <= min_prec)
            break;
        Opcode       op   = binary_ops[idx];
        unsigned int prec = binary_precedence[idx];
        lex();

        if (op == OP_AND) {
            /* Short-circuit: LHS false → false, RHS never evaluated */
            chunk_emit(c, OP_JUMP_IF_FALSE);
            unsigned int lhs_patch = c->count; chunk_emit(c, 0);
            parse_binary(c, prec);
            chunk_emit(c, OP_JUMP_IF_FALSE);
            unsigned int rhs_patch = c->count; chunk_emit(c, 0);
            chunk_emit(c, OP_PUSH); chunk_emit(c, (int)chunk_add_pool(c, aer_bool(true)));
            chunk_emit(c, OP_JUMP);
            unsigned int end_patch = c->count; chunk_emit(c, 0);
            c->code[lhs_patch] = c->code[rhs_patch] = (int)c->count;
            chunk_emit(c, OP_PUSH); chunk_emit(c, (int)chunk_add_pool(c, aer_bool(false)));
            c->code[end_patch] = (int)c->count;
        } else if (op == OP_OR) {
            /* Short-circuit: LHS true → true, RHS never evaluated */
            chunk_emit(c, OP_JUMP_IF_TRUE);
            unsigned int lhs_patch = c->count; chunk_emit(c, 0);
            parse_binary(c, prec);
            chunk_emit(c, OP_JUMP_IF_TRUE);
            unsigned int rhs_patch = c->count; chunk_emit(c, 0);
            chunk_emit(c, OP_PUSH); chunk_emit(c, (int)chunk_add_pool(c, aer_bool(false)));
            chunk_emit(c, OP_JUMP);
            unsigned int end_patch = c->count; chunk_emit(c, 0);
            c->code[lhs_patch] = c->code[rhs_patch] = (int)c->count;
            chunk_emit(c, OP_PUSH); chunk_emit(c, (int)chunk_add_pool(c, aer_bool(true)));
            c->code[end_patch] = (int)c->count;
        } else if (op == OP_PIPE) {
            /* x |> f(args) desugars to f(x, args) — x is already on the stack as argument
               zero, so this parses a normal "(args)" and bumps the arg count by one. f's
               arguments may not themselves contain a function call at any depth, so pipe
               chains stay flat. A module-qualified target (x |> string.upper()) works the
               same way, emitting OP_CALL_MODULE instead — same "arg count already includes
               the piped value" trick, since OP_CALL_MODULE pops arg_count values like OP_CALL. */
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '|>'"); return; }
            if (at_module_name(c)) {
                unsigned int module_idx = chunk_add_pool(c, token.value);
                lex();
                if (!consume(TOKEN_DOT)) {
                    error_at("A module can't be used as a value on its own — call a function on it, e.g. 'module.function(...)'");
                    return;
                }
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '.'"); return; }
                unsigned int fn_idx = chunk_add_pool(c, token.value);
                lex();
                require(TOKEN_OPEN_PARENTHESE, "expected '(' after module function name");
                bool outer_forbid = pipe_forbid_calls;
                pipe_forbid_calls = true;
                unsigned int n = parse_arg_list(c);
                pipe_forbid_calls = outer_forbid;
                if (outer_forbid) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
                chunk_emit(c, OP_CALL_MODULE);
                chunk_emit(c, (int)module_idx);
                chunk_emit(c, (int)fn_idx);
                chunk_emit(c, (int)(n + 1));
            } else {
                unsigned int name_idx = chunk_add_pool(c, token.value);
                lex();
                require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped function name");
                bool outer_forbid = pipe_forbid_calls;
                pipe_forbid_calls = true;
                unsigned int n = parse_arg_list(c);
                pipe_forbid_calls = outer_forbid;
                if (outer_forbid) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
                chunk_emit(c, OP_CALL);
                chunk_emit(c, (int)name_idx);
                chunk_emit(c, (int)(n + 1));
                chunk_emit(c, (int)chunk_add_addr_cache(c));
            }
        } else if (op == OP_CAST) {
            /* x as T — T is a bare type name, read directly rather than through parse_binary.
               Anything not a known primitive is a struct type, resolved at runtime via
               OP_CHECK_SHAPE, which only ever verifies (never converts — no well-defined way to reshape one struct into another). */
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a type name after 'as'"); return; }
            unsigned int type_idx = chunk_add_pool(c, token.value);
            const char* type_name = aer_as_string(token.value)->data;
            unsigned int type_len = aer_as_string(token.value)->length;
            lex();
            if      (type_len == 6 && strncmp(type_name, "string",  6) == 0) { chunk_emit(c, OP_TO_STR); }
            else if (type_len == 7 && strncmp(type_name, "integer", 7) == 0) { chunk_emit(c, OP_CAST); chunk_emit(c, CAST_INTEGER); }
            else if (type_len == 5 && strncmp(type_name, "float",   5) == 0) { chunk_emit(c, OP_CAST); chunk_emit(c, CAST_FLOAT); }
            else if (type_len == 7 && strncmp(type_name, "boolean", 7) == 0) { chunk_emit(c, OP_CAST); chunk_emit(c, CAST_BOOLEAN); }
            else { chunk_emit(c, OP_CHECK_SHAPE); chunk_emit(c, (int)type_idx); }
        } else {
            parse_binary(c, prec);
            chunk_emit(c, (int)op);
        }
    }
}

static void parse_binary(Chunk* c, unsigned int min_prec) {
    parse_unary(c);
    parse_binary_ops(c, min_prec);
}

static void parse_unary_inner(Chunk* c) {
    if (consume(TOKEN_NOT))         { parse_unary(c); chunk_emit(c, OP_NOT);         return; }
    if (consume(TOKEN_BITWISE_NOT)) { parse_unary(c); chunk_emit(c, OP_BITWISE_NOT); return; }
    if (consume(TOKEN_SUBTRACT))    { parse_unary(c); chunk_emit(c, OP_NEGATE);      return; }
    parse_primary(c);
}

static void parse_unary(Chunk* c) {
    if (!expr_depth_enter()) return;
    parse_unary_inner(c);
    expr_depth--;
}

/* Processes escape sequences from raw source s[0..len) into a pool entry (\n \t \\ \" \{); safe to call repeatedly since chunk_add_pool copies. */
static unsigned int pool_escaped_string(Chunk* c, const char* s, unsigned int len) {
    static char buf[4096];
    if (len >= sizeof(buf)) {
        error_at("String literal too long (max %u bytes)", (unsigned int)sizeof(buf) - 1);
        len = 0;
    }
    unsigned int out = 0;
    for (unsigned int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case '\\': buf[out++] = '\\'; break;
                case '"':  buf[out++] = '"';  break;
                case '{':  buf[out++] = '{';  break;
                default:   buf[out++] = '\\'; buf[out++] = s[i]; break;
            }
        } else {
            buf[out++] = s[i];
        }
    }
    /* Copies out of the static scratch buffer — AerString always owns its data, and buf is reused every call so wrapping it directly would alias across literals. */
    char* owned = xmalloc(out + 1);
    memcpy(owned, buf, out);
    owned[out] = '\0';
    AerVal sv = aer_make_string(owned, out);
    return chunk_add_pool(c, sv);
}

static void parse_primary(Chunk* c) {
    if (!expr_depth_enter()) return;
    parse_primary_inner(c);
    expr_depth--;
}

/* Called right after consuming '['. A missing bound on either side of ':' pushes null, which the VM treats as start/end of the collection. */
static void parse_index_or_slice(Chunk* c) {
    bool has_start = !equal(TOKEN_COLON);
    if (has_start) parse_binary(c, 0);

    if (!consume(TOKEN_COLON)) {
        require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
        chunk_emit(c, OP_INDEX_GET);
        return;
    }

    if (!has_start) {
        AerVal null_val = aer_null();
        chunk_emit(c, OP_PUSH);
        chunk_emit(c, (int)chunk_add_pool(c, null_val));
    }
    if (equal(TOKEN_CLOSE_BRACKET)) {
        AerVal null_val = aer_null();
        chunk_emit(c, OP_PUSH);
        chunk_emit(c, (int)chunk_add_pool(c, null_val));
    } else {
        parse_binary(c, 0);
    }
    require(TOKEN_CLOSE_BRACKET, "expected ']' after slice");
    chunk_emit(c, OP_SLICE_GET);
}

static void parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return;
    }
    if (consume(TOKEN_IF)) { parse_if_expr(c); return; }
    if (consume(TOKEN_FUNCTION)) { parse_function_expr(c); return; }
    if (consume(TOKEN_OPEN_BRACKET)) {
        unsigned int count = 0;
        if (!equal(TOKEN_CLOSE_BRACKET)) {
            do { parse_binary(c, 0); count++; } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACKET, "expected ']' after array elements");
        chunk_emit(c, OP_ARRAY_NEW);
        chunk_emit(c, (int)count);
        return;
    }
    if (consume(TOKEN_OPEN_BRACE)) {
        unsigned int count = 0;
        if (!equal(TOKEN_CLOSE_BRACE)) {
            do {
                parse_binary(c, 0);    /* key   */
                require(TOKEN_COLON, "expected ':' between key and value");
                parse_binary(c, 0);    /* value */
                count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACE, "expected '}' after dict entries");
        chunk_emit(c, OP_DICT_NEW);
        chunk_emit(c, (int)count);
        return;
    }

    AerVal v = aer_null();
    if (token.type == TOKEN_INTEGER) {
        v = aer_int(aer_as_int(token.value));
    } else if (token.type == TOKEN_REAL) {
        v = aer_real(aer_as_real(token.value));
    } else if (token.type == TOKEN_TRUE || token.type == TOKEN_FALSE) {
        v = aer_bool(aer_as_bool(token.value));
    } else if (token.type == TOKEN_NULL) {
        v = aer_null();
    } else if (token.type == TOKEN_STRING) {
        char*        s   = aer_as_string(token.value)->data;
        unsigned int len = aer_as_string(token.value)->length;
        /* Detect unescaped '{' — skip \X pairs so \{ doesn't count */
        bool has_interp = false;
        for (unsigned int k = 0; k < len; k++) {
            if (s[k] == '\\' && k + 1 < len) { k++; continue; }
            if (s[k] == '{') { has_interp = true; break; }
        }

        if (has_interp) {
            int parts = 0;
            unsigned int i = 0;
            while (i <= len) {
                /* literal segment up to next unescaped '{' or end */
                unsigned int seg_start = i;
                while (i < len) {
                    if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
                    if (s[i] == '{') break;
                    i++;
                }
                if (i > seg_start) {
                    chunk_emit(c, OP_PUSH);
                    chunk_emit(c, (int)pool_escaped_string(c, s + seg_start, i - seg_start));
                    if (parts++ > 0) chunk_emit(c, OP_ADD);
                }
                if (i >= len) break;
                /* interpolation: {name} */
                i++;
                unsigned int var_start = i;
                while (i < len && s[i] != '}') i++;
                if (i >= len) { error_at("Unclosed '{' in string"); break; }
                if (i == var_start) { error_at("Empty '{}' in string"); i++; continue; }
                /* Copies out of s rather than pointing into it — AerString always owns its data (see the GC design note). */
                unsigned int name_len = i - var_start;
                char* name_buf = xmalloc(name_len + 1);
                memcpy(name_buf, s + var_start, name_len);
                name_buf[name_len] = '\0';
                AerVal name = aer_make_string(name_buf, name_len);
                emit_load(c, chunk_add_pool(c, name));
                chunk_emit(c, OP_TO_STR);
                if (parts++ > 0) chunk_emit(c, OP_ADD);
                i++;
            }
            if (parts == 0) {
                /* A fresh, owned empty buffer, not a static literal — sweeping would fault free()'ing a static string (AerString always owns its data). */
                char* empty_buf = xmalloc(1);
                empty_buf[0] = '\0';
                AerVal empty = aer_make_string(empty_buf, 0);
                chunk_emit(c, OP_PUSH);
                chunk_emit(c, (int)chunk_add_pool(c, empty));
            }
            lex();
            return;
        }
        /* no interpolation: process escape sequences and emit */
        chunk_emit(c, OP_PUSH);
        chunk_emit(c, (int)pool_escaped_string(c, s, len));
        lex();
        return;
    } else if (token.type == TOKEN_IDENTIFIER && at_module_name(c)) {
        parse_module_call(c);
        return;
    } else if (token.type == TOKEN_IDENTIFIER) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) {
            parse_call_args(c, name_idx);
        } else if (consume(TOKEN_OPEN_BRACKET)) {
            /* Array-index-get fusion (Part 2 — see OP_INDEX_GET_*'s comment in vm.h).
               Only a bare identifier's FIRST bracket qualifies; a chained second bracket
               or the postfix loop below never reaches here since their "collection" is
               already an expression result, not a simple name. parse_index_or_slice runs
               unchanged; fusion only truncates and re-emits after, like Part 1's scheme. */
            unsigned int arr_start = c->count;
            emit_load(c, name_idx);
            unsigned int idx_start = c->count;
            parse_index_or_slice(c);
            if (c->code[c->count - 1] == OP_INDEX_GET) {
                Operand arr = classify_operand(c, arr_start, idx_start);
                Operand idx = classify_operand(c, idx_start, c->count - 1);
                if ((arr.kind == OPERAND_LOCAL || arr.kind == OPERAND_NAME) &&
                    (idx.kind == OPERAND_CONST || idx.kind == OPERAND_LOCAL || idx.kind == OPERAND_NAME)) {
                    c->count = arr_start;   /* discard the LOAD+index+OP_INDEX_GET just emitted */
                    if (arr.kind == OPERAND_LOCAL) {
                        if (idx.kind == OPERAND_CONST) {
                            chunk_emit(c, OP_INDEX_GET_LOCAL_CONST);
                            chunk_emit(c, arr.a); chunk_emit(c, idx.a);
                        } else if (idx.kind == OPERAND_LOCAL) {
                            chunk_emit(c, OP_INDEX_GET_LOCAL_LOCAL);
                            chunk_emit(c, arr.a); chunk_emit(c, idx.a);
                        } else {
                            chunk_emit(c, OP_INDEX_GET_LOCAL_NAME);
                            chunk_emit(c, arr.a); chunk_emit(c, idx.a); chunk_emit(c, idx.b);
                        }
                    } else {
                        if (idx.kind == OPERAND_CONST) {
                            chunk_emit(c, OP_INDEX_GET_NAME_CONST);
                            chunk_emit(c, arr.a); chunk_emit(c, arr.b); chunk_emit(c, idx.a);
                        } else if (idx.kind == OPERAND_LOCAL) {
                            chunk_emit(c, OP_INDEX_GET_NAME_LOCAL);
                            chunk_emit(c, arr.a); chunk_emit(c, arr.b); chunk_emit(c, idx.a);
                        } else {
                            chunk_emit(c, OP_INDEX_GET_NAME_NAME);
                            chunk_emit(c, arr.a); chunk_emit(c, arr.b); chunk_emit(c, idx.a); chunk_emit(c, idx.b);
                        }
                    }
                }
            }
            /* Not fusable (slice, non-simple index, or fusion didn't apply) — parse_index_or_slice's emitted code is left as-is. */
        } else {
            emit_load(c, name_idx);
        }
        /* Postfix chain: arr[0](args)[1], f(x)(y), dict["k"](z), p.x.y etc. */
        for (;;) {
            if (consume(TOKEN_OPEN_BRACKET)) {
                parse_index_or_slice(c);
            } else if (consume(TOKEN_OPEN_PARENTHESE)) {
                unsigned int n = parse_arg_list(c);
                if (pipe_forbid_calls) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
                chunk_emit(c, OP_CALL_VALUE);
                chunk_emit(c, (int)n);
            } else if (consume(TOKEN_DOT)) {
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
                unsigned int field_idx = chunk_add_pool(c, token.value);
                lex();
                chunk_emit(c, OP_FIELD_GET);
                chunk_emit(c, (int)field_idx);
            } else {
                break;
            }
        }
        return;
    } else if (token.type == TOKEN_BREAK || token.type == TOKEN_CONTINUE || token.type == TOKEN_RETURN || token.type == TOKEN_DEFER) {
        error_at("'break', 'continue', 'return', and 'defer' are statements and cannot be used as expressions");
        return;
    } else {
        error_at("Expected a value");
        return;
    }
    chunk_emit(c, OP_PUSH);
    chunk_emit(c, (int)chunk_add_pool(c, v));
    lex();
}

/* ------------------------------------------------------------------ */
/* Compound body — requires an INDENT, reads until DEDENT              */
/* ------------------------------------------------------------------ */

static void parse_compound(Chunk* c) {
    require(TOKEN_NEW_LINE, "expected newline before indented block");
    require(TOKEN_INDENT,   "expected indented block");
    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        parse_had_error       = false;
        recovered_at_boundary = false;
        unsigned int saved       = c->count;             /* rollback point */
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        /* Roll back partial bytecode and skip to the next line/terminator — without this
           a parse error leaves the token unconsumed and the loop spins forever in
           MODE_SHELL (error_at() doesn't exit there). Skipped if a nested block already
           recovered to a boundary — see recovered_at_boundary's comment. */
        if (parse_had_error) {
            c->count           = saved;
            c->line_mark_count = saved_marks;
            if (!recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) &&
                       !equal(TOKEN_DEDENT)       && !equal(TOKEN_NEW_LINE))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    consume(TOKEN_DEDENT);
    /* However this loop exited (EOF/ELSE, or DEDENT just consumed), the lexer now sits at a fresh statement boundary regardless of recovery. */
    recovered_at_boundary = true;
}

/* ------------------------------------------------------------------ */
/* Control flow                                                         */
/* ------------------------------------------------------------------ */

static void parse_if(Chunk* c) {
    parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");

    chunk_emit(c, OP_JUMP_IF_FALSE);
    unsigned int patch_jif = c->count;
    chunk_emit(c, 0);

    compile_scope_depth++;
    chunk_emit(c, OP_PUSH_SCOPE);
    parse_compound(c);
    chunk_emit(c, OP_POP_SCOPE);
    compile_scope_depth--;

    if (consume(TOKEN_ELSE)) {
        /* The if-branch's parse_compound() just set recovered_at_boundary; that's stale
           now that consume() advanced past 'else' — a require(COLON) failure below is a fresh error, not a boundary. */
        recovered_at_boundary = false;
        require(TOKEN_COLON, "expected ':' after else");
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        c->code[patch_jif] = (int)c->count;
        compile_scope_depth++;
        chunk_emit(c, OP_PUSH_SCOPE);
        parse_compound(c);
        chunk_emit(c, OP_POP_SCOPE);
        compile_scope_depth--;
        c->code[patch_jmp] = (int)c->count;
    } else {
        c->code[patch_jif] = (int)c->count;
    }
}

/* Inline if-expression: if cond: value else: value  (no newline after ':') */
static void parse_if_expr(Chunk* c) {
    parse_binary(c, 0);          /* condition */
    require(TOKEN_COLON, "expected ':' after if condition");

    chunk_emit(c, OP_JUMP_IF_FALSE);
    unsigned int patch_jif = c->count;
    chunk_emit(c, 0);

    parse_binary(c, 0);          /* then-value */

    chunk_emit(c, OP_JUMP);
    unsigned int patch_jmp = c->count;
    chunk_emit(c, 0);
    c->code[patch_jif] = (int)c->count;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        parse_binary(c, 0);      /* else-value */
    } else {
        AerVal zero = aer_null();        /* no else: evaluates to null */
        chunk_emit(c, OP_PUSH);
        chunk_emit(c, (int)chunk_add_pool(c, zero));
    }

    c->code[patch_jmp] = (int)c->count;
}

/* Shared loop epilogue: emit backward jump, patch exit and break targets. */
static void finish_loop(Chunk* c, LoopContext* ctx, unsigned int loop_top, unsigned int patch_exit) {
    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    c->code[patch_exit] = (int)c->count;
    for (unsigned int i = 0; i < ctx->patch_count; i++)
        c->code[ctx->patches[i]] = (int)c->count;
    loop_depth--;
}

static void parse_for(Chunk* c) {
    if (loop_depth >= LOOP_MAX) { error_at("Too many nested loops"); return; }
    LoopContext* ctx = &loop_stack[loop_depth++];
    ctx->patch_count = 0;
    ctx->scope_depth = compile_scope_depth;
    ctx->iter_slots  = 0;

    if (equal(TOKEN_IN)) {
        error_at("Expected loop variable name before 'in'; use 'for x in collection:'");
        loop_depth--;
        return;
    }

    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name1 = chunk_add_pool(c, token.value);
        lex();

        unsigned int name2 = 0;
        bool is_pair = false;
        if (consume(TOKEN_COMMA)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected identifier after ','"); loop_depth--; return; }
            name2 = chunk_add_pool(c, token.value);
            lex();
            is_pair = true;
        }

        if (consume(TOKEN_IN)) {
            /* --- for x in expr:  /  for i in A..B[..step]:  /  for k, v in dict: --- */
            parse_binary(c, 0);   /* collection or range-start */
            bool is_range = consume(TOKEN_DOT_DOT);
            if (is_range) {
                if (is_pair) { error_at("Range does not support two loop variables"); loop_depth--; return; }
                parse_binary(c, 0);   /* end */
                /* Optional step: `for i in 0..10..2:`. Always push a step value (1 if
                   none written) so OP_ITER_RANGE's stack shape stays uniform; direction
                   is inferred from cur vs end, not the step's sign — see vm.c for why
                   (keeps AER's auto-reverse idiom instead of Python's empty-range footgun). */
                if (consume(TOKEN_DOT_DOT)) {
                    parse_binary(c, 0);   /* step — stack: [cur, end, step] */
                } else {
                    chunk_emit(c, OP_PUSH); chunk_emit(c, (int)chunk_add_pool(c, aer_int(1)));
                }
            } else {
                /* push index 0 — stack: [col, 0] */
                chunk_emit(c, OP_PUSH); chunk_emit(c, (int)chunk_add_pool(c, aer_int(0)));
            }
            require(TOKEN_COLON, "expected ':' after for clause");
            /* require() can't abort us on failure — without this, a malformed clause still gets a real (infinite-looping) back-edge compiled in. */
            if (parse_had_error) { loop_depth--; return; }

            ctx->iter_slots = is_range ? 3 : 2;
            ctx->top = c->count;

            if (is_range)     chunk_emit(c, OP_ITER_RANGE);
            else if (is_pair) chunk_emit(c, OP_ITER_NEXT_PAIR);
            else              chunk_emit(c, OP_ITER_NEXT);
            unsigned int patch_exit = c->count;
            chunk_emit(c, 0);

            if (is_pair) {
                emit_define(c, name2); /* value (TOS) */
                emit_define(c, name1); /* key         */
            } else {
                emit_define(c, name1);
            }

            compile_scope_depth++;
            chunk_emit(c, OP_PUSH_SCOPE);
            parse_compound(c);
            chunk_emit(c, OP_POP_SCOPE);
            compile_scope_depth--;

            finish_loop(c, ctx, ctx->top, patch_exit);
            return;
        }

        /* --- while loop whose condition starts with an identifier --- nothing emitted yet; record loop_top, then emit load + rest of expression. */
        ctx->top = c->count;
        emit_load(c, name1);
        /* The identifier may head a postfix chain (current.next, queue[i], f()), not just
           a bare name — mirrors parse_assignment's expression-statement fallback and
           parse_primary_inner's postfix loop. Without it, 'for current.next:' failed with
           a confusing "expected ':' after while condition" pointing at the '.'. */
        for (;;) {
            if (consume(TOKEN_OPEN_BRACKET)) {
                parse_index_or_slice(c);
            } else if (consume(TOKEN_OPEN_PARENTHESE)) {
                unsigned int n = parse_arg_list(c);
                if (pipe_forbid_calls) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
                chunk_emit(c, OP_CALL_VALUE);
                chunk_emit(c, (int)n);
            } else if (consume(TOKEN_DOT)) {
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
                unsigned int field_idx = chunk_add_pool(c, token.value);
                lex();
                chunk_emit(c, OP_FIELD_GET);
                chunk_emit(c, (int)field_idx);
            } else {
                break;
            }
        }
        parse_binary_ops(c, 0);
    } else {
        /* --- while loop, non-identifier condition --- */
        ctx->top = c->count;
        parse_binary(c, 0);
    }

    require(TOKEN_COLON, "expected ':' after while condition");
    /* Same check as the for-in clause above, same reason. */
    if (parse_had_error) { loop_depth--; return; }
    chunk_emit(c, OP_JUMP_IF_FALSE);
    unsigned int patch_jif = c->count;
    chunk_emit(c, 0);

    parse_compound(c);

    finish_loop(c, ctx, ctx->top, patch_jif);
}

/* ------------------------------------------------------------------ */
/* Break and continue                                                   */
/* ------------------------------------------------------------------ */

static void parse_break(Chunk* c) {
    if (loop_depth == loop_floor) { error_at("'break' outside loop"); return; }
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    int n = compile_scope_depth - ctx->scope_depth;
    for (int i = 0; i < n; i++) chunk_emit(c, OP_POP_SCOPE);
    /* Pop iterator state ([col, idx] or [cur, end, step]) that for-each leaves on the stack */
    for (int i = 0; i < ctx->iter_slots; i++) chunk_emit(c, OP_POP);
    chunk_emit(c, OP_JUMP);
    if (ctx->patch_count >= BREAK_MAX) { error_at("Too many breaks in one loop"); return; }
    ctx->patches[ctx->patch_count++] = c->count;
    chunk_emit(c, 0);  /* placeholder — patched by parse_loop */
}

static void parse_continue(Chunk* c) {
    if (loop_depth == loop_floor) { error_at("'continue' outside loop"); return; }
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    int n = compile_scope_depth - ctx->scope_depth;
    for (int i = 0; i < n; i++) chunk_emit(c, OP_POP_SCOPE);
    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)ctx->top);  /* known at compile time — no patching needed */
}

/* ------------------------------------------------------------------ */
/* Structs                                                              */
/* ------------------------------------------------------------------ */

/* Reads a literal (optionally negated numeric) value into *out and advances past it —
   struct field defaults must be compile-time constants, so this bypasses parse_binary/primary. */
static bool parse_literal_default(AerVal* out) {
    bool negative = false;
    if (consume(TOKEN_SUBTRACT)) negative = true;

    if (token.type == TOKEN_INTEGER) {
        long long n = aer_as_int(token.value);
        *out = aer_int(negative ? -n : n);
    } else if (token.type == TOKEN_REAL) {
        double d = aer_as_real(token.value);
        *out = aer_real(negative ? -d : d);
    } else if (negative) {
        return false;   /* '-' is only meaningful before a number */
    } else if (token.type == TOKEN_TRUE || token.type == TOKEN_FALSE) {
        *out = aer_bool(aer_as_bool(token.value));
    } else if (token.type == TOKEN_NULL) {
        *out = aer_null();
    } else if (token.type == TOKEN_STRING) {
        /* Copies rather than aliasing the token's AerString buffer — AerString always
           owns its data, and the token's cell plus this new one (kept via Shape.field_defaults)
           would otherwise share a buffer only one of them owns. */
        unsigned int len = aer_as_string(token.value)->length;
        char* buf = xmalloc(len + 1);
        memcpy(buf, aer_as_string(token.value)->data, len);
        buf[len] = '\0';
        *out = aer_make_string(buf, len);
    } else {
        return false;
    }
    lex();
    return true;
}

/* struct Point:
       x = 0.0
       y = 0.0
   One field per line; `= value` is optional (default null). Compiles to one
   OP_DEFINE_STRUCT that registers the shape at runtime, so — like functions —
   a struct must be declared before anything instantiates or type-checks it. */
static void parse_struct(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected struct name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_COLON, "expected ':' after struct name");
    require(TOKEN_NEW_LINE, "expected newline before indented struct body");
    require(TOKEN_INDENT,   "expected indented struct body");

    unsigned int field_names[MAX_STRUCT_FIELDS];
    AerVal       field_defaults[MAX_STRUCT_FIELDS];
    unsigned int field_count = 0;

    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (!equal(TOKEN_IDENTIFIER))    { error_at("Expected field name");      return; }
        if (field_count >= MAX_STRUCT_FIELDS) { error_at("Too many struct fields"); return; }

        unsigned int fname = chunk_add_pool(c, token.value);
        lex();
        AerVal dflt = aer_null();   /* null unless a default is given */
        if (consume(TOKEN_ASSIGN)) {
            if (!parse_literal_default(&dflt)) { error_at("Struct field defaults must be a literal value"); return; }
        }
        field_names[field_count]    = fname;
        field_defaults[field_count] = dflt;
        field_count++;

        if (!equal(TOKEN_DEDENT) && !equal(TOKEN_END_OF_FILE))
            require(TOKEN_NEW_LINE, "expected newline after struct field");
    }
    consume(TOKEN_DEDENT);

    if (field_count == 0) { error_at("Struct must have at least one field"); return; }

    chunk_emit(c, OP_DEFINE_STRUCT);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)field_count);
    for (unsigned int i = 0; i < field_count; i++) {
        chunk_emit(c, (int)field_names[i]);
        chunk_emit(c, (int)chunk_add_pool(c, field_defaults[i]));
    }
}

/* ------------------------------------------------------------------ */
/* Modules                                                              */
/* ------------------------------------------------------------------ */

static void parse_import(Chunk* c) {
    /* Top-level only — a nested import would need saving/restoring the whole parser
       context (loop_depth, function_depth, capture_ctx...), not just the lexer's, for no real benefit. */
    if (compile_scope_depth != 0) {
        error_at("'import' is only allowed at the top level of a file, not inside a function, loop, or if block");
        return;
    }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a module name after 'import'"); return; }

    /* A dotted path is a directory hint only — "sub.mid" resolves "sub/mid.aer" but still binds as "mid". */
    char path_buf[256];
    unsigned int path_len = 0, bind_start = 0, bind_len = 0;
    while (true) {
        const char* seg     = aer_as_string(token.value)->data;
        unsigned int seg_len = aer_as_string(token.value)->length;
        if (path_len > 0) path_buf[path_len++] = '/';
        if (path_len + seg_len >= sizeof(path_buf)) { error_at("Import path too long"); return; }
        bind_start = path_len;
        memcpy(path_buf + path_len, seg, seg_len);
        path_len += seg_len;
        bind_len = seg_len;
        lex();
        if (!consume(TOKEN_DOT)) break;
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a module name segment after '.'"); return; }
    }
    chunk_add_import(c, path_buf + bind_start, bind_len, path_buf, path_len);
}

/* True if the current (not yet consumed) token names a module imported earlier —
   checked before the normal identifier/assignment path, in both parse_statement and parse_primary_inner. */
static bool at_module_name(Chunk* c) {
    return token.type == TOKEN_IDENTIFIER &&
           chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length);
}

/* Parses `module.function(args)` (caller must have verified at_module_name(c)); "module" isn't a real scope variable, so this emits OP_CALL_MODULE directly. */
static void parse_module_call(Chunk* c) {
    unsigned int module_idx = chunk_add_pool(c, token.value);
    lex();
    if (!consume(TOKEN_DOT)) {
        error_at("A module can't be used as a value on its own — call a function on it, e.g. 'module.function(...)'");
        return;
    }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '.'"); return; }
    unsigned int fn_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after module function name");
    unsigned int n = parse_arg_list(c);
    if (pipe_forbid_calls) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
    chunk_emit(c, OP_CALL_MODULE);
    chunk_emit(c, (int)module_idx);
    chunk_emit(c, (int)fn_idx);
    chunk_emit(c, (int)n);
}

/* ------------------------------------------------------------------ */
/* Functions                                                            */
/* ------------------------------------------------------------------ */

static unsigned int parse_arg_list(Chunk* c) {
    unsigned int n = 0;
    if (!equal(TOKEN_CLOSE_PARENTHESE))
        do { parse_binary(c, 0); n++; } while (consume(TOKEN_COMMA));
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after arguments");
    return n;
}

static void parse_call_args(Chunk* c, unsigned int name_idx) {
    unsigned int n = parse_arg_list(c);
    if (pipe_forbid_calls) { error_at("Function calls are not allowed inside a pipe's arguments"); return; }
    chunk_emit(c, OP_CALL);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)n);
    chunk_emit(c, (int)chunk_add_addr_cache(c));
    last_bare_call_end = c->count;   /* enables tail-call detection in parse_return, see there */
}

/* Shared core of parse_function/parse_function_expr: parses '(' params ')' ':' body and
   leaves a TYPE_FUNCTION Value pushed as the last thing emitted. Callers handle what
   comes before (a name, for a declaration) and after (an OP_STORE, or nothing for an expression). */
typedef struct {
    unsigned int func_start;
    unsigned int arity;
    unsigned int min_arity;                        /* params [0, min_arity) are required */
    unsigned int default_pool_idx[MAX_PARAMS];      /* valid for indices [min_arity, arity) */
    bool         has_receiver;
    unsigned int receiver_type;
} FunctionSig;

static FunctionSig parse_function_body(Chunk* c) {
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");

    unsigned int params[MAX_PARAMS], arity = 0;
    unsigned int default_pool_idx[MAX_PARAMS];
    unsigned int min_arity = 0;
    bool seen_default = false;
    bool has_receiver = false;
    unsigned int receiver_type = 0;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected parameter name"); return (FunctionSig){0}; }
            if (arity >= MAX_PARAMS)      { error_at("Too many parameters");      return (FunctionSig){0}; }
            params[arity] = chunk_add_pool(c, token.value);
            lex();
            /* `p as Type` — only meaningful on parameter 0 (the receiver), enforced at
               call time against the argument's struct shape. Always consumed here (not
               just when arity==0) so a later annotation gets one clear error instead of
               desyncing the parser into cascading, unrelated-looking errors. */
            if (consume(TOKEN_AS)) {
                if (arity != 0) {
                    error_at("A struct-type parameter ('as Type') is only allowed on the first parameter");
                    return (FunctionSig){0};
                }
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a struct type name after 'as'"); return (FunctionSig){0}; }
                receiver_type = chunk_add_pool(c, token.value);
                has_receiver  = true;
                lex();
            }
            /* `= <literal>` — same restriction as struct field defaults, and must be trailing. */
            if (consume(TOKEN_ASSIGN)) {
                AerVal dflt;
                if (!parse_literal_default(&dflt)) { error_at("Parameter defaults must be a literal value"); return (FunctionSig){0}; }
                default_pool_idx[arity] = chunk_add_pool(c, dflt);
                seen_default = true;
            } else if (seen_default) {
                error_at("A parameter without a default cannot follow one that has a default");
                return (FunctionSig){0};
            } else {
                min_arity++;
            }
            arity++;
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after parameters");
    require(TOKEN_COLON,            "expected ':' after function signature");

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);

    unsigned int func_start = c->count;

    /* Only a genuine 0 -> 1 transition starts a fresh outer_locals window — a nested closure's body (1 -> 2) must see the enclosing function's accumulated set, not a cleared one. */
    if (function_depth == 0) outer_local_count = 0;

    /* Isolate break/continue and return from outer context */
    int saved_loop_depth = loop_depth;
    int saved_loop_floor = loop_floor;
    loop_floor = loop_depth;
    function_depth++;

    /* current_params tracks the currently-compiling function's own parameters (see its
       declaration) — a nested closure gets a fresh table here, with the enclosing
       function's restored after, the same save/restore pattern as capturing/capture_ctx. */
    unsigned int saved_params[MAX_PARAMS];
    unsigned int saved_param_count = current_param_count;
    memcpy(saved_params, current_params, sizeof(unsigned int) * current_param_count);
    current_param_count = 0;

    compile_scope_depth++;
    chunk_emit(c, OP_PUSH_SCOPE);
    for (int i = (int)arity - 1; i >= 0; i--) {
        emit_define_param(c, params[i]);
    }

    parse_compound(c);
    compile_scope_depth--;

    /* Implicit void return if control falls off the end */
    AerVal zero = aer_null();
    chunk_emit(c, OP_PUSH);
    chunk_emit(c, (int)chunk_add_pool(c, zero));
    chunk_emit(c, OP_RETURN);

    current_param_count = saved_param_count;
    memcpy(current_params, saved_params, sizeof(unsigned int) * saved_param_count);

    function_depth--;
    loop_depth = saved_loop_depth;
    loop_floor = saved_loop_floor;

    c->code[patch] = (int)c->count;

    FunctionSig sig;
    sig.func_start     = func_start;
    sig.arity          = arity;
    sig.min_arity      = min_arity;
    memcpy(sig.default_pool_idx, default_pool_idx, sizeof(unsigned int) * MAX_PARAMS);
    sig.has_receiver   = has_receiver;
    sig.receiver_type  = receiver_type;
    return sig;
}

/* Emits the "plain" (non-capturing) function value: a precomputed pool constant, identical across calls to this function_stmt/function_expr — the pre-closures bytecode shape. */
static void emit_plain_function_value(Chunk* c, FunctionSig sig) {
    /* vm_new_function(), not raw xcalloc — every AerFunction must come from function_pool
       (see vm.h) so the GC's mark phase can walk it once in Chunk.pool. Zeroed by hand
       since pool_alloc returns uninitialized memory, unlike the xcalloc this replaced. */
    AerFunction* fn = vm_new_function();
    fn->upvalues      = NULL;
    fn->upvalue_count = 0;
    fn->code_offset   = sig.func_start;
    fn->arity         = sig.arity;
    fn->min_arity     = sig.min_arity;
    unsigned int default_count = sig.arity - sig.min_arity;
    if (default_count > 0) {
        fn->defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++)
            fn->defaults[i] = c->pool[sig.default_pool_idx[sig.min_arity + i]];
    } else {
        fn->defaults = NULL;
    }
    fn->has_receiver  = sig.has_receiver;
    fn->receiver_type = sig.receiver_type;
    chunk_emit(c, OP_PUSH);
    chunk_emit(c, (int)chunk_add_pool(c, aer_function_val(fn)));
}

static void parse_function(Chunk* c) {
    /* Named functions can't nest — no capture mechanism backs them (that's what anonymous
       functions are for). Parsing still runs normally so a malformed body doesn't cascade
       into confusing errors; parse_had_error is re-forced true below to roll back the definition. */
    bool is_nested = function_depth > 0;
    if (is_nested) {
        error_at("Nested named function definitions are not supported — define an anonymous "
                 "function instead (f = function(...): ...), or define this at the top level.");
    }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected function name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    /* A rejected nested function might still sit inside a capturing closure's body, so
       capturing is suspended regardless of is_nested — otherwise its reads/writes would pollute the enclosing capture_ctx with entries that outlive the rollback. */
    bool       saved_capturing = capturing;
    CaptureCtx saved_ctx       = capture_ctx;
    capturing = false;

    FunctionSig sig = parse_function_body(c);

    capturing   = saved_capturing;
    capture_ctx = saved_ctx;

    /* Re-assert after body parsing: parse_compound() resets parse_had_error per statement, which would otherwise erase the flag before the caller rolls bytecode back. */
    if (is_nested) { parse_had_error = true; return; }

    emit_plain_function_value(c, sig);
    chunk_emit(c, OP_STORE);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)chunk_add_addr_cache(c));
}

/* Anonymous function expression: function(params): body, used as a value. Unlike
   parse_function, this is allowed to nest; capture_supported is false past one nesting level (see the capture-analysis comment above). */
static void parse_function_expr(Chunk* c) {
    bool capture_supported = (function_depth == 1);

    /* Unconditional save/restore: a function expression nested in an already-capturing one must have capturing suspended for its own body, not inherit the outer context's true. */
    bool        saved_capturing = capturing;
    CaptureCtx  saved_ctx       = capture_ctx;
    capturing = capture_supported;
    if (capture_supported) {
        capture_ctx.local_count   = 0;
        capture_ctx.capture_count = 0;
    }

    FunctionSig sig = parse_function_body(c);

    if (capture_supported && capture_ctx.capture_count > 0) {
        for (unsigned int i = 0; i < capture_ctx.capture_count; i++) {
            chunk_emit(c, OP_CAPTURE);
            chunk_emit(c, (int)capture_ctx.captured_names[i]);
        }
        chunk_emit(c, OP_MAKE_CLOSURE);
        chunk_emit(c, (int)sig.func_start);
        chunk_emit(c, (int)sig.arity);
        chunk_emit(c, (int)sig.min_arity);
        chunk_emit(c, sig.has_receiver ? 1 : 0);
        chunk_emit(c, (int)sig.receiver_type);
        chunk_emit(c, (int)capture_ctx.capture_count);
        for (unsigned int i = sig.min_arity; i < sig.arity; i++)
            chunk_emit(c, (int)sig.default_pool_idx[i]);
    } else {
        emit_plain_function_value(c, sig);
    }

    capturing   = saved_capturing;
    capture_ctx = saved_ctx;
}

/* defer name(args) — args are evaluated now at the defer statement (via parse_arg_list,
   like any normal call), but the call itself is stashed on the current frame and replayed
   LIFO by lbl_return (vm.c). Only a bare name is supported as target (not a module call
   or arbitrary expression), matching OP_DEFER_PUSH's shape (name idx + arg count) to OP_CALL. */
static void parse_defer(Chunk* c) {
    if (function_depth == 0) { error_at("'defer' outside function"); return; }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after 'defer'"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after deferred function name");
    unsigned int n = parse_arg_list(c);
    if (n > MAX_DEFER_ARGS) {
        error_at("Too many arguments to a deferred call (max %d)", MAX_DEFER_ARGS);
        return;
    }
    chunk_emit(c, OP_DEFER_PUSH);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)n);
}

static void parse_return(Chunk* c) {
    if (function_depth == 0) { error_at("'return' outside function"); return; }
    if (!equal(TOKEN_NEW_LINE) && !equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        parse_binary(c, 0);
        /* return a, b → return [a, b] */
        unsigned int count = 1;
        while (consume(TOKEN_COMMA)) { parse_binary(c, 0); count++; }
        if (count > 1) {
            chunk_emit(c, OP_ARRAY_NEW); chunk_emit(c, (int)count);
        } else if (last_bare_call_end == c->count) {
            /* parse_binary's entire output was exactly one bare call — true tail position.
               Patch OP_CALL to OP_TAIL_CALL in place (same operand shape, same lbl_call
               handler, branching only at frame-push). 4 words back, not 3 — see the
               cache-slot operand chunk_add_addr_cache adds in parse_call_args. */
            c->code[c->count - 4] = OP_TAIL_CALL;
        }
    } else {
        AerVal zero = aer_null();
        chunk_emit(c, OP_PUSH);
        chunk_emit(c, (int)chunk_add_pool(c, zero));
    }
    chunk_emit(c, OP_RETURN);
}
