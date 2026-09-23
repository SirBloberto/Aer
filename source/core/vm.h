#ifndef VM_H
#define VM_H

#include "error.h"
#include "hashtable.h"
#include "heap.h"
#include "pool.h"
#include "strbuf.h"
#include "opcodes.h"
#include "value.h"
#include "value_format.h"

/* Set = constant-pool index, clear = register (Lua's BITRK convention). Parser-internal: every
   emission site converts it to one of the wire encodings below. */
#define RK_CONST_FLAG (1 << 30)

/* Parser-internal too: a slot's static type, so the parser can pick an unchecked opcode. Not a
   separate bank -- every slot is an AerVal in the one register file. */
#define RK_RAW_INT_FLAG (1 << 29)
#define RK_RAW_REAL_FLAG (1 << 28)
#define RK_RAW_SLOT_MASK 0x7F

/* Per-frame register bank size; a register index must stay within RK8's 7 index bits with zero
   headroom to spare -- see RK8 below. */
#define FRAME_REGISTERS 128

/* Real-typed slots grow DOWN from the top of the frame, everything else up, so their position is
   fixed before a body compiles -- which a 7-bit operand needs and a per-function size cannot give.
   Frame entry tags them once and the unchecked real opcodes then store only the payload. Such a body
   gets a full-size frame; FRAME_BOUNDS names the gap that leaves, never written, tagged or traced.
   Integers need none of this -- their unchecked opcodes still write the tag. */
#define FRAME_BOUNDS(dyn_end, real_base) ((unsigned short)(((dyn_end) << 8) | (real_base)))
#define FRAME_DYN_END(bounds) ((unsigned int)((bounds) >> 8))
#define FRAME_REAL_BASE(bounds) ((unsigned int)((bounds) & 0xFF))

/* Fixed-width, word-granular instruction encoding: every instruction is one or more 32-bit words,
   the shape (1-word, 2-word, ...) fixed per opcode at compile time -- never a variable byte count.
   See ARCHITECTURE.md §3.1-3.2 for the full field vocabulary (PACK3/PACK2/PACK1, RK8, RK16,
   PACK_2X16) and why this design was chosen over a wider bit-packed word. */

#define UNPACK_A(word) (((word) >> 8) & 0xFF)
#define UNPACK_B(word) (((word) >> 16) & 0xFF)
#define UNPACK_C(word) (((word) >> 24) & 0xFF)

#define SCALED_A 1u
#define SCALED_B 2u
#define SCALED_C 4u

/* Which of an opcode's A/B/C fields hold a register slot doubled, so its handler reaches the 16-byte
   AerVal with an addressing scale of 8 instead of a separate shift. */
static inline unsigned op_scaled_fields(Opcode op) {
    switch (op) {
#define OPCODE(name, handler, scaled, ...)     case OP_##name:                                 return scaled;
#include "opcodes.def"
#undef OPCODE
    default:
        return 0;
    }
}

/* op(8) | A(8) | B(8) | C(8), low byte first. Takes and gives back operands as the parser means them:
   PACK3 scales a slot field and OPERAND_A/B/C undo it, so only a handler ever sees the wire value. */
static inline uint32_t pack3(Opcode op, uint32_t a, uint32_t b, uint32_t cc) {
    unsigned scaled = op_scaled_fields(op);
    a <<= (scaled & SCALED_A) != 0;
    b <<= (scaled & SCALED_B) != 0;
    cc <<= (scaled & SCALED_C) != 0;
    return ((uint32_t)op & 0xFF) | ((a & 0xFF) << 8) | ((b & 0xFF) << 16) | ((cc & 0xFF) << 24);
}
#define PACK3(op, a, b, cc) pack3((Opcode)(op), (uint32_t)(a), (uint32_t)(b), (uint32_t)(cc))
#define PACK2(op, a, b) PACK3(op, a, b, 0)
#define PACK1(op, a) PACK3(op, a, 0, 0)
#define OPERAND_A(word) (UNPACK_A(word) >> ((op_scaled_fields((Opcode)((word) & 0xFF)) & SCALED_A) != 0))
#define OPERAND_B(word) (UNPACK_B(word) >> ((op_scaled_fields((Opcode)((word) & 0xFF)) & SCALED_B) != 0))
#define OPERAND_C(word) (UNPACK_C(word) >> ((op_scaled_fields((Opcode)((word) & 0xFF)) & SCALED_C) != 0))

