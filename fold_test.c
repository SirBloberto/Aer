/* Minimal reproducer: does always_inline + a literal-constant switch argument actually fold
   away on this compiler/target, the way vm.c's vm_arith/vm_compare/etc. are assumed to? */
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_FLOOR_DIV } Op;

static inline __attribute__((always_inline)) int compute(int a, int b, Op op) {
    switch (op) {
        case OP_ADD:       return a + b;
        case OP_SUB:       return a - b;
        case OP_MUL:       return a * b;
        case OP_DIV:       return a / b;
        case OP_MOD:       return a % b;
        case OP_FLOOR_DIV: return (a - (a % b)) / b;
        default:           return 0;
    }
}

/* Each of these calls compute() with a LITERAL enum constant, exactly like vm_run's
   BINARY_OP_LABEL sites call vm_arith(b, cc, OP_ADD) etc. If folding works, each function
   below should compile down to just ITS ONE operation — no switch, no dead cases. */
int do_add(int a, int b) { return compute(a, b, OP_ADD); }
int do_sub(int a, int b) { return compute(a, b, OP_SUB); }
int do_mul(int a, int b) { return compute(a, b, OP_MUL); }
int do_div(int a, int b) { return compute(a, b, OP_DIV); }

/* Runtime-variable case, for comparison — this one SHOULD keep the full switch. */
int do_runtime(int a, int b, Op op) { return compute(a, b, op); }
