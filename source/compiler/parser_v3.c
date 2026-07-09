#include "parser_v3.h"
#include <string.h>
#include "error.h"
#include "lexer.h"

/* See parser_v3.h's comment — this whole file is M1's standalone prototype, not reachable from
   real .aer source. current_v3_watermark tracks the next free temp register; registers below
   reserved_floor are "locals" a test has already set up (via OP_V3_LOADK) and must never be
   handed out or freed here. */
static int v3_next_temp_register = 0;
static int v3_reserved_floor     = 0;

void v3_reg_reset(void) {
    v3_next_temp_register = 0;
    v3_reserved_floor     = 0;
}

void v3_reg_reserve(int count) {
    v3_reserved_floor     += count;
    v3_next_temp_register += count;
}

int v3_reg_alloc(void) {
    return v3_next_temp_register++;
}

void v3_reg_free(int count) {
    v3_next_temp_register -= count;
    /* Never free below the reserved floor — a bug in a future caller shouldn't be able to hand
       out a "local"'s register as if it were a free temp. */
    if (v3_next_temp_register < v3_reserved_floor) v3_next_temp_register = v3_reserved_floor;
}

int v3_compile_node(Chunk* c, V3Node* node) {
    if (node->kind == V3_NODE_CONST) {
        unsigned int pool_idx = chunk_add_pool(c, node->const_value);
        return (int)pool_idx | V3_RK_CONST_FLAG;
    }
    if (node->kind == V3_NODE_REG) {
        return node->reg;   /* already live — an RK "register" operand, not const-flagged */
    }

    int rk_lhs = v3_compile_node(c, node->lhs);
    int rk_rhs = v3_compile_node(c, node->rhs);

    /* Free operand registers (if this node allocated them — a CONST or an already-reserved REG
       leaf never does) BEFORE allocating the result's register, so the result reuses the lowest
       just-freed slot instead of growing the watermark further. Exactly Lua's own free-then-
       allocate discipline (lcode.c) — this is what keeps register usage compact across a deep
       expression tree instead of growing linearly with tree size. Only free slots THIS call
       allocated: a V3_NODE_REG leaf's register belongs to whatever reserved it, not to us. */
    if (!(rk_rhs & V3_RK_CONST_FLAG) && node->rhs->kind == V3_NODE_BINARY) v3_reg_free(1);
    if (!(rk_lhs & V3_RK_CONST_FLAG) && node->lhs->kind == V3_NODE_BINARY) v3_reg_free(1);

    int dest = v3_reg_alloc();
    chunk_emit(c, OP_V3_BINARY);
    chunk_emit(c, dest);
    chunk_emit(c, rk_lhs);
    chunk_emit(c, (int)node->bin_op);
    chunk_emit(c, rk_rhs);
    return dest;
}

unsigned int v3_emit_cmp_jump_false(Chunk* c, int rk_a, Opcode cmp_op, int rk_b) {
    chunk_emit(c, OP_V3_CMP_JUMP_FALSE);
    chunk_emit(c, rk_a);
    chunk_emit(c, (int)cmp_op);
    chunk_emit(c, rk_b);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by v3_patch_jump once the target is known */
    return patch_offset;
}

unsigned int v3_emit_jump_if_false_reg(Chunk* c, int reg) {
    chunk_emit(c, OP_V3_JUMP_IF_FALSE_REG);
    chunk_emit(c, reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);
    return patch_offset;
}

void v3_patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (int)target;
}

void v3_emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count) {
    chunk_emit(c, OP_V3_CALL);
    chunk_emit(c, dest_reg);
    chunk_emit(c, (int)callee_offset);
    chunk_emit(c, arg_reg_base);
    chunk_emit(c, arg_count);
}

void v3_emit_return(Chunk* c, int src_reg) {
    chunk_emit(c, OP_V3_RETURN);
    chunk_emit(c, src_reg);
}

void v3_emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count) {
    chunk_emit(c, OP_V3_ARRAY_NEW);
    chunk_emit(c, dest_reg);
    chunk_emit(c, item_reg_base);
    chunk_emit(c, item_count);
}

void v3_emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx) {
    chunk_emit(c, OP_V3_INDEX_GET);
    chunk_emit(c, dest_reg);
    chunk_emit(c, arr_reg);
    chunk_emit(c, rk_idx);
}

void v3_emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val) {
    chunk_emit(c, OP_V3_INDEX_SET);
    chunk_emit(c, arr_reg);
    chunk_emit(c, rk_idx);
    chunk_emit(c, rk_val);
}

void v3_emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count) {
    chunk_emit(c, OP_V3_DICT_NEW);
    chunk_emit(c, dest_reg);
    chunk_emit(c, pair_reg_base);
    chunk_emit(c, pair_count);
}

unsigned int v3_emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg) {
    chunk_emit(c, OP_V3_ITER_NEXT_ARRAY);
    chunk_emit(c, col_reg);
    chunk_emit(c, idx_reg);
    chunk_emit(c, item_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by v3_patch_jump once the loop-exit target is known */
    return patch_offset;
}

void v3_emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base, int arg_count) {
    chunk_emit(c, OP_V3_STRUCT_NEW);
    chunk_emit(c, dest_reg);
    chunk_emit(c, (int)type_name_pool_idx);
    chunk_emit(c, arg_reg_base);
    chunk_emit(c, arg_count);
}