/* Packs two independent 16-bit fields into one word -- used for word1-style "two wide fields,
   no room for anything else" shapes (e.g. field_idx + an RK16 operand). */
#define PACK_2X16(hi, lo) ((((uint32_t)(hi) & 0xFFFF) << 16) | ((uint32_t)(lo) & 0xFFFF))
#define UNPACK_2X16_HI(word) (((word) >> 16) & 0xFFFF)
#define UNPACK_2X16_LO(word) ((word) & 0xFFFF)

/* RK8: 1 flag bit + 7 index bits. A register index is always < FRAME_REGISTERS(128) by the time it
   reaches emission, so it fits with zero headroom; a constant-pool index past 127 must be hoisted
   into a scratch register first (parser.c's existing materialize(), unchanged). */
#define RK8_CONST_FLAG 0x80U
#define RK8_INDEX_MASK 0x7FU
#define RK8_MAX_INDEX 0x7F
static inline uint8_t pack_rk8(int rk) {
    if (rk & RK_CONST_FLAG)
        return (uint8_t)(RK8_CONST_FLAG | ((unsigned)(rk & ~RK_CONST_FLAG) & RK8_INDEX_MASK));
    return (uint8_t)((unsigned)rk & RK8_INDEX_MASK);
}
#define RK8_IS_CONST(b) ((b) & RK8_CONST_FLAG)
#define RK8_INDEX(b) ((b) & RK8_INDEX_MASK)

/* RK16: 1 flag bit + 15 index bits (32767 registers/constants direct) -- generous enough that no
   overflow path is needed anywhere it's used. */
#define RK16_CONST_FLAG (1U << 15)
#define RK16_INDEX_MASK 0x7FFFU
#define RK16_MAX_INDEX 0x7FFF
static inline uint16_t pack_rk16(int rk) {
    if (rk & RK_CONST_FLAG)
        return (uint16_t)(RK16_CONST_FLAG | ((unsigned)(rk & ~RK_CONST_FLAG) & RK16_INDEX_MASK));
    return (uint16_t)((unsigned)rk & RK16_INDEX_MASK);
}
#define RK16_IS_CONST(w) ((w) & RK16_CONST_FLAG)
#define RK16_INDEX(w) ((w) & RK16_INDEX_MASK)

/* type_name_idx / field_name_idx / module_idx / fn_idx / callee_offset / jump targets all get a
   full dedicated 32-bit word wherever this comment appears in the shapes below -- no packing, no
   guard needed, direct emit_u32-equivalent (a plain chunk_emit of the raw value). */

/* op(8) | a(8) | w16(16) -- one small field plus one 16-bit field, both in word0. Used by opcodes
   whose only two real fields are a register/small-count and one RK16/count16 value (OP_FIELD_SET,
   OP_INDEX_FIELD_SET's obj_reg+rk_idx half). */
#define PACK_OP_A_W16(op, a, w16) (PACK3(op, a, 0, 0) | (((uint32_t)(w16) & 0xFFFF) << 16))
#define UNPACK_W16(word) (((word) >> 16) & 0xFFFFU)

/* OP_DEFINE_STRUCT's header word: op(8) | name_idx(16) | field_count(8) -- name_idx sits in the
   middle (unlike PACK_OP_A_W16), so it gets its own macro rather than misusing that one. */
#define PACK_STRUCT_HEADER(name_idx, field_count)                                                            \
    (((uint32_t)(OP_DEFINE_STRUCT) & 0xFF) | (((uint32_t)(name_idx) & 0xFFFF) << 8) |                        \
     (((uint32_t)(field_count) & 0xFF) << 24))
