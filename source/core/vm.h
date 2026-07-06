#ifndef VM_H
#define VM_H

#include "hashmap.h"
#include "value.h"
#include "dictmap.h"
#include "value_box.h"

/* Defined here (after dictmap.h) using DictMap not HashMap: dict values are stored inline with
   their key. A scope's overflow storage still uses HashMap (boxed values) — that requirement was
   closures needing a stable address to capture; closures are gone now, so scope-overflow could
   move to DictMap too, but that's a separate, not-yet-done follow-up, not part of this change. */
struct AerDict {
    DictMap map;
};

typedef enum {
    /* Constants */
    OP_PUSH,           /* operand: pool index — push value onto stack */
    OP_DUP_N,          /* operand: count n — pushes copies of the top n stack values, in their
                           original order (n=1: [x] -> [x,x]; n=2: [x,y] -> [x,y,x,y]). Used by
                           compound assignment to a field/index chain (parser.c's parse_assignment)
                           to read-then-write the same target without re-evaluating whatever
                           expression produced it (e.g. not calling a side-effecting index twice). */

    /* Variables */
    OP_LOAD,           /* operands: pool index (TYPE_STRING), addr_cache slot — walk scope chain and push value, or use cache if populated */
    OP_STORE,          /* operands: pool index (TYPE_STRING), addr_cache slot — update nearest binding, or create in current scope */
    OP_DEFINE,         /* operand: pool index (TYPE_STRING) — always create/update in innermost scope */

    /* Every local in a function — parameters and ordinary body locals alike — gets a
       compile-time-known slot in the call's one flat base scope (current_locals, parser.c);
       assignment inside a function is always local (no walk-up-and-mutate-outer), which is what
       makes every name in a function body resolvable to a fixed slot at parse time.
       OP_DEFINE_LOCAL still carries name_idx (not just the slot) even though emit_load/emit_store
       never need it — a call site (OP_CALL, and defer's replay) isn't resolved at parse time the
       way a plain variable reference is, since the parser doesn't know a name will be *called*
       until it sees it, and it emits the same name-based opcode either way. Its runtime fallback
       (vm_scope_get) must still be able to find a function value held in a local by name, which
       needs the slot's name recorded. */
    OP_LOAD_LOCAL,     /* operand: slot index */
    OP_STORE_LOCAL,    /* operand: slot index */
    OP_DEFINE_LOCAL,   /* operands: slot index, name pool idx — first assignment to a name in this function */

    /* Compile-time fusion of compound assignments (`x += 1`) into one opcode, done by parser.c
       at emit time (never a post-hoc peephole pass) so it's safe against AER's absolute-offset
       jump patching; only fires when both operands are provably simple. "NAME" here means
       anything OP_LOAD/OP_STORE would resolve via the scope chain (global or non-parameter
       local), not just true globals. Each handler must recheck runtime_had_error after every
       fallible step before writing back, since — unlike the unfused 4-opcode form — there's no
       intervening DISPATCH() to abort on an error partway through. */
    OP_COMPOUND_NAME_CONST,  /* operands: lhs_name_idx, lhs_cache_idx, bin_op, rhs_pool_idx */
    OP_COMPOUND_NAME_NAME,   /* operands: lhs_name_idx, lhs_cache_idx, bin_op, rhs_name_idx, rhs_cache_idx */
    OP_COMPOUND_LOCAL_CONST, /* operands: lhs_slot, bin_op, rhs_pool_idx */
    OP_COMPOUND_LOCAL_LOCAL, /* operands: lhs_slot, bin_op, rhs_slot */
    OP_COMPOUND_LOCAL_NAME,  /* operands: lhs_slot, bin_op, rhs_name_idx, rhs_cache_idx */
    OP_COMPOUND_NAME_LOCAL,  /* operands: lhs_name_idx, lhs_cache_idx, bin_op, rhs_slot */

    /* Compile-time fusion of a plain binary arithmetic expression (`a + b`, not an assignment) —
       same discard-and-truncate-then-reemit scheme as OP_COMPOUND_* above, applied a third time
       (see parse_binary_ops, parser.c). Profiling on nbody.aer showed OP_LOAD_LOCAL alone at ~29%
       of all dispatches even though it's already minimal cost (one array read, no checks) — the
       remaining cost is dispatch *count*, which only fusion (not caching) can reduce. Scoped to the
       6 arithmetic ops only (+ - * / % //); AND/OR excluded (short-circuit doesn't fit "evaluate
       both sides unconditionally"), and a struct-field operand (`b.mass`) doesn't fuse in this pass
       — only LOCAL/NAME/CONST, the same shapes classify_operand already recognizes. `bin_op` is a
       runtime operand (like OP_COMPOUND_* already does), not one opcode per operator. */
    OP_BINARY_LOCAL_LOCAL,  /* operands: lhs_slot, bin_op, rhs_slot */
    OP_BINARY_LOCAL_CONST,  /* operands: lhs_slot, bin_op, rhs_pool_idx */
    OP_BINARY_LOCAL_NAME,   /* operands: lhs_slot, bin_op, rhs_name_idx, rhs_cache_idx */
    OP_BINARY_NAME_LOCAL,   /* operands: lhs_name_idx, lhs_cache_idx, bin_op, rhs_slot */
    OP_BINARY_NAME_CONST,   /* operands: lhs_name_idx, lhs_cache_idx, bin_op, rhs_pool_idx */
    OP_BINARY_NAME_NAME,    /* operands: lhs_name_idx, lhs_cache_idx, bin_op, rhs_name_idx, rhs_cache_idx */

    /* Binary arithmetic */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_FLOOR_DIV,

    /* Binary comparison */
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_IN,              /* key in dict → key existence; value in array → element scan */

    /* Binary logical */
    OP_AND, OP_OR,
    OP_PIPE,            /* never emitted — parser.c rewrites x |> f(args) into OP_CALL; exists only for parse_binary_ops's table-driven dispatch */

    /* Binary bitwise */
    OP_BITWISE_AND, OP_BITWISE_OR, OP_BITWISE_XOR, OP_LSHIFT, OP_RSHIFT,

    /* Unary */
    OP_NEGATE, OP_NOT, OP_BITWISE_NOT,

    /* Control flow */
    OP_JUMP,            /* operand: absolute code index */
    OP_JUMP_IF_FALSE,   /* operand: absolute code index — pops condition, jumps if falsy */
    OP_JUMP_IF_TRUE,    /* operand: absolute code index — pops condition, jumps if truthy */

    /* Compile-time-fused `<comparison>` immediately followed by a JUMP_IF_FALSE — the condition
       of every if/while, virtually always a comparison, otherwise costs two dispatches (push a
       bool, then pop-and-branch on it) for what's conceptually one decision. Recognized only when
       parse_binary_ops (parser.c) reports that a condition's own OUTERMOST operator was a bare
       EQ/NEQ/LT/GT/LTE/GTE — a real signal from the parser about what it structurally just
       emitted, not inferred by inspecting the trailing bytecode word after the fact (which would
       be unsound: an unrelated instruction's operand — a pool index, a cache slot — can
       coincidentally equal a comparison opcode's numeric value). See emit_jump_if_false's comment
       in parser.c for the full reasoning. */
    OP_CMP_JUMP_FALSE,  /* operands: comparison Opcode, absolute code index — pops b, pops a, jumps if !truthy(a <op> b) */

    /* Scope */
    OP_PUSH_SCOPE,
    OP_POP_SCOPE,

    /* Functions */
    OP_CALL,            /* operands: name pool index, arg count, addr_cache slot — looks up function by name, or by cache if populated */
    OP_TAIL_CALL,       /* same operands/handler as OP_CALL; emitted for a bare `return name(args)` (parse_return) — reuses the call frame instead of pushing a new one, unless it has pending defers */
    OP_CALL_VALUE,      /* operand: arg count — pops function value from stack, then args  */
    OP_RETURN,          /* pops return value, restores call frame, pushes at call site */
    OP_DEFER_PUSH,      /* operands: name pool index, arg count — stashes name+popped args on the current frame's defers[] list; the call itself happens later, in lbl_return (vm.c) */

    /* Arrays and dicts */
    OP_ARRAY_NEW,        /* operand: item count — pops N values, pushes array            */
    OP_DICT_NEW,         /* operand: pair count — pops N key+val pairs, pushes dict      */
    OP_INDEX_GET,        /* pops key/index, pops collection, pushes value                */

    /* Compile-time-fused `name[index]` reads (Part 2 of the OP_COMPOUND_* fusion scheme above)
       — recognized only where a bare identifier is immediately followed by '[' (parse_primary_
       inner, parser.c), never for a chained bracket or a slice. Reads both operands directly,
       computes via the same vm_index_get_compute() OP_INDEX_GET uses, then pushes the result.
       v1 only fuses a bare CONST/LOCAL/NAME index — an arithmetic index (`distances[i+1]`) falls
       back to the unfused path; a NAME-kind operand must recheck runtime_had_error and DISPATCH()
       immediately, same as OP_COMPOUND_NAME_* (a LOCAL-kind parameter slot can't fail). */
    OP_INDEX_GET_LOCAL_CONST, /* operands: arr_slot, idx_pool_idx */
    OP_INDEX_GET_LOCAL_LOCAL, /* operands: arr_slot, idx_slot */
    OP_INDEX_GET_LOCAL_NAME,  /* operands: arr_slot, idx_name_idx, idx_cache_idx */
    OP_INDEX_GET_NAME_CONST,  /* operands: arr_name_idx, arr_cache_idx, idx_pool_idx */
    OP_INDEX_GET_NAME_LOCAL,  /* operands: arr_name_idx, arr_cache_idx, idx_slot */
    OP_INDEX_GET_NAME_NAME,   /* operands: arr_name_idx, arr_cache_idx, idx_name_idx, idx_cache_idx */

    OP_INDEX_SET,        /* pops value, pops key/index, pops collection, sets            */

    /* Compile-time-fused `name[index] = <literal>` writes — the write-side counterpart of the
       OP_INDEX_GET_*_* family above, same array/index shape restrictions (bare identifier's
       first bracket only, CONST/LOCAL/NAME index, never a chain or a slice). The value is
       further restricted to a bare CONST specifically (not also LOCAL/NAME, unlike the index) —
       see parse_assignment's comment (parser.c) for why: it's what makes resolving array/index
       at THIS opcode's own dispatch time — necessarily after the value's own bytecode, which is
       emitted first — safe regardless of evaluation order. Stack-neutral: none of the three
       operands come from the stack, so nothing is pushed or popped by these at all. */
    OP_INDEX_SET_LOCAL_CONST, /* operands: arr_slot, idx_pool_idx, val_pool_idx */
    OP_INDEX_SET_LOCAL_LOCAL, /* operands: arr_slot, idx_slot, val_pool_idx */
    OP_INDEX_SET_LOCAL_NAME,  /* operands: arr_slot, idx_name_idx, idx_cache_idx, val_pool_idx */
    OP_INDEX_SET_NAME_CONST,  /* operands: arr_name_idx, arr_cache_idx, idx_pool_idx, val_pool_idx */
    OP_INDEX_SET_NAME_LOCAL,  /* operands: arr_name_idx, arr_cache_idx, idx_slot, val_pool_idx */
    OP_INDEX_SET_NAME_NAME,   /* operands: arr_name_idx, arr_cache_idx, idx_name_idx, idx_cache_idx, val_pool_idx */

    OP_SLICE_GET,        /* pops end, start (either may be TYPE_NULL for "unspecified"), pops collection, pushes a new array or string sub-range */
    OP_UNPACK,           /* operand: index — peeks TOS (array), pushes items[index]      */
    OP_ITER_NEXT,        /* operand: end_addr — stack: [col, idx]; advances, pushes item */
    OP_ITER_NEXT_PAIR,   /* operand: end_addr — stack: [dict, idx]; pushes key then val  */
    OP_ITER_RANGE,       /* operand: end_addr — stack: [cur, end]; pushes cur, advances  */

    /* Structs — instances are TYPE_ARRAY with a non-NULL AerArray.shape; instantiation (`Point(1, 2)`) reuses OP_CALL, resolved in lbl_call against the shape registry when the name isn't a variable. */
    OP_DEFINE_STRUCT,    /* operands: name pool idx, field count, then that many (field-name, default-value) pool-idx pairs — registers a Shape in the chunk's shape table */
    OP_FIELD_GET,        /* operand: field-name pool idx — pops struct, pushes field */
    OP_FIELD_SET,        /* operand: field-name pool idx — pops value, pops struct   */
    OP_CHECK_SHAPE,      /* operand: type-name pool idx — `x as Player`; pops value, errors unless it's that exact struct type, else pushes back unchanged (never converts, unlike OP_CAST) */

    /* Modules — `import math` emits no bytecode, just records "math" as a known module name on the chunk (parse-time only); a module is never a runtime value, so `math.sqrt(x)` compiles directly to OP_CALL_MODULE. */
    OP_CALL_MODULE,     /* operands: module-name pool idx, function-name pool idx, arg count — dispatched against the VM's native module table (aer_stdlib.c), never a scope lookup */

    /* Misc */
    OP_PRINT_REPL,  /* shell mode: prints value only if not null, then pops */
    OP_POP,
    OP_TO_STR,  /* pops any value, pushes its string representation */
    OP_CAST,    /* operand: CAST_INTEGER/CAST_FLOAT/CAST_BOOLEAN — pops value, pushes it converted */
    OP_HALT,
} Opcode;