void v3_emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx) {
    chunk_emit(c, OP_V3_FIELD_GET);
    chunk_emit(c, dest_reg);
    chunk_emit(c, struct_reg);
    chunk_emit(c, (int)field_name_pool_idx);
}

void v3_emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val) {
    chunk_emit(c, OP_V3_FIELD_SET);
    chunk_emit(c, struct_reg);
    chunk_emit(c, (int)field_name_pool_idx);
    chunk_emit(c, rk_val);
}

/* ------------------------------------------------------------------ */
/* M5 — real .aer source wiring, built up slice by slice (variables/arithmetic/if/for-while;
   functions/calls incl. recursion; array/dict literals + indexing; `for x in y:` array
   iteration — plain string literals landed alongside containers, needed to test dict keys).
   An entirely separate recursive-descent compiler from parser.c, sharing only the real lexer's
   token stream (lex()/token/consume()/equal()/require(), lexer.h) — not a shared/refactored
   grammar, so this can't destabilize the production compiler while v3 remains unproven. See the
   plan file for the full roadmap and what's still deferred (structs, compound assignment, unary
   operators, and/or, string interpolation, dict-key/pair + range iteration, imports, defer,
   modules — none of that is reachable from real source through this entry point yet). */
/* ------------------------------------------------------------------ */

static int  v3_parse_primary_inner(Chunk* c);
static int  v3_parse_primary(Chunk* c);
static int  v3_parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs);
static int  v3_parse_binary(Chunk* c, unsigned int min_prec);
static void v3_parse_statement(Chunk* c);
static void v3_parse_block(Chunk* c);
static void v3_parse_if(Chunk* c);
static void v3_parse_for_while(Chunk* c);
static void v3_parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond);
static void v3_parse_assignment(Chunk* c, unsigned int name_idx);
static void v3_parse_index_assignment(Chunk* c, unsigned int name_idx);
static void v3_parse_field_assignment(Chunk* c, unsigned int name_idx);
static int  v3_parse_call(Chunk* c, unsigned int name_idx);
static void v3_parse_function(Chunk* c);
static void v3_parse_return(Chunk* c);
static void v3_parse_struct(Chunk* c);
static int  v3_parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base);

/* name_idx -> register, in encounter order: v3_var_names[i]'s register is always exactly i (see
   v3_var_slot below for why). No global/local distinction (unlike parser.c's current_locals) —
   this slice has no function defs, so every register in the one frame a script runs in is
   effectively local. */
static unsigned int v3_var_names[V3_FRAME_REGISTERS];
static int          v3_var_count = 0;

/* Mirrors current_local_slot_or_alloc's exact shape (parser.c:114-124). Register i is always
   v3_var_count's value at the moment name i was first seen, because v3_reg_reserve(count) always
   reserves the *next* `count` registers — so incrementing v3_var_count and reserving 1 register in
   the same call keeps them in lockstep, with no separate slot-number bookkeeping needed.
     Known, honest inefficiency (not a correctness issue): if the right-hand side of this
   variable's first assignment itself used temp registers, v3_reg_reserve's unconditional bump can
   skip past those temps' current watermark, permanently wasting a register or two per assignment
   statement (the temp-vs-permanent numbering shares one space here, unlike parser.c's local slots
   and vm->stack being entirely separate address spaces). Harmless at V3_FRAME_REGISTERS=32 for
   this slice's hand-fed test scripts. */
static int v3_var_slot(unsigned int name_idx) {
    for (int i = 0; i < v3_var_count; i++)
        if (v3_var_names[i] == name_idx) return i;
    if (v3_var_count >= V3_FRAME_REGISTERS) {
        error_at("Too many variables for the v3 prototype (max %d)", V3_FRAME_REGISTERS);
        return -1;
    }
    int reg = v3_var_count;
    v3_var_names[v3_var_count++] = name_idx;
    v3_reg_reserve(1);   /* permanently protects this register from the temp allocator */
    return reg;
}

/* Register-value equivalent of v3_compile_node's "was this a V3_NODE_BINARY" check (only
   meaningful for a hand-built tree) — works for any RK operand since a temp always lives at/above
   the reserved floor, a permanent variable register always below it. static int
   v3_reserved_floor's declaration (top of this file) makes this valid here. */
static bool v3_is_temp(int rk) {
    return !(rk & V3_RK_CONST_FLAG) && rk >= v3_reserved_floor;
}

/* Ensures rk is a plain register (not RK-const), materializing a constant into a fresh temp via
   OP_V3_LOADK if needed — OP_V3_JUMP_IF_FALSE_REG needs an actual register operand, no RK form. */
static int v3_materialize(Chunk* c, int rk) {
    if (!(rk & V3_RK_CONST_FLAG)) return rk;
    int reg = v3_reg_alloc();
    chunk_emit(c, OP_V3_LOADK);
    chunk_emit(c, reg);
    chunk_emit(c, rk & ~V3_RK_CONST_FLAG);
    return reg;
}

/* M5 slice 3 — function name -> instruction offset, separate from v3_var_names (a function needs
   a jump target, not a register). Functions are NOT first-class values in this slice (no `f =
   square; f(5)`, no passing a function as an argument) — a real fix needs OP_V3_CALL's value-based
   counterpart (like the stack VM's OP_CALL_VALUE), out of scope here. */