#define UNPACK_STRUCT_HEADER_NAME(word) (((word) >> 8) & 0xFFFFU)
#define UNPACK_STRUCT_HEADER_COUNT(word) (((word) >> 24) & 0xFFU)

/* Module, function and builtin ids -- the stdlib's wire identity. */
#include "aer_abi.h"

/* Parts in one OP_INTERP. Bounds the builder's stack scratch; a longer interpolation compiles to
   the ordinary concatenate chain instead, which has no limit. */
/* OP_ITER_RANGE_PREP's item_dest operand is a whole 32-bit word for what is an 8-bit register
   index, so the spare high bits carry a flag. GUARD_NONNEG means this loop's body was compiled
   with UNCHECKED indexing on the strength of a compile-time proof that the start is >= 0
   (Parser.reg_nonneg), so PREP verifies that once here instead of the body checking every index.
   Only reachable when integer overflow defeated the proof -- see 5.27. */
#define RANGE_PREP_GUARD_NONNEG 0x80000000u
#define RANGE_PREP_ITEM_REG(w) ((int)((w) & 0xFFu))

#define INTERP_MAX_PARTS 16

/* Longest interpolated dict key OP_INDEX_GET_INTERP will build without allocating. Anything longer
   falls back to the allocating path -- the buffer lives in a noinline helper's frame, never
   vm_run_slice's (see 5.18). */
#define INTERP_KEY_MAX 256

/* Which runtime shape a shape-sensitive parameter arrived as. STRUCT and PACKED_ARRAY carry a
   structural guarantee -- a struct's shape never changes, a packed array cannot hold mixed shapes
   -- so they need no per-access recheck. ARRAY_OF_STRUCTS carries none: a plain array may hold
   heterogeneous elements, so every call must re-verify uniformity first. */
typedef enum {
    SPEC_KIND_STRUCT,
    SPEC_KIND_PACKED_ARRAY,
    SPEC_KIND_ARRAY_OF_STRUCTS,
} SpecKind;

/* One already-compiled specialized body for a shape-sensitive function (ChunkFunction.
   specializations below) -- keyed by the Shape observed for its shape-sensitive parameter(s) at
   the point specialization was triggered. Each specialized body has its OWN max_registers peak,
   independent of the generic body's. */
#define SPEC_MAX_RAW_PARAMS 3

typedef struct {
    Shape* shape;
    SpecKind kind;
    unsigned int code_offset;
    unsigned int max_registers;
    unsigned short frame_bounds;

    /* Optional second specialized body for this shape that also binds up to SPEC_MAX_RAW_PARAMS
       numeric parameters as raw locals. Declines silently to this entry's baseline body when it
       doesn't fit -- never touches `megamorphic` or the shape table.
       raw_param_count: 0 = never attempted, -1 = attempted and permanently declined, >0 = compiled
       for exactly raw_param_regs/raw_param_types, reused only while observed types still match. */
    int raw_param_count;
    int raw_param_regs[SPEC_MAX_RAW_PARAMS];
    ValueType raw_param_types[SPEC_MAX_RAW_PARAMS];
    unsigned int raw_variant_code_offset;
    unsigned int raw_variant_max_registers;
    unsigned short raw_variant_frame_bounds;
} SpecEntry;
#define SPEC_MAX 4

/* Runtime-visible function registration -- outlives the parser tables so cross-module calls
   can find exports by name after compilation (same precedent as chunk->shapes[]). */