/* OP_CAST operand values — target type for `x as T` (T=string compiles to OP_TO_STR instead, since that conversion already existed). */
#define CAST_INTEGER 0
#define CAST_FLOAT   1
#define CAST_BOOLEAN 2

#define MAX_STRUCT_FIELDS 16
#define MAX_DEFERS_PER_CALL 8  /* max pending `defer` statements per function call */
#define MAX_DEFER_ARGS      8  /* max arguments to a single deferred call */

/* A struct type's blueprint (field names in order + default literals); individually heap-allocated and never moved/realloc'd, so AerArray.shape pointers stay valid as the shape table grows. */
struct Shape {
    unsigned int name;                              /* pool index of the struct's type name */
    unsigned int field_count;
    unsigned int field_names[MAX_STRUCT_FIELDS];     /* pool indices, declaration order       */
    AerVal       field_defaults[MAX_STRUCT_FIELDS];
};

/* ------------------------------------------------------------------ */
/* Bytecode chunk                                                       */
/* code[] is a flat int array: each instruction is one opcode int, optionally followed by one operand int. */
/* ------------------------------------------------------------------ */

typedef struct {
    int*         code;
    unsigned int count, capacity;

    AerVal*      pool;           /* constants and variable names — all deduplicated by value */
    unsigned int pool_count, pool_cap;

    /* name -> pool index, for O(1) dedup of TYPE_STRING pool entries (chunk_add_pool, vm.c); owns an independent copy of each key. */
    HashMap      name_index;

    /* Struct type registry appended to by OP_DEFINE_STRUCT; redeclaring a struct appends rather than replaces so old Shape pointers stay valid, and chunk_find_shape() searches newest-first. */
    Shape**      shapes;
    unsigned int shape_count, shape_cap;

    /* Module names from `import`, parse-time only, tracked on the chunk so a later REPL line still recognizes a module an earlier line imported. */
    char**       imported_modules;
    unsigned int import_count, import_cap;

    /* Bytecode-offset -> source-line mapping, one entry per statement (not instruction), so
       runtime errors can report a line the way parse errors do. Strictly increasing by offset;
       chunk_line_for_offset() binary-searches it. Rolled back alongside bytecode on a recovered
       parse error so a discarded statement doesn't leave a dangling marker. */
    unsigned int* line_mark_offsets;
    unsigned int* line_mark_lines;
    unsigned int  line_mark_count, line_mark_cap;

    /* One inline cache slot per OP_CALL/OP_TAIL_CALL/OP_LOAD/OP_STORE site, populated the first
       time that site resolves its name to an address in the GLOBAL scope's inline slots (a
       fixed, never-reallocated location — see lbl_call/lbl_load/lbl_store in vm.c). NULL means
       not cached. OP_LOAD/OP_STORE always target a true global now (every function-local name
       resolves to OP_LOAD_LOCAL/STORE_LOCAL at parse time instead — see parser.c's current_locals),
       so their sites cache unconditionally; OP_CALL/OP_TAIL_CALL can still name a parameter holding
       a function value, so those must only cache when the global is the WINNING resolution, not
       merely "a global with this name exists somewhere" (see lbl_call's comment on the bug that
       taught this). No invalidation needed — this caches the ADDRESS a name resolved to, not a
       value, so reassignment is already reflected. */
    AerVal**     addr_cache;
    unsigned int addr_cache_count, addr_cache_cap;

#ifdef AER_DEBUG_TOOLS
    /* One dispatch counter per bytecode word, indexed by offset — only the word an opcode itself
       starts at is ever incremented (see DISPATCH() in vm.c), operand words stay 0. Grown in
       lockstep with `code` by chunk_ensure_debug_hits (vm.c), called once at the top of vm_run.
       Entirely absent from a normal build — see source/core/disasm.h. */
    unsigned long long* debug_hits;
    unsigned int         debug_hits_cap;
#endif
} Chunk;