#define V3_FUNC_MAX 16
static unsigned int v3_func_names[V3_FUNC_MAX];
static unsigned int v3_func_offsets[V3_FUNC_MAX];
static int          v3_func_count = 0;

/* Nonzero while compiling a function body — lets v3_parse_return reject a top-level `return`,
   mirroring parser.c's own function_depth check (parser.c:1727). No nesting to track (named
   functions can't nest — matches the real language's own restriction, parser.c:1673-1682), so a
   plain counter (0 or 1) is enough, not a stack. */
static int v3_function_depth = 0;

static bool v3_func_lookup(unsigned int name_idx, unsigned int* out_offset) {
    for (int i = 0; i < v3_func_count; i++)
        if (v3_func_names[i] == name_idx) { *out_offset = v3_func_offsets[i]; return true; }
    return false;
}

/* M5 slice 6 — struct type name registry, separate from v3_func_names (a struct type isn't a
   callable offset — it resolves at runtime via chunk_find_shape(), OP_V3_STRUCT_NEW just needs to
   know AT COMPILE TIME that `Name(...)` means "construct", not "call", mirroring the stack VM's
   own OP_CALL-does-double-duty precedent one level up, at the v3_parse_call dispatch point rather
   than inside a single runtime opcode). */
#define V3_STRUCT_MAX 16
static unsigned int v3_struct_names[V3_STRUCT_MAX];
static int          v3_struct_count = 0;

static bool v3_is_struct_name(unsigned int name_idx) {
    for (int i = 0; i < v3_struct_count; i++)
        if (v3_struct_names[i] == name_idx) return true;
    return false;
}

/* Forces rk into exactly the CURRENT allocator watermark, needed because OP_V3_CALL's arguments
   must land in contiguous registers (its bulk-copy reads v3_registers[arg_reg_base..+arg_count)).
   The common case is a no-op: v3_parse_binary's own free-then-allocate discipline already leaves
   a freshly-computed temp sitting at exactly the watermark, so `rk == target` already holds
   whenever rk came from real computation — the copy only actually fires for a bare constant or a
   permanent variable register, neither of which is part of the temp-watermark sequence. */
static int v3_arg_materialize(Chunk* c, int rk) {
    int target = v3_reg_alloc();
    if (rk & V3_RK_CONST_FLAG) {
        chunk_emit(c, OP_V3_LOADK);
        chunk_emit(c, target);
        chunk_emit(c, rk & ~V3_RK_CONST_FLAG);
    } else if (rk != target) {
        chunk_emit(c, OP_V3_MOVE);
        chunk_emit(c, target);
        chunk_emit(c, rk);
    }
    return target;
}

/* M5 slice 4 — shared by call arguments, array literals, and (per-key/value) dict literals: all
   three bulk-copy opcodes (OP_V3_CALL/OP_V3_ARRAY_NEW/OP_V3_DICT_NEW) need their operands in
   contiguous registers. Parses a comma-separated expression list up to (not including) close_tok
   — the caller still requires() close_tok itself, since a dict's pairs need extra ':' handling
   this bare list can't express. Returns the count; *out_base is unspecified when count==0. */
static int v3_parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base) {
    int base  = -1;
    int count = 0;
    if (!equal(close_tok)) {
        do {
            int rk  = v3_parse_binary(c, 0);
            int reg = v3_arg_materialize(c, rk);
            if (count == 0) base = reg;
            count++;
        } while (consume(TOKEN_COMMA));
    }
    *out_base = base;
    return count;
}

/* M5 slice 4 — plain string literals only (no `"...{name}..."` interpolation: that's a real,
   separate parser.c feature, out of scope here). Mirrors parser.c's own pool_escaped_string
   (parser.c:826-854) exactly for escape-sequence handling; duplicated rather than shared since
   that helper is static to parser.c and this file deliberately shares no code with it. Needed now
   (not deferred to a later "strings" slice) because dict literals' keys must be strings — there's
   no way to test M5 slice 4's dict-literal support from real source without it. */
static unsigned int v3_pool_escaped_string(Chunk* c, const char* s, unsigned int len) {
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
                default:   buf[out++] = '\\'; buf[out++] = s[i]; break;
            }
        } else {
            buf[out++] = s[i];
        }
    }
    char* owned = xmalloc((size_t)out + 1);
    memcpy(owned, buf, out);
    owned[out] = '\0';
    AerVal sv = aer_make_string(owned, out);
    return chunk_add_pool(c, sv);
}

/* Subset of parser.c's binary_precedence[]/binary_ops[] table (parser.c:10-30) — comparisons then
   arithmetic only, same relative precedence numbers. No and/or (need short-circuit jump patching),
   no in/bitwise/shift/pipe/as (need containers/modules not built for v3 yet). Returns false for
   any operator outside this subset. */