typedef struct {
    unsigned int name; /* pool index of the function's name */
    unsigned int code_offset;
    unsigned int arity;
    unsigned int min_arity;
    AerVal* defaults; /* (arity - min_arity) owned values or NULL; freed by chunk_free */
    /* Real peak register need, patched in after the body compiles; the FRAME_REGISTERS
       placeholder (read only by in-body self-reference) is never an under-allocation. */
    unsigned int max_registers;
    /* Which slots of that frame frame entry has to touch -- see FRAME_BOUNDS. A body holding real
       slots gets a full-size frame because those slots sit at its top, and without this every call
       would tag all 128 of them and the collector would trace all 128 too. */
    unsigned short frame_bounds;

    /* Shape-specializing compilation (lazy, per-call-observed-shape recompiles) -- see
       vm_call_resolve_specialization (vm.c). Bit i set = parameter i was seen used as the base of a
       struct-field access (directly, or through a one-hop plain-local alias) during the ordinary
       compile; folded in at function-exit, same moment max_registers is captured. Zero means this
       function is never specialized, and h_call skips the lookup. */
    unsigned int shape_sensitive_mask;
/* Set instead of a parameter bit when no parameter is shape-sensitive but the body composed a raw
   local with a boxed value -- work that goes raw once the numeric parameters do. It rides in this
   mask rather than a field of its own so h_call's gate stays the one already-fetched test it is;
   parameter bits are capped at 31 to keep the top one free. */
#define SHAPE_MASK_NUMERIC_ONLY (1u << 31)
    /* Counted only for a numeric-only function, only until it specializes. A variant re-parses the
       body, so specializing on the first call loses outright for a function called once -- which
       bench/compile_bound.aer is a whole file of. */
    unsigned int numeric_call_count;
#define NUMERIC_SPECIALIZE_AFTER 16
    /* '(' through the end of the body, for re-invoking the parser long after parse() returned.
       NULL unless shape_sensitive_mask != 0. Owned, not a lexer pointer: aer_run_source replaces its
       one source buffer on the next call, which is exactly when a lazy specialization might fire. */
    char* source_span;
    unsigned int source_span_len;
    /* Absolute line of the span's first character, so a recompile's bytecode carries true line
       numbers rather than ones relative to the span. */
    unsigned int source_span_line;
    /* Small, bounded table of already-compiled specialized bodies, keyed by the shape observed for
       this function's shape-sensitive parameter(s). Checked (via the call site's own
       CallSpecCacheEntry first, then this table on a miss) before recompiling for a never-before-seen
       shape. megamorphic permanently stops specializing once the table fills, falling back to the
       generic body via code_offset above for every further call. */
    /* NULL until this function first specializes, which most never do. Inline, the SPEC_MAX
       entries were 83% of this struct (272 of 328 bytes) and spread every ChunkFunction across six
       cache lines for the handful of fields h_call reads per call. Allocated once at full
       SPEC_MAX size and never grown, so the SpecEntry* CallSpecCacheEntry caches stays valid. */
    SpecEntry* specializations;
    int specialization_count;
    bool megamorphic;
} ChunkFunction;

/* Bytecode chunk                                                       */
/* Flat 32-bit word array: one descriptor word (opcode + narrow packed fields) plus, per opcode's
   own fixed shape, zero or more trailing wide-field words -- see the fixed-width encoding comment
   above chunk_emit_word/read_word (vm.c). */

/* offset/ftype/narrow are pure functions of (shape, slot), cached here so a hit reads them from the
   entry it already touched for the shape check rather than indirecting through Shape again. */
typedef struct {
    Shape* shape;
    int slot;
    unsigned int offset;
    ValueType ftype;
    bool narrow;
} FieldCacheEntry;

/* The last shape seen at ONE call site, checked before falling into the callee's function-wide
   SpecEntry table. NULL last_shape = never populated. */
typedef struct {
    Shape* last_shape;
    unsigned int last_code_offset;
    unsigned int last_max_registers;
    unsigned short last_frame_bounds;
    /* ARRAY_OF_STRUCTS only: the array and its generation at the last homogeneity scan that fully
       succeeded, so a failed scan never poisons a later genuinely-uniform call. */
    AerArray* last_verified_array;
    unsigned int last_verified_generation;
    /* Which SpecEntry last_shape matched, so a hit here can still reach that entry's raw-variant
       fields. Stable for the chunk's life: specializations[] is fixed-size and never reallocated. */
    SpecEntry* last_entry;
} CallSpecCacheEntry;