/* ------------------------------------------------------------------ */
/* Virtual machine                                                      */
/* ------------------------------------------------------------------ */

#define VM_STACK_MAX    256
#define VM_CALL_MAX     64
/* No closures and no per-block scoping left (see parser.c's current_locals / vm_scope_floor):
   exactly one AerScope is pushed per active call, plus scopes[0] for globals — so scope_depth
   can never exceed call_depth+1. Defined off VM_CALL_MAX so the relationship stays exact instead
   of an independent guess. */
#define VM_SCOPE_MAX    (VM_CALL_MAX + 1)
/* Inline slots before spilling to hashmap; also the hard ceiling for OP_LOAD_LOCAL/OP_STORE_LOCAL/
   OP_DEFINE_LOCAL's unchecked slot index, so must stay >= parser.c's MAX_PARAMS. Holds every
   distinct local name in a whole function now (assignment inside a function is always local —
   see parser.c's current_locals), not just parameters, hence the larger ceiling than the old
   params-only 24. */
#define SCOPE_SLOT_MAX  32
#define VM_KEY_MAX      4096   /* max dict key length for stack-buffered lookups */

typedef struct {
    unsigned int name;  /* pool index of the variable name */
    AerVal       val;
} ScopeSlot;

/* Small scopes (<=8 vars) use a flat array with integer-key comparison, zero heap allocation;
   larger scopes spill into the hashmap. */