static bool v3_binary_op_info(TokenType t, unsigned int* prec, Opcode* op) {
    switch (t) {
        case TOKEN_EQUAL:         *prec = 6;  *op = OP_EQ;        return true;
        case TOKEN_NOT_EQUAL:     *prec = 6;  *op = OP_NEQ;       return true;
        case TOKEN_LESS:          *prec = 7;  *op = OP_LT;        return true;
        case TOKEN_GREATER:       *prec = 7;  *op = OP_GT;        return true;
        case TOKEN_LESS_EQUAL:    *prec = 7;  *op = OP_LTE;       return true;
        case TOKEN_GREATER_EQUAL: *prec = 7;  *op = OP_GTE;       return true;
        case TOKEN_ADD:           *prec = 9;  *op = OP_ADD;       return true;
        case TOKEN_SUBTRACT:      *prec = 9;  *op = OP_SUB;       return true;
        case TOKEN_MULTIPLY:      *prec = 10; *op = OP_MUL;       return true;
        case TOKEN_DIVIDE:        *prec = 10; *op = OP_DIV;       return true;
        case TOKEN_MODULO:        *prec = 10; *op = OP_MOD;       return true;
        case TOKEN_FLOOR_DIVIDE:  *prec = 10; *op = OP_FLOOR_DIV; return true;
        default: return false;
    }
}

/* Literal/identifier/parenthesized/array-literal/dict-literal/call primary — no unary operators
   (`-x`/`!x`/`~x`): v3 has no unary opcode at all yet, only OP_V3_BINARY, and adding one is out of
   scope for this slice. */
static int v3_parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        int rk = v3_parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return rk;
    }
    /* M5 slice 4 — array literal. Elements share the same contiguous-register-materialization
       helper call arguments use (v3_parse_contiguous_exprs), then OP_V3_ARRAY_NEW (M4) — the
       result reuses the first element's register (freeing the rest), same compact-reuse
       convention as a call's result. */
    if (consume(TOKEN_OPEN_BRACKET)) {
        int item_reg_base;
        int item_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_BRACKET, &item_reg_base);
        require(TOKEN_CLOSE_BRACKET, "expected ']' after array literal");
        if (parse_had_error) return 0;
        int dest = (item_count > 0) ? item_reg_base : v3_reg_alloc();
        if (item_count > 1) v3_reg_free(item_count - 1);
        v3_emit_array_new(c, dest, item_reg_base < 0 ? dest : item_reg_base, item_count);
        return dest;
    }
    /* Dict literal — not a bare comma list (each item is a key:value pair), so this doesn't reuse
       v3_parse_contiguous_exprs; otherwise the exact same contiguous-materialize-then-bulk-copy
       shape, now via OP_V3_DICT_NEW (M4). Key then value are materialized back to back, so
       pair i's key/value land at exactly pair_reg_base+2i/+2i+1, matching that opcode's layout. */
    if (consume(TOKEN_OPEN_BRACE)) {
        int pair_reg_base = -1;
        int pair_count = 0;
        if (!equal(TOKEN_CLOSE_BRACE)) {
            do {
                int rk_key  = v3_parse_binary(c, 0);
                int reg_key = v3_arg_materialize(c, rk_key);
                if (pair_count == 0) pair_reg_base = reg_key;
                require(TOKEN_COLON, "expected ':' after dict key");
                if (parse_had_error) return 0;
                int rk_val = v3_parse_binary(c, 0);
                v3_arg_materialize(c, rk_val);
                pair_count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACE, "expected '}' after dict literal");
        if (parse_had_error) return 0;
        int dest = (pair_count > 0) ? pair_reg_base : v3_reg_alloc();
        if (pair_count > 1) v3_reg_free(2 * pair_count - 1);
        v3_emit_dict_new(c, dest, pair_reg_base < 0 ? dest : pair_reg_base, pair_count);
        return dest;
    }
    if (token.type == TOKEN_INTEGER || token.type == TOKEN_REAL ||
        token.type == TOKEN_TRUE    || token.type == TOKEN_FALSE || token.type == TOKEN_NULL) {
        AerVal v;
        if      (token.type == TOKEN_INTEGER) v = aer_int(aer_as_int(token.value));
        else if (token.type == TOKEN_REAL)    v = aer_real(aer_as_real(token.value));
        else if (token.type == TOKEN_NULL)    v = aer_null();
        else                                   v = aer_bool(aer_as_bool(token.value));
        lex();
        return (int)chunk_add_pool(c, v) | V3_RK_CONST_FLAG;
    }
    /* Plain string literal (no interpolation — see v3_pool_escaped_string's own comment). */
    if (token.type == TOKEN_STRING) {
        AerString* ts = aer_as_string(token.value);
        unsigned int pool_idx = v3_pool_escaped_string(c, ts->data, ts->length);
        lex();
        return (int)pool_idx | V3_RK_CONST_FLAG;
    }
    if (token.type == TOKEN_IDENTIFIER) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) return v3_parse_call(c, name_idx);
        int reg = v3_var_slot(name_idx);
        return reg < 0 ? 0 : reg;   /* reg<0: error_at already called */
    }
    error_at("Expected an expression (v3 prototype: only literals, variables, calls, array/dict literals, arithmetic/comparisons, and parentheses)");
    return 0;
}

/* M5 slice 4 — postfix `[index]` chain (arbitrarily deep reads: `matrix[i][j]` works for free,
   since each step's result just feeds back into the next). Only a single-level WRITE
   (`arr[i] = v`) is supported, dispatched separately from v3_parse_statement — chained/nested
   index writes need the same GET-until-the-last-step-then-SET shape parser.c's parse_assignment
   has (parser.c:352-398), out of scope for this slice. */