typedef struct Chunk {
    uint32_t* code;
    unsigned int count, capacity;

    char* source_filename; /* owned; NULL for a chunk with no file */

    AerVal* pool; /* constants and variable names -- all deduplicated by value */
    unsigned int pool_count, pool_cap;

    /* Numeric constants as raw scalars, for the raw opcode families. Those opcodes name their
       operand type (OP_RAW_LT_INT is int by construction), so a const-flagged operand can
       index these directly -- no tag to check, and no encoding bit spent saying so. Kept separate
       from pool[] rather than replacing entries in it: a literal reached by both a raw and a boxed
       path needs both forms, and RK8's 7 index bits make a dense numeric-only table reach further. */
    int64_t* rawk_i;
    unsigned int rawk_i_count, rawk_i_cap;
    double* rawk_d;
    unsigned int rawk_d_count, rawk_d_cap;

    /* name -> pool index, for O(1) string dedup. Owns its keys, and the pools behind them. */
    HashTable name_index;
    HashPools name_index_pools;

    /* Appended to by OP_DEFINE_STRUCT. A redeclare appends rather than replaces, so old Shape
       pointers stay valid; chunk_find_shape searches newest-first. */
    Shape** shapes;
    unsigned int shape_count, shape_cap;

    /* Function registry -- appended by func_register, searched newest-first. */
    ChunkFunction* functions;
    unsigned int function_count, function_cap;

    /* Parse-time only, on the chunk so a later REPL line still sees an earlier line's import. */
    char** imported_modules;
    unsigned int import_count, import_cap;

    /* Offset -> source line, one entry per statement; strictly increasing (binary-searched);
       rolled back with bytecode on parse-error recovery. */
    unsigned int* line_mark_offsets;
    unsigned int* line_mark_lines;
    unsigned int line_mark_count, line_mark_cap;

    /* Per-site inline cache for FIELD_GET/SET: last Shape* + resolved slot. Monomorphic sites
       skip the name scan; polymorphic sites just miss. Shape* is never reallocated, so a cached
       pointer can't go stale. */
    FieldCacheEntry* field_cache;
    unsigned int field_cache_cap;

    /* Per-site inline cache for h_call's specialization dispatch, same growth/addressing idiom as
       field_cache above (sized to c->count, indexed by bytecode word offset). */
    CallSpecCacheEntry* call_spec_cache;
    unsigned int call_spec_cache_cap;

    /* Struct-name -> Shape*, keyed by POOL index: chunk_add_pool dedups strings, so every site
       building the same struct shares one. Saves a newest-first scan with a strcmp per shape, worth
       1.63% of binary_trees. Cleared wholesale when a shape is registered, so a redeclare cannot
       keep resolving to the shape it shadowed -- definitions are rare, constructions are not. */
    Shape** shape_by_name;
    unsigned int shape_by_name_cap;

    /* Per-word dispatch counters, allocated only when --debug-path asked for them; only opcode
       words increment. NULL is the signal DISPATCH() tests, so this must stay NULL otherwise. */
    uint64_t* debug_hits;
    unsigned int debug_hits_cap;
} Chunk;

/* Virtual machine                                                      */

#define VM_STACK_MAX 256
/* Non-tail call depth. The call stack and register bank grow on demand, about 2KB a frame, so this
   bounds what a runaway recursion can take rather than what every VM reserves. */
#define VM_CALL_MAX 10000

/* Per-call register frame; each VM owns its own chain, so a nested module VM gets its own. */
typedef struct {
    /* The anonymous union pads sizeof(CallFrame) up to 32 without raising its ALIGNMENT. _Alignas(32)
       was the obvious way to get that size and is wrong: the requirement propagates to struct VM,
       which aer_module.c -- and any embedder -- allocates with plain malloc, only 8-byte aligned.
       Every module VM was undefined behaviour; UBSan caught it. Only the size matters here. */
    union {
        struct {
            /* Bump-pointer base into vm->register_stack -- a call is a pointer add, not an
               allocation. */
            AerVal* registers;
            /* Registers THIS frame reserved (callee's compile-time peak; FRAME_REGISTERS for frame
               0) -- read by the next push. */
            unsigned int frame_size;

            unsigned int return_ip; /* where to resume in the CALLER */
            int dest_reg; /* which of the CALLER's registers gets the return value */

            unsigned int code_offset; /* this frame's entry point, for stack traces; unset on frame 0 */
            unsigned int tail_calls_collapsed; /* tail calls collapsed since this frame's last real push */
            bool synthetic_entry; /* set by setup_call() -- return_ip isn't a real caller line */
            unsigned short frame_bounds; /* FRAME_BOUNDS, above; fits the padding CallFrame had */
        };
        char size_is_a_power_of_two[32];
    };
} CallFrame;