typedef struct {
    ScopeSlot slots[SCOPE_SLOT_MAX];
    int       count;
    bool      overflow;
    HashMap   map;
} AerScope;

/* A `defer name(args)` statement. `name_idx` resolves at replay time like OP_CALL (not snapshotted, so reassigning the name before return changes what runs — a disclosed limitation, see README); `args` ARE snapshotted, evaluated once at the defer statement. */
typedef struct {
    unsigned int name_idx;
    AerVal       args[MAX_DEFER_ARGS];
    int          arg_count;
} DeferredCall;

typedef struct {
    unsigned int return_ip;
    int          return_scope_depth;
    AerFunction* function;   /* the function this frame runs (never NULL) — keeps this function_pool cell alive for the GC's mark phase during the call, even when nothing else references it (e.g. an immediately-invoked anonymous function). */

    /* Pending `defer` calls, drained LIFO by lbl_return before the frame unwinds; pending_return_value
       stashes the real return value while deferred calls run, since they may reuse this same stack slot.
       `defers` is a pointer, lazily xmalloc'd on first use rather than an embedded MAX_DEFERS_PER_CALL
       array, because `defer` is rare but call_stack[VM_CALL_MAX] is not — an embedded array bloated every
       CallFrame to ~1.2KB and blew through L1 cache on deep recursion for a feature most calls never use.
       Kept (not freed) and reused once allocated for a slot, freed only in vm_free(); defer_count resets
       to 0 at every call setup regardless of whether defers is NULL or already allocated. */
    DeferredCall* defers;
    int          defer_count;
    bool         defers_draining;   /* false until lbl_return's first visit — distinguishes a real return value from a just-finished deferred call's discarded result */
    AerVal       pending_return_value;
} CallFrame;