static int v3_parse_primary(Chunk* c) {
    int rk = v3_parse_primary_inner(c);
    for (;;) {
        if (consume(TOKEN_OPEN_BRACKET)) {
            int rk_idx = v3_parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
            if (parse_had_error) return rk;

            int arr_reg = v3_materialize(c, rk);

            /* Free-then-allocate, matching v3_parse_binary_ops's own discipline. */
            if (v3_is_temp(rk_idx))  v3_reg_free(1);
            if (v3_is_temp(arr_reg)) v3_reg_free(1);

            int dest = v3_reg_alloc();
            v3_emit_index_get(c, dest, arr_reg, rk_idx);
            rk = dest;
            continue;
        }
        /* M5 slice 6 — postfix '.field' read, same shape as '[...]' above: p.x and chained forms
           like p.pos.x work for free, since each step's result feeds back into the next. */
        if (consume(TOKEN_DOT)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return rk; }
            unsigned int field_idx = chunk_add_pool(c, token.value);
            lex();

            int struct_reg = v3_materialize(c, rk);
            if (v3_is_temp(struct_reg)) v3_reg_free(1);

            int dest = v3_reg_alloc();
            v3_emit_field_get(c, dest, struct_reg, field_idx);
            rk = dest;
            continue;
        }
        break;
    }
    return rk;
}

/* Precedence-climbing loop, `lhs` already parsed — mirrors parser.c's parse_binary_ops/parse_binary
   split (parser.c:673-810) exactly, so a for-while condition that starts with an identifier can
   resolve that identifier once (see v3_parse_for_while) and climb from there, same as parser.c's
   own for-loop does for its while-fallback form. */
static int v3_parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs) {
    for (;;) {
        unsigned int prec;
        Opcode op;
        if (!v3_binary_op_info(token.type, &prec, &op) || prec <= min_prec) break;
        lex();
        int rhs = v3_parse_binary(c, prec);   /* same precedence as floor -> left-associative */

        /* Free-then-allocate, RHS then LHS, matching v3_compile_node's own discipline exactly. */
        if (v3_is_temp(rhs)) v3_reg_free(1);
        if (v3_is_temp(lhs)) v3_reg_free(1);

        int dest = v3_reg_alloc();
        chunk_emit(c, OP_V3_BINARY);
        chunk_emit(c, dest);
        chunk_emit(c, lhs);
        chunk_emit(c, (int)op);
        chunk_emit(c, rhs);
        lhs = dest;
    }
    return lhs;
}

static int v3_parse_binary(Chunk* c, unsigned int min_prec) {
    int lhs = v3_parse_primary(c);
    return v3_parse_binary_ops(c, min_prec, lhs);
}

/* `name = expr` only — no compound assignment or field access (out of scope: structs aren't
   wired to real source yet). `name` is already consumed by the caller (v3_parse_statement), which
   must decide between this, a bare call statement, and an indexed write first — all three start
   with an identifier, and only one token of lookahead (TOKEN_ASSIGN vs TOKEN_OPEN_PARENTHESE vs
   TOKEN_OPEN_BRACKET) distinguishes them, so the identifier can't be re-consumed here. */
static void v3_parse_assignment(Chunk* c, unsigned int name_idx) {
    if (!consume(TOKEN_ASSIGN)) {
        error_at("v3 prototype only supports plain 'name = expr' assignment (no compound assignment or field access)");
        return;
    }
    int rk_val = v3_parse_binary(c, 0);
    if (parse_had_error) return;
    int reg = v3_var_slot(name_idx);
    if (reg < 0) return;   /* error_at already called */
    if (rk_val & V3_RK_CONST_FLAG) {
        chunk_emit(c, OP_V3_LOADK);
        chunk_emit(c, reg);
        chunk_emit(c, rk_val & ~V3_RK_CONST_FLAG);
    } else {
        chunk_emit(c, OP_V3_MOVE);
        chunk_emit(c, reg);
        chunk_emit(c, rk_val);
        /* Checked AFTER v3_var_slot (which may have just reserved `reg`, raising the floor) so
           this correctly recognizes rk_val as no-longer-a-temp in the (common) case where a
           brand-new variable's assigned register happens to already be the exact temp its RHS
           computed into. */
        if (v3_is_temp(rk_val)) v3_reg_free(1);
    }
}

/* M5 slice 4 — `name[index] = expr`, single-level only (no `name[i][j] = v` or `name[i].field = v`
   — those need the GET-until-the-last-step-then-SET shape parser.c's parse_assignment has,
   parser.c:352-398, out of scope here). `name` is already consumed and confirmed to be a known
   variable's register (the array/dict it holds is resolved dynamically at runtime, same as the
   real language — v3 does no compile-time type checking). */
static void v3_parse_index_assignment(Chunk* c, unsigned int name_idx) {
    int arr_reg = v3_var_slot(name_idx);
    if (arr_reg < 0) return;   /* error_at already called */

    int rk_idx = v3_parse_binary(c, 0);
    require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
    if (parse_had_error) return;
    require(TOKEN_ASSIGN, "v3 prototype only supports single-level 'name[index] = expr' writes (no chained/nested index or field writes)");
    if (parse_had_error) return;

    int rk_val = v3_parse_binary(c, 0);
    if (parse_had_error) return;

    v3_emit_index_set(c, arr_reg, rk_idx, rk_val);

    /* arr_reg is a permanent variable register — never freed. rk_idx/rk_val may be temps, now
       consumed by the SET; free the later-allocated one first, matching the free-then-allocate
       ordering used elsewhere (doesn't affect correctness, just keeps register use compact). */
    if (v3_is_temp(rk_val)) v3_reg_free(1);
    if (v3_is_temp(rk_idx)) v3_reg_free(1);
}

