#ifndef AER_CHUNK_H
#define AER_CHUNK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "encoding.h"
#include "hashtable.h"
#include "objects.h"
#include "value.h"

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
       vm_call_resolve_specialization (call.c). Bit i set = parameter i was seen used as the base of a
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

/* Flat 32-bit word array: one descriptor word (opcode + narrow packed fields) plus, per opcode's
   own fixed shape, zero or more trailing wide-field words -- see the fixed-width encoding comment
   above PACK3. */

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

/* Whether `import` is allowed; checked here at parse time, set through aer_set_import_enabled(). */
extern bool aer_import_enabled;

/* Prints, in order: a full annotated disassembly of c->code (offset, opcode name, one-line
   description, decoded operands, and -- if c->debug_hits is populated -- a hit count and source
   line for that instruction); a per-opcode summary table (name -> total hits, sorted descending);
   and a per-source-line hot-spot rollup (line -> total hits, sorted descending). */
void aer_disassemble(Chunk* c, FILE* out);

#endif
