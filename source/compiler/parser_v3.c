#include "parser_v3.h"

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