/* Tags the slots a frame about to start running will actually use: the dynamically typed ones hold
   nothing yet, and the real ones at the top get the tag their unchecked opcodes then rely on --
   those store a payload and leave the tag exactly as this wrote it. The gap between the two is
   skipped, and frame_ref_slots stops before it, so a full-size frame costs no more here than the
   tight one it replaced. */
static inline void frame_init_tags(AerVal* registers, unsigned int from, unsigned short bounds) {
    for (unsigned int i = from, dyn_end = FRAME_DYN_END(bounds); i < dyn_end; i++)
        registers[i].tag = TYPE_NULL;
    /* Runs to FRAME_REGISTERS, not to the frame size, because only a frame that IS full size has
       real slots -- for any other, real_base is already FRAME_REGISTERS and this does nothing. That
       keeps the frame size off the call path entirely. */
    for (unsigned int i = FRAME_REAL_BASE(bounds); i < FRAME_REGISTERS; i++)
        registers[i].tag = TYPE_REAL;
}

/* The slots in this frame a heap reference can live in, for the GC to trace. Every slot is a tagged
   AerVal whatever its static type, so this stops only where nothing can reach: at the frame's
   dynamically typed peak, below both the untouched gap and the real slots above it. */
static inline unsigned int frame_ref_slots(CallFrame* frame, AerVal** out_slots) {
    unsigned int dyn_end = FRAME_DYN_END(frame->frame_bounds);
    *out_slots = frame->registers;
    /* Clamped, not trusted: a chunk built by hand rather than by the parser (the embedding tests do
       this) carries the placeholder bounds, which are wider than its frame. */
    return dyn_end < frame->frame_size ? dyn_end : frame->frame_size;
}
/* Indexing call_stack[] is `base + depth * sizeof(CallFrame)`, and at 44 bytes that compiled to a
   multiply plus a materialized constant per field on 32-bit ARM. A power of two makes it a shift. */
_Static_assert((sizeof(CallFrame) & (sizeof(CallFrame) - 1)) == 0,
               "CallFrame must stay a power of two -- see CALL_FRAME_PAD");

typedef struct VM {
    /* First, with call_stack beside it. A Thumb-2 `ldr` reaches a 12-bit displacement, so a field
       past 4095 bytes needs its offset materialized into a register first. Nothing mirrors the
       active frame's registers here: mark_vm_roots scans call_stack[f].registers directly, so a
       mirror would cost three stores per call and per return to serve no hot reader. */
    int call_depth;
    /* Frames the call stack can currently seat -- always <= VM_CALL_MAX. It stands in for the constant
       in the depth test every push already makes, and the cold side decides grow-or-overflow. */
    int call_depth_limit;
    CallFrame* call_stack;
    /* The highest base a new frame may start at: FRAME_REGISTERS short of the bank's end, the most any
       frame's tagging touches whatever its bounds say. A tail call reuses a base that already passed
       this test and the bank only grows, so pushes are the only thing that checks it. */
    AerVal* push_base_limit;

    Chunk* chunk;
    unsigned int ip;
    /* The yield budget, read and decremented only at the handful of checkpoints that can yield.
       Held here rather than in vm_run_slice locals so a handler reaches it through vm alone. */
    unsigned int slice_budget;
    unsigned int slice_max; /* 0 disables yielding entirely */

    /* For runtime error line lookup only. A pointer, not an offset: vm_run_slice writes it at
       every site that can raise, and an offset would keep `code` live across all of them. `ip`
       stays an offset because it must survive a yield/resume and a REPL reparse in between. */
    const uint32_t* error_pc;

    /* This VM's own heap -- every pool it allocates from, independent of every other VM's. */
    VmHeap heap;

    /* Seeded from the process-wide defaults at vm_init and at every aer_run_source call, so the
       documented "toggle off, run one thing, toggle back" pattern works on an existing VM. */
    bool io_enabled;
    bool net_enabled;

    /* Scratch argument channel for bridging out of the register convention (stdlib/module calls).
       Deliberately after every field the call path reads: its 4096 bytes in front of them would push
       their offsets past the 12-bit displacement window. Nothing in the call path touches it. */
    AerVal stack[VM_STACK_MAX];
    int stack_top;

    /* One value this VM holds onto between calls (actor.keep/actor.kept). Functions cannot reach a
       top-level variable, so without this an actor has nowhere to put data it wants to reuse and
       every call has to be handed it again -- which for a column meant copying it across the heap
       boundary every time. A GC root, and it outlives aer_vm_reset_for_reuse deliberately. */
    AerVal kept;

    /* One shared register bank for the whole chain (calls bump a base pointer). Grown on demand
       rather than reserved: inline at full size this was 93% of sizeof(VM), and every actor and
       every imported module allocates a VM. */
    AerVal* register_stack;
    size_t register_capacity; /* slots in register_stack */
} VM;