typedef struct {
    Chunk*       chunk;
    unsigned int ip;
    AerVal       stack[VM_STACK_MAX];
    int          stack_top;
    AerScope     scopes[VM_SCOPE_MAX];
    int          scope_depth;    /* always >= 1; scopes[0] is the global scope */
    CallFrame    call_stack[VM_CALL_MAX];
    int          call_depth;
} VM;

void         chunk_init(Chunk* c);
void         chunk_free(Chunk* c);
void         chunk_emit(Chunk* c, int word);

/* Records that bytecode from `offset` onward belongs to source `line`, once per statement not instruction (see line_mark_offsets); no-op if offset doesn't strictly increase from the last mark. */
void         chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line);

/* The source line whose statement contains `offset` (the largest recorded mark at or before it), or 0 if the chunk has no marks yet. */
unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset);

/* Allocates one new (NULL) inline-cache slot for an OP_CALL/OP_TAIL_CALL/OP_LOAD/OP_STORE site being compiled, returning the index its cache-slot operand carries (see Chunk.addr_cache). */
unsigned int chunk_add_addr_cache(Chunk* c);
unsigned int chunk_add_pool(Chunk* c, AerVal v);
Shape*       chunk_find_shape(Chunk* c, const char* name);

/* `name` binds the module ("mid" for both `import mid` and `import sub.mid`); `path_name` is what
   resolves to a file, dots as directory separators ("sub.mid" -> "sub/mid.aer"), equal to name/len
   for a plain import. Neither needs NUL-termination. Returns false if `name` isn't a native module
   (aer_stdlib.h) and the file can't be loaded; errors via error_at() if MAX_IMPORTS is exceeded. */