/* NEWLINE+INDENT, statements until DEDENT/EOF — mirrors parse_compound's shape (parser.c:1081-
   1110) minus its rollback/skip-to-boundary error recovery: a malformed script in this slice may
   abort ungracefully rather than recover per-statement, a stated simplification for a hand-fed
   test-string prototype. */
static void v3_parse_block(Chunk* c) {
    require(TOKEN_NEW_LINE, "expected newline before indented block");
    require(TOKEN_INDENT,   "expected indented block");
    if (parse_had_error) return;
    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        v3_parse_statement(c);
        if (parse_had_error) return;
    }
    consume(TOKEN_DEDENT);
}

static void v3_parse_if(Chunk* c) {
    int rk_cond = v3_parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");
    /* require() can't abort us on failure (a known gap in parser.c's own parse_if too — see the
       "AER require() no-abort bug" project notes) — without this, a malformed condition still
       gets a real branch compiled in. */
    if (parse_had_error) return;

    int reg_cond = v3_materialize(c, rk_cond);
    unsigned int patch_jif = v3_emit_jump_if_false_reg(c, reg_cond);
    if (v3_is_temp(reg_cond)) v3_reg_free(1);

    v3_parse_block(c);
    if (parse_had_error) return;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error) return;
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        v3_patch_jump(c, patch_jif, c->count);
        v3_parse_block(c);
        v3_patch_jump(c, patch_jmp, c->count);
    } else {
        v3_patch_jump(c, patch_jif, c->count);
    }
}

/* Shared while/for-while tail: require ':', branch-if-false, body, back-edge to loop_top. */
static void v3_parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond) {
    require(TOKEN_COLON, "expected ':' after for/while condition");
    if (parse_had_error) return;

    int reg_cond = v3_materialize(c, rk_cond);
    unsigned int patch_exit = v3_emit_jump_if_false_reg(c, reg_cond);
    if (v3_is_temp(reg_cond)) v3_reg_free(1);

    v3_parse_block(c);
    if (parse_had_error) return;

    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    v3_patch_jump(c, patch_exit, c->count);
}

/* M5 slice 5 — `for x in collection:`, arrays only (matching M4's OP_V3_ITER_NEXT_ARRAY's own
   scope — dict-key/pair and range iteration are still further out). No exit-time cleanup needed
   (col_reg/idx_reg are just registers, unlike the stack VM's popped-on-exit iterator slots — see
   OP_V3_ITER_NEXT_ARRAY's own comment in vm.h).
     Reserves the loop variable's permanent register BEFORE compiling the collection expression or
   allocating the index register — the reverse of what would look natural mirroring parser.c
   (which evaluates the collection first). This order is required here specifically because v3's
   temps and permanent variables share one register-numbering space (parser.c's stack slots and
   operand stack are separate address spaces, so it never has to think about this): reserving the
   loop variable first guarantees the collection/idx temps are allocated above it, so the temp
   allocator can never hand out the exact register the loop variable was just assigned. */
static void v3_parse_for_in(Chunk* c, unsigned int loop_var_name) {
    int item_reg = v3_var_slot(loop_var_name);
    if (item_reg < 0) return;

    int rk_col = v3_parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error) return;

    int col_reg = v3_materialize(c, rk_col);

    int idx_reg = v3_reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, OP_V3_LOADK);
    chunk_emit(c, idx_reg);
    chunk_emit(c, (int)pool_zero);

    unsigned int loop_top = c->count;   /* the iterate opcode is its own back-edge target */
    unsigned int patch_exit = v3_emit_iter_next_array(c, col_reg, idx_reg, item_reg);

    v3_parse_block(c);
    if (parse_had_error) return;

    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    v3_patch_jump(c, patch_exit, c->count);

    if (v3_is_temp(idx_reg)) v3_reg_free(1);
    if (v3_is_temp(col_reg)) v3_reg_free(1);
}

/* Real AER has no separate `while` keyword — `for <condition>:` (no `in`) IS the while form (see
   parse_for's fallback branch, parser.c:1275-1319). */
static void v3_parse_for_while(Chunk* c) {
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_IN)) {
            v3_parse_for_in(c, name_idx);
            return;
        }
        int reg = v3_var_slot(name_idx);
        if (reg < 0) return;
        unsigned int loop_top = c->count;
        int rk_cond = v3_parse_binary_ops(c, 0, reg);
        v3_parse_for_body(c, loop_top, rk_cond);
        return;
    }
    unsigned int loop_top = c->count;
    int rk_cond = v3_parse_binary(c, 0);
    v3_parse_for_body(c, loop_top, rk_cond);
}

/* M5 slice 3 — call sites. Reached both as an expression (v3_parse_primary, when an identifier is
   immediately followed by '(') and as a bare statement (v3_parse_statement, result discarded).
   `name_idx` is already consumed by the caller. */
/* Reached both as an expression (v3_parse_primary_inner, identifier immediately followed by '(')
   and as a bare statement (v3_parse_statement, result discarded). `name_idx` is already consumed
   by the caller. Handles BOTH function calls and struct instantiation — mirrors the stack VM's own
   OP_CALL-does-double-duty precedent (lbl_call falls back to chunk_find_shape() when a name isn't
   a variable/function), just resolved one level up at compile time via v3_is_struct_name instead
   of a runtime fallback chain, consistent with v3 already resolving functions at compile time too. */