/* Frames the call stack seats, and full-size frames the bank holds, before either first grows. */
#define REGISTER_STACK_INITIAL_FRAMES 2

/* Bounds-checked push/pop for native-module files, outside vm_run's PUSH()/POP() macros. */
static inline bool vm_stack_push(VM* vm, AerVal v) {
    if (vm->stack_top >= VM_STACK_MAX) {
        error("Stack overflow");
        return false;
    }
    vm->stack[vm->stack_top++] = v;
    return true;
}

static inline AerVal vm_stack_pop(VM* vm) {
    if (vm->stack_top <= 0) {
        error("Stack underflow");
        return aer_null();
    }
    return vm->stack[--vm->stack_top];
}

void chunk_init(Chunk* c);
void chunk_free(Chunk* c);
void chunk_emit(Chunk* c, uint32_t word);

/* Bytecode from `offset` on belongs to `line`. Once per statement; a non-increasing offset is a
   no-op. */
void chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line);

/* The line whose statement contains `offset`, or 0 if there are no marks yet. */
unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset);

unsigned int chunk_add_pool(Chunk* c, AerVal v);

/* Interned index into Chunk.rawk_i/rawk_d -- see their comment on Chunk. */
unsigned int chunk_add_rawk_int(Chunk* c, int64_t v);
unsigned int chunk_add_rawk_real(Chunk* c, double v);

Shape* chunk_find_shape(Chunk* c, const char* name);

/* `defaults` is taken by ownership, never copied. */
void chunk_add_function(Chunk* c, unsigned int name_idx, unsigned int code_offset, unsigned int arity,
                        unsigned int min_arity, AerVal* defaults);
ChunkFunction* chunk_find_function(Chunk* c, const char* name);

/* Parse-time variant -- name_idx is a dedup'd pool index, so this is an int compare, no strcmp. */
ChunkFunction* chunk_find_function_by_name_idx(Chunk* c, unsigned int name_idx);

/* Recovers a frame's function from its code_offset, for stack traces. Cold path only. */
ChunkFunction* chunk_find_function_by_offset(Chunk* c, unsigned int code_offset);

/* `name` binds the module; `path_name` resolves to the file (dots as separators). Neither
   is NUL-terminated. */
bool chunk_add_import(Chunk* c, const char* name, unsigned int len, const char* path_name,
                      unsigned int path_len);
bool chunk_is_imported(Chunk* c, const char* name, unsigned int len);

/* Process-wide capability defaults, all true. io/net are defaults only: vm_init copies them into
   VM.io_enabled/net_enabled, and the runtime checks the per-VM field, so two VMs in one process can
   differ. import_enabled stays a directly-checked global because chunk_add_import runs at parse
   time with only a Chunk* in scope. Set via aer_set_*_enabled(). A blast-radius limiter, not a
   permission system -- see the README's Sandboxing note. */