bool chunk_add_import(Chunk* c, const char* name, unsigned int len,
                       const char* path_name, unsigned int path_len);
bool chunk_is_imported(Chunk* c, const char* name, unsigned int len);

void vm_init(VM* vm, Chunk* chunk);
void vm_free(VM* vm);

/* Runs vm->chunk's bytecode from vm->ip until OP_HALT or a runtime error; returns true on a clean
   finish, false on error (same as reading runtime_had_error right after). Embedding contract: since
   an error no longer kills the process, a host reusing the same VM* after a false return must first
   reset stack_top=0, call_depth=0, and pop scope_depth back to 1 (freeing any overflow hashmap) —
   main.c's run() does this between REPL statements; other hosts must replicate it. */
bool vm_run(VM* vm);

/* GC suppression is a counter, not a flag (imports can nest: A imports B imports C). aer_module.c
   wraps its nested vm_run() for a file-module's top-level code in suppress/unsuppress: during that
   window the outer chunk/VM isn't in any root set yet, so a collection could sweep what the outer
   file still needs. Imports are small and one-time, so not collecting here is cheap insurance. */
void vm_gc_suppress(void);
void vm_gc_unsuppress(void);

/* Shared call-setup (arity/receiver-type check, call-frame construction) used by lbl_call_value (vm.c) and aer_module_call (aer_module.c) — full contract in vm_setup_call's own comment in vm.c. */
bool vm_setup_call(VM* target, Chunk* fn_chunk, AerVal fv, int arg_count,
                    AerVal* args, unsigned int return_ip);

/* Returns an uninitialized AerArray header from vm.c's internal slab pool, as if xmalloc'd directly (every in-file vm.c site still uses pool_alloc); exposed only because aer_stdlib.c's string.split() needs one and the pool isn't a raw global outside vm.c. */
AerArray* vm_new_array(void);

/* Same idea, for AerDict — exposed for aer_json.c's json.decode(); caller must set map.is_inline = true and zero the rest of map itself (see lbl_dict_new's call site in vm.c). */
AerDict* vm_new_dict(void);

/* Same idea, for AerFunction — exposed for parser.c's emit_plain_function_value. Must come from
   function_pool like every AerFunction: the GC's mark phase expects every TYPE_FUNCTION value in
   Chunk.pool to be a real function_pool cell, and an xmalloc'd/xcalloc'd one makes pool_mark's slab
   lookup fail outright, not just leak. Zero the struct yourself after calling — unlike the xcalloc
   this replaced, pool_alloc returns uninitialized memory. */
AerFunction* vm_new_function(void);

#ifdef AER_DEBUG_TOOLS
#include <stdio.h>
/* Prints a byte-accurate memory breakdown (header vs. payload bytes per pool, plus GC run counts)
   to `out` — see source/core/vm.c for what "payload" means per type. Debug-build only. */
void aer_debug_memory_report(FILE* out);
#endif

#endif
