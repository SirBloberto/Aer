#ifndef VM_H
#define VM_H

#include "hashmap.h"
#include "value.h"
#include "dictmap.h"
#include "value_box.h"

/* Defined here (after dictmap.h) using DictMap not HashMap: dict values are stored inline with their key, unlike a scope's overflow HashMap whose boxed values must keep a stable address for closure capture. */
struct AerDict {
    DictMap map;
};

typedef enum {
    /* Constants */
    OP_PUSH,           /* operand: pool index — push value onto stack */

    /* Variables */
    OP_LOAD,           /* operands: pool index (TYPE_STRING), addr_cache slot — walk scope chain and push value, or use cache if populated */
    OP_STORE,          /* operands: pool index (TYPE_STRING), addr_cache slot — update nearest binding, or create in current scope */
    OP_DEFINE,         /* operand: pool index (TYPE_STRING) — always create/update in innermost scope */

    /* Parameters only: a compile-time-known slot in the call's base scope, emitted instead of the name-based opcodes above only when the parser has proven the name is a parameter of the function being compiled (current_params, parser.c); still must check a captured-by-closure box (see lbl_load_local etc. in vm.c). */
    OP_LOAD_LOCAL,     /* operand: slot index */
    OP_STORE_LOCAL,    /* operand: slot index */
    OP_DEFINE_LOCAL,   /* operand: slot index */

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

    /* Closures — only anonymous function expressions can capture. A captured variable's scope slot is "boxed" (promoted to a heap cell) the moment it's captured; see ScopeSlot.box, vm_box_slot(). */
    OP_CAPTURE,          /* operand: name pool idx — boxes that name's binding if not boxed already, appends it to vm->pending_upvalues (consumed by the OP_MAKE_CLOSURE that follows) */
    OP_MAKE_CLOSURE,      /* operands: code_offset, arity, min_arity, has_receiver (0/1), receiver_type, upvalue_count, then (arity - min_arity) default-value pool indices — builds a TYPE_FUNCTION from pending_upvalues[0..count), pushes it, resets the pending count */
    OP_LOAD_UPVALUE,      /* operand: upvalue slot index — pushes *upvalues[slot] for the currently-executing closure */
    OP_STORE_UPVALUE,     /* operand: upvalue slot index — pops value, writes *upvalues[slot] = value */

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
#define MAX_CAPTURES      16   /* max upvalues a single closure can have */
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
       not cached, including when a name resolves as a local: caching must only happen when the
       global is the WINNING resolution, not merely "a global with this name exists somewhere"
       (see lbl_call's comment on the bug that taught this). No invalidation needed — this caches
       the ADDRESS a name resolved to, not a value, so reassignment is already reflected. */
    AerVal**     addr_cache;
    unsigned int addr_cache_count, addr_cache_cap;
} Chunk;

/* ------------------------------------------------------------------ */
/* Virtual machine                                                      */
/* ------------------------------------------------------------------ */

#define VM_STACK_MAX    256
#define VM_SCOPE_MAX    64
#define VM_CALL_MAX     64
/* Inline slots before spilling to hashmap; also the hard ceiling for OP_LOAD_LOCAL/OP_STORE_LOCAL/
   OP_DEFINE_LOCAL's unchecked slot index, so must stay >= parser.c's MAX_PARAMS (parameters plus
   hybrid-scope top-level locals sharing the slot table). Raised 8->24: 8 was already silently
   unsafe for any function with 9+ parameters even before hybrid scopes existed. */
#define SCOPE_SLOT_MAX  24
#define VM_KEY_MAX      4096   /* max dict key length for stack-buffered lookups */

typedef struct {
    unsigned int name;  /* pool index of the variable name */
    AerVal       val;   /* live value when box == NULL */
    AerVal*      box;   /* non-NULL once captured by a closure — val is then stale, reads/writes go through *box; a hashmap-overflow variable boxes instead by reusing its existing hashmap pointer (see AerScope.overflow_has_captures, vm_box_slot() in vm.c) */
} ScopeSlot;

/* Small scopes (<=8 vars) use a flat array with integer-key comparison, zero heap allocation; larger scopes spill into the hashmap. */
typedef struct {
    ScopeSlot slots[SCOPE_SLOT_MAX];
    int       count;
    bool      overflow;
    HashMap   map;

    /* Set by vm_box_slot() when a closure captures a variable during hashmap-overflow: the
       hashmap value pointer is stable enough for the closure to hold, but hashmap_free would
       otherwise free it out from under that closure at scope-pop — so once set, this scope's
       hashmap is deliberately never freed (consistent with the leak-forever policy elsewhere). */
    bool      overflow_has_captures;
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
    AerVal**     upvalues;   /* the called function's upvalues array, or NULL if not a closure — lets OP_LOAD/STORE_UPVALUE find the right boxes without a name lookup */
    AerFunction* function;   /* the function/closure this frame runs (never NULL) — keeps this function_pool cell (and its upvalues) alive for the GC's mark phase during the call, even when nothing else references it (e.g. an immediately-invoked anonymous closure). */

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

    /* Boxes accumulated by OP_CAPTURE, consumed by the OP_MAKE_CLOSURE that follows — see OP_CAPTURE's comment. */
    AerVal*      pending_upvalues[MAX_CAPTURES];
    unsigned int pending_upvalue_count;
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
   reset stack_top=0, call_depth=0, and pop scope_depth back to 1 (honoring overflow_has_captures) —
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

#endif