extern bool aer_io_enabled;
extern bool aer_net_enabled;
extern bool aer_import_enabled;

void vm_init(VM* vm, Chunk* chunk);
void vm_free(VM* vm);

/* Same save/restore need as vm_current_heap, for the file-scope active_vm_for_errors global (vm.c)
   -- vm_init() unconditionally repoints it at the new VM before that VM ever runs. */
VM* vm_active_error_vm(void);
void vm_set_active_error_vm(VM* vm);

/* Runs from vm->ip to OP_HALT or runtime error (returns false). A host reusing the VM after
   a false return must reset stack_top/call_depth first -- see main.c's run(). */
bool vm_run(VM* vm);

typedef enum {
    VM_SLICE_DONE, /* reached OP_HALT */
    VM_SLICE_YIELDED, /* budget reached at a back-edge or call; vm->ip is a valid resume point */
    VM_SLICE_ERROR, /* runtime error, same as vm_run's false */
} VmSliceResult;

/* 0 = unlimited, which is what every caller but the scheduler passes. A nonzero budget counts loop
   back-edges and calls, then returns VM_SLICE_YIELDED with vm->ip at a valid resume point; calling
   again continues from there. */
VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions);

/* Cross-module call setup (aer_module_call only): pushes a real frame with dest_reg fixed at
   0 -- after the trampoline drains, the result is in call_stack[0].registers[0]. */
bool setup_call(VM* target, ChunkFunction* fn, int arg_count, AerVal* args, unsigned int return_ip);

/* An empty plain array with room for `capacity` items; 0 leaves items NULL. */
AerArray* vm_new_array(unsigned int capacity);

/* An array's item buffer at `capacity`, which both set as a->capacity and count toward the next
   collection -- the array's one cell says nothing about them. alloc is for a new array, whose items
   field is still garbage; grow is for an existing one. */
void vm_array_alloc_items(AerArray* a, unsigned int capacity);
void vm_array_grow_items(AerArray* a, unsigned int capacity);

/* Same, for AerDict -- exposed for json.decode(). The caller must zero-init `map` itself. */
AerDict* vm_new_dict(void);

/* Every store into an already-existing array goes through this. `index` feeds card marking, so a
   minor GC rescans only the slots dirtied since the last cycle. A freshly built, not-yet-returned
   array needs no barrier. */
void gc_barrier_array(VM* vm, AerArray* a, unsigned int index, AerVal new_value);

/* Same, for a struct field-set. No index: a struct's field count is small and fixed, so there is
   nothing for card marking to save. */
void gc_barrier_struct(VM* vm, AerStruct* s, AerVal new_value);

/* Same, for a dict entry. `index` is the DENSE index, which the caller resolves before the write. */
void gc_barrier_dict(VM* vm, AerDict* d, unsigned int index, AerVal new_value);

/* Called from vm.c's gc_maybe_collect once the threshold is actually crossed. gc_maybe_collect runs
   at hand-placed points in the allocating opcodes, not on every dispatch. */
void gc_run_collection_cycle(VM* vm);

/* Must come from function_pool (pool_mark's slab lookup fails on xmalloc'd cells); returns
   uninitialized memory -- zero it yourself. */
AerFunction* vm_new_function(void);

/* Test-only register readback (tests/smoke_test.c). */
AerVal register_get(VM* vm, int slot);

#include <stdio.h>
/* Byte-accurate per-pool memory breakdown. */
void aer_debug_memory_report(FILE* out);

/* Prints, in order: a full annotated disassembly of c->code (offset, opcode name, one-line
   description, decoded operands, and -- if c->debug_hits is populated -- a hit count and source
   line for that instruction); a per-opcode summary table (name -> total hits, sorted descending);
   and a per-source-line hot-spot rollup (line -> total hits, sorted descending). */
void aer_disassemble(Chunk* c, FILE* out);

/* Turns on the per-instruction counters DISPATCH() feeds. Must be called before the chunk runs;
   a run started without it never allocates the counters and never tests anything but a register. */
void aer_profile_enable(void);
bool aer_profile_is_enabled(void);

#endif