static int v3_parse_call(Chunk* c, unsigned int name_idx) {
    bool is_struct = v3_is_struct_name(name_idx);
    unsigned int func_offset = 0;
    if (!is_struct && !v3_func_lookup(name_idx, &func_offset)) {
        error_at("Unknown function or struct type (v3 prototype requires it to be defined before any use — no forward references or mutual recursion)");
        return 0;
    }

    /* Arguments must land in contiguous registers for OP_V3_CALL/OP_V3_STRUCT_NEW's bulk-copy —
       see v3_arg_materialize's own comment for why this is usually a no-op check, not a copy. */
    int arg_reg_base;
    int arg_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    /* The result reuses the first argument's register (freeing the rest) — matches Lua's own
       convention of reusing the base register for a call's result, same compact-register-use
       philosophy as v3_compile_node's free-then-allocate discipline elsewhere. */
    int dest = (arg_count > 0) ? arg_reg_base : v3_reg_alloc();
    if (arg_count > 1) v3_reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    if (is_struct) v3_emit_struct_new(c, dest, name_idx, base, arg_count);
    else           v3_emit_call(c, dest, func_offset, base, arg_count);
    return dest;
}

/* `return expr` or bare `return` (implicit null) — no `return a, b` multi-value, no tail-call
   rewriting (both real parser.c features, out of scope here). */
static void v3_parse_return(Chunk* c) {
    if (v3_function_depth == 0) { error_at("'return' outside function"); return; }

    int rk;
    if (!equal(TOKEN_NEW_LINE) && !equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        rk = v3_parse_binary(c, 0);
        if (parse_had_error) return;
    } else {
        rk = (int)chunk_add_pool(c, aer_null()) | V3_RK_CONST_FLAG;
    }
    int reg = v3_materialize(c, rk);
    v3_emit_return(c, reg);
}

/* `function name(params):` — see this milestone's plan for the stated simplifications (no
   forward references/mutual recursion, no defaults, functions aren't first-class values, no
   nesting — the last one matches the real language's own restriction already). */
static void v3_parse_function(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected function name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");
    if (parse_had_error) return;

    unsigned int param_names[V3_FRAME_REGISTERS];
    int param_count = 0;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected parameter name"); return; }
            if (param_count >= V3_FRAME_REGISTERS) {
                error_at("Too many parameters for the v3 prototype (max %d)", V3_FRAME_REGISTERS);
                return;
            }
            param_names[param_count++] = chunk_add_pool(c, token.value);
            lex();
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after parameters");
    require(TOKEN_COLON,            "expected ':' after function signature");
    if (parse_had_error) return;

    if (v3_func_count >= V3_FUNC_MAX) {
        error_at("Too many functions for the v3 prototype (max %d)", V3_FUNC_MAX);
        return;
    }

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    /* Isolate the body's register space from the caller/top-level's — save the allocator's raw
       state and the variable table, reset both to fresh, restore after. Mirrors parser.c's
       current_locals save/restore around a function body (parser.c:1612-1614, 1630-1631); no
       stack of saved tables needed since named functions can't nest. */
    unsigned int saved_var_names[V3_FRAME_REGISTERS];
    int saved_var_count      = v3_var_count;
    int saved_next_temp      = v3_next_temp_register;
    int saved_reserved_floor = v3_reserved_floor;
    memcpy(saved_var_names, v3_var_names, sizeof(unsigned int) * (size_t)v3_var_count);
    v3_var_count          = 0;
    v3_next_temp_register = 0;
    v3_reserved_floor     = 0;

    /* Register the function BEFORE compiling its body — func_start is already known, so a
       self-recursive call inside the body resolves correctly (mirrors Test 9's hand-driven
       factorial precedent); a call to any other not-yet-defined function still correctly fails
       (the stated "no forward references/mutual recursion" limitation). */
    v3_func_names[v3_func_count]   = name_idx;
    v3_func_offsets[v3_func_count] = func_start;
    v3_func_count++;

    for (int i = 0; i < param_count; i++) v3_var_slot(param_names[i]);

    v3_function_depth++;
    v3_parse_block(c);
    v3_function_depth--;

    if (!parse_had_error) {
        /* Implicit 'return null' if control falls off the end (mirrors parser.c:1624-1628). */
        int rk_null  = (int)chunk_add_pool(c, aer_null()) | V3_RK_CONST_FLAG;
        int reg_null = v3_materialize(c, rk_null);
        v3_emit_return(c, reg_null);
    }

    v3_var_count = saved_var_count;
    memcpy(v3_var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    v3_next_temp_register = saved_next_temp;
    v3_reserved_floor     = saved_reserved_floor;

    v3_patch_jump(c, patch, c->count);
}

/* `struct Name:\n    field [= literal]\n ...` — mirrors parse_struct (parser.c:1414-1455)
   field-by-field, reusing the already-global MAX_STRUCT_FIELDS (vm.h) exactly as parser.c does.
   Emits the UNMODIFIED OP_DEFINE_STRUCT bytecode directly (see this opcode's own comment in vm.h
   for why it's genuinely stack-neutral and needs no v3-specific wrapper), then registers the type
   name so a later `Name(args)` call site resolves to construction (v3_parse_call). Field defaults
   are limited to the same literal set v3 already parses elsewhere (int/real/bool/null/string) — no
   bare `[]`/`{}` empty-container defaults, unlike parser.c's parse_literal_default. */
static void v3_parse_struct(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected struct name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_COLON,    "expected ':' after struct name");
    require(TOKEN_NEW_LINE, "expected newline before indented struct body");
    require(TOKEN_INDENT,   "expected indented struct body");
    if (parse_had_error) return;

    unsigned int field_names[MAX_STRUCT_FIELDS];
    AerVal       field_defaults[MAX_STRUCT_FIELDS];
    unsigned int field_count = 0;

    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (!equal(TOKEN_IDENTIFIER))          { error_at("Expected field name");        return; }
        if (field_count >= MAX_STRUCT_FIELDS)  { error_at("Too many struct fields (max %d)", MAX_STRUCT_FIELDS); return; }

        unsigned int fname = chunk_add_pool(c, token.value);
        lex();
        AerVal dflt = aer_null();
        if (consume(TOKEN_ASSIGN)) {
            if      (token.type == TOKEN_INTEGER) dflt = aer_int(aer_as_int(token.value));
            else if (token.type == TOKEN_REAL)    dflt = aer_real(aer_as_real(token.value));
            else if (token.type == TOKEN_TRUE || token.type == TOKEN_FALSE) dflt = aer_bool(aer_as_bool(token.value));
            else if (token.type == TOKEN_NULL)    dflt = aer_null();
            else if (token.type == TOKEN_STRING) {
                AerString* ts = aer_as_string(token.value);
                dflt = c->pool[v3_pool_escaped_string(c, ts->data, ts->length)];
            } else {
                error_at("Struct field defaults must be a literal value (v3 prototype: no array/dict defaults)");
                return;
            }
            lex();
        }
        field_names[field_count]    = fname;
        field_defaults[field_count] = dflt;
        field_count++;

        if (!equal(TOKEN_DEDENT) && !equal(TOKEN_END_OF_FILE))
            require(TOKEN_NEW_LINE, "expected newline after struct field");
        if (parse_had_error) return;
    }
    consume(TOKEN_DEDENT);

    if (field_count == 0) { error_at("Struct must have at least one field"); return; }
    if (v3_struct_count >= V3_STRUCT_MAX) {
        error_at("Too many struct types for the v3 prototype (max %d)", V3_STRUCT_MAX);
        return;
    }

    chunk_emit(c, OP_DEFINE_STRUCT);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)field_count);
    for (unsigned int i = 0; i < field_count; i++) {
        chunk_emit(c, (int)field_names[i]);
        chunk_emit(c, (int)chunk_add_pool(c, field_defaults[i]));
    }

    v3_struct_names[v3_struct_count++] = name_idx;
}

/* M5 slice 6 — `name.field = expr`, single-level only (no `name.a.b = v` or `name.a[0] = v` —
   those need the GET-until-the-last-step-then-SET shape parser.c's parse_assignment has, out of
   scope here, same limitation slice 4 already established for indexed writes). `name` is already
   consumed by the caller. */
static void v3_parse_field_assignment(Chunk* c, unsigned int name_idx) {
    int struct_reg = v3_var_slot(name_idx);
    if (struct_reg < 0) return;   /* error_at already called */

    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
    unsigned int field_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_ASSIGN, "v3 prototype only supports single-level 'name.field = expr' writes (no chained field/index writes)");
    if (parse_had_error) return;

    int rk_val = v3_parse_binary(c, 0);
    if (parse_had_error) return;

    v3_emit_field_set(c, struct_reg, field_idx, rk_val);
    if (v3_is_temp(rk_val)) v3_reg_free(1);
}

static void v3_parse_statement(Chunk* c) {
    if (consume(TOKEN_IF))       { v3_parse_if(c);       return; }
    if (consume(TOKEN_FOR))      { v3_parse_for_while(c); return; }
    if (consume(TOKEN_FUNCTION)) { v3_parse_function(c);  return; }
    if (consume(TOKEN_RETURN))   { v3_parse_return(c);    return; }
    if (consume(TOKEN_STRUCT))   { v3_parse_struct(c);    return; }
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) { v3_parse_call(c, name_idx); return; }
        if (consume(TOKEN_OPEN_BRACKET))    { v3_parse_index_assignment(c, name_idx); return; }
        if (consume(TOKEN_DOT))             { v3_parse_field_assignment(c, name_idx); return; }
        v3_parse_assignment(c, name_idx);
        return;
    }
    error_at("Expected a statement (v3 prototype: only assignment, indexed/field writes, if/else, for-while, function/struct defs, return, and calls are supported)");
}

/* Entry point — mirrors parse()'s own loop (parser.c:267-299) minus its rollback/skip-to-boundary
   recovery. Resets the register allocator, the variable table, the function table, and
   parse_had_error so this run is isolated from any earlier chunk compiled in the same process
   (same reasoning as tests/v3_smoke_test.c's run_chunk resetting runtime_had_error per test). */
void v3_parse(Chunk* c) {
    v3_reg_reset();
    v3_var_count      = 0;
    v3_func_count     = 0;
    v3_struct_count   = 0;
    v3_function_depth = 0;
    parse_had_error   = false;
    while (!equal(TOKEN_END_OF_FILE)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (consume(TOKEN_DEDENT))   continue;
        v3_parse_statement(c);
        if (parse_had_error) return;
    }
}
