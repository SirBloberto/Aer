#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_stdlib.h"
#include "error.h"
#include "typed_array.h"
#include "value_ops.h"
#include "vm.h"
#include "vm_internal.h"

/* Formats an interpolated dict key into a stack buffer and probes with the bytes, never building the
   AerString. Returns false (leaving *out alone) when that cannot work, and the caller falls back.
   Scalars render through aer_format_scalar, as in vm_interp_build, so a key written through OP_INTERP
   is found here; tests/test_interp_dict_keys.aer guards that. */
static bool vm_dict_get_interp(AerDict* d, const uint32_t* rks, unsigned int count,
                                                         AerVal* registers, AerVal* pool, AerVal* out) {
    char key[INTERP_KEY_MAX];
    unsigned int at = 0;
    for (unsigned int i = 0; i < count; i++) {
        AerVal v = *vm_rk_ptr16(registers, pool, rks[i]);
        const char* piece;
        unsigned int piece_len;
        char scratch[32];
        if (aer_type(v) == TYPE_STRING) {
            piece = aer_as_string(v)->data;
            piece_len = aer_as_string(v)->length;
        } else if (!aer_format_scalar(v, scratch, sizeof(scratch), &piece, &piece_len)) {
            return false; /* a collection part needs vm_to_str, which allocates */
        }
        if (at + piece_len > INTERP_KEY_MAX)
            return false;
        memcpy(key + at, piece, piece_len);
        at += piece_len;
    }
    unsigned int klen = hashtable_key_true_len(key, at);
    AerVal* found = hashtable_get_hashed(&d->map, key, klen, hashtable_hash_bytes(key, klen));
    *out = found ? *found : aer_null();
    return true;
}

/* OP_INTERP's builder. A part is rendered into `scratch` only if it isn't already a string;
   unbounded content defers to vm_to_str. */
static AerVal vm_interp_build(VM* vm, const AerVal* parts, unsigned int count) {
    const char* piece[INTERP_MAX_PARTS];
    unsigned int piece_len[INTERP_MAX_PARTS];
    char scratch[INTERP_MAX_PARTS][32];
    /* Only for parts that had to become a real string first; kept alive until the copy is done. */
    AerVal spilled[INTERP_MAX_PARTS];
    unsigned int spill_count = 0;
    unsigned int total = 0;

    for (unsigned int i = 0; i < count; i++) {
        AerVal v = parts[i];
        if (aer_type(v) == TYPE_STRING) {
            piece[i] = aer_as_string(v)->data;
            piece_len[i] = aer_as_string(v)->length;
        } else if (!aer_format_scalar(v, scratch[i], sizeof(scratch[i]), &piece[i], &piece_len[i])) {
            /* vm_to_str allocates; hold the result so a collection triggered by a later part cannot
               reclaim bytes this one still points at. */
            AerVal s = vm_to_str(vm->chunk, v);
            spilled[spill_count++] = s;
            piece[i] = aer_as_string(s)->data;
            piece_len[i] = aer_as_string(s)->length;
        }
        total += piece_len[i];
    }

    AerString* out = aer_string_alloc(total);
    char* buf;
    if (total <= AER_STRING_INLINE_MAX) {
        buf = out->inline_buf;
        out->data = buf;
    } else {
        buf = vm_string_payload_alloc(vm_require_current_heap(), total);
        out->data = buf;
    }
    unsigned int at = 0;
    for (unsigned int i = 0; i < count; i++) {
        memcpy(buf + at, piece[i], piece_len[i]);
        at += piece_len[i];
    }
    buf[total] = '\0';
    (void)spilled;
    return aer_string_val(out);
}


/* Field and element access -- struct, packed, typed                */

/* Resolves field_idx within shape via the per-site inline cache (keyed by bytecode offset) --
   shared by both boxed-struct and packed-array callers, since a field's slot within a given
   Shape is identical either way. False (error reported) if shape has no such field. */
static inline __attribute__((always_inline)) bool
vm_resolve_field_by_shape(Chunk* c, unsigned int site, Shape* shape, int field_idx, int* out_slot,
                          unsigned int* out_offset, ValueType* out_ftype, bool* out_narrow) {
    FieldCacheEntry* entry = &c->field_cache[site];
    if (entry->shape == shape) {
        *out_slot = entry->slot;
        *out_offset = entry->offset;
        *out_ftype = entry->ftype;
        *out_narrow = entry->narrow;
        return true;
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            entry->shape = shape;
            entry->slot = (int)i;
            entry->offset = shape->field_offsets[i];
            entry->ftype = shape->field_types[i];
            entry->narrow = shape->field_narrow[i];
            *out_slot = (int)i;
            *out_offset = entry->offset;
            *out_ftype = entry->ftype;
            *out_narrow = entry->narrow;
            return true;
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    return false;
}

/* Resolves struct_reg's field to an (AerStruct*, slot, offset, ftype, narrow) tuple, delegating the
   shape+cache lookup above. Shared by every field-access opcode. False (error reported) if not a
   struct instance or no such field. */
static inline __attribute__((always_inline)) bool vm_resolve_field(AerVal* registers, Chunk* c,
                                                                   unsigned int site, int struct_reg,
                                                                   int field_idx, AerStruct** out_s,
                                                                   int* out_slot, unsigned int* out_offset,
                                                                   ValueType* out_ftype, bool* out_narrow) {
    AerVal* obj = &registers[struct_reg];
    if (obj->tag != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        return false;
    }
    AerStruct* s = (AerStruct*)obj->as.ptr;
    *out_s = s;
    return vm_resolve_field_by_shape(c, site, s->shape, field_idx, out_slot, out_offset, out_ftype,
                                     out_narrow);
}

/* vm_dict_get_interp's fallback: builds the key string for real, then takes the general index
   path. */
static AerVal vm_index_get_interp_slow(VM* vm, AerVal obj, const uint32_t* rks,
                                                                 unsigned int count, AerVal* registers,
                                                                 AerVal* pool) {
    AerVal parts[INTERP_MAX_PARTS];
    for (unsigned int i = 0; i < count; i++)
        parts[i] = *vm_rk_ptr16(registers, pool, rks[i]);
    AerVal out;
    vm_index_get_compute(obj, vm_interp_build(vm, parts, count), &out);
    return out;
}

/* Shared by h_index_set and the fused index-set handlers -- same dispatch/bounds/errors.
   Caller must DISPATCH() immediately after. */
static inline void vm_index_set_compute(VM* vm, AerVal obj, AerVal idx, AerVal val) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) {
            return error("Struct fields are assigned with '.', not '[]'");
        }
        uint64_t i;
        if (!vm_array_position(idx, a->count, &i))
            return vm_array_index_error(idx, a->count);
        gc_barrier_array(vm, a, (unsigned int)i, val);
        a->items[i] = val;
        /* Replacing an element can change whether this array is uniformly one struct shape --
           invalidates h_call's SPEC_KIND_ARRAY_OF_STRUCTS "already verified homogeneous"
           per-call-site cache (call.c), which is keyed on (array pointer, generation, shape). */
        a->generation++;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) {
            return error("Hashtable key must be a string");
        }
        AerString* is = aer_as_string(idx);
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        HashValue khash = hashtable_string_hash(is, klen);
        AerDict* d = aer_as_dict(obj);
        /* Resolved BEFORE the write, not after -- an update reuses this exact dense index, a fresh
           key always lands at d->map.count (see gc_barrier_dict's own comment, gc.c). */
        int existing_idx = hashtable_get_index_hashed(&d->map, is->data, klen, khash);
        unsigned int write_idx = existing_idx >= 0 ? (unsigned int)existing_idx : d->map.count;
        gc_barrier_dict(vm, d, write_idx, val);
        if (existing_idx >= 0) {
            d->map.dense[existing_idx].payload = val; /* update in place -- no allocation */
        } else {
            char* k = hashtable_key_dup_known(d->map.pools, is->data, klen);
            hashtable_put_hashed(&d->map, k, klen, khash, val);
        }
    } else if (aer_type(obj) == TYPE_STRING) {
        error("Strings are immutable — cannot assign to an index");
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        uint64_t i;
        if (!vm_array_position(idx, ta->count, &i))
            return vm_array_index_error(idx, ta->count);
        if (!vm_typed_array_check(vm->chunk, ta->elem_kind, val))
            return;
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        vm_typed_elem_write(ta->data + (size_t)i * width, ta->elem_kind, val);
    } else {
        error("This value cannot be assigned to by index — an array, a column or a hashtable can");
    }
}

/* One function per opcode, each ending in its own indirect jump to the next -- see DISPATCH. The
   predictor learns per-opcode patterns instead of funnelling every opcode through one switch. */

#ifdef AER_LAYOUT_PAD
#define AER_PAD_STR2(x) #x
#define AER_PAD_STR(x) AER_PAD_STR2(x)
__attribute__((used, noinline)) static void aer_layout_pad_fn(void) {
    __asm__ volatile(".space " AER_PAD_STR(AER_LAYOUT_PAD));
}
#endif

/* Every opcode is its own function, tail-calling the next. That makes the handler the unit the
   compiler allocates registers for and the unit perf reports, rather than one body whose live
   ranges span 150 blocks. The four parameters are the only values measured to earn a register;
   everything else is reached through vm or c. */
typedef VmSliceResult (*OpHandler)(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c);
static const OpHandler aer_handlers[256];

/* Deliberately not a hoisted local. Every use is on a boxed path, while keeping it in a
   register costs the raw paths one they need more -- the same trade that made hoisting
   rawk_i/rawk_d expensive. `c` is live regardless, and reading through it also means a
   specialization recompile that moves the pool needs no refresh here. */
#define const_pool (c->pool)

/* pc points past word0 on entry, and op_word is captured before any handler moves pc. */
#define HANDLER(name)                                                                                        \
    static VmSliceResult h_##name(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {                 \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;

/* noinline, or the compiler folds it back into its tail caller along with the registers its calls save. */
#define SEPARATE_HANDLER(name) __attribute__((noinline)) HANDLER(name)
/* The register a doubled field names: its byte offset is the field times 8, which a load folds. */
#define SCALED_REG(field) (*(AerVal*)((char*)registers + (size_t)(field) * 8))

#define READ() (*pc++)
#define PUSH(v)                                                                                              \
    do {                                                                                                     \
        if (vm->stack_top >= VM_STACK_MAX) {                                                                 \
            error("Stack overflow");                                                                         \
            return VM_SLICE_ERROR;                                                                           \
        }                                                                                                    \
        vm->stack[vm->stack_top++] = (v);                                                                    \
    } while (0)
#define POP() (vm->stack_top > 0 ? vm->stack[--vm->stack_top] : (error("Stack underflow"), aer_null()))
/* Neither a GC nor an error poll here: allocating labels call gc_maybe_collect themselves right
   after storing their result into a VM-visible root, and error() longjmps straight to this call's
   catch_point rather than setting a flag every dispatch would have to test. */
/* Full 8-bit mask -- opcode is unambiguously its own byte now (OP_OPCODE_COUNT_MARKER's static
   assert guarantees <=256), no reason to ever mask narrower. */
/* Replicated on purpose: every handler ends with its own indirect branch, so the predictor learns
   per-opcode successor patterns instead of one shared branch guessing among 150 targets.
   musttail is required rather than preferred -- an ordinary call here would grow the stack once
   per instruction executed. */
#define DISPATCH()                                                                                           \
    do {                                                                                                     \
        AER_PROFILE_HIT();                                                                                   \
        uint32_t next_word = *pc++;                                                                          \
        __attribute__((musttail)) return aer_handlers[next_word & 0xFF](vm, pc, registers, c);               \
    } while (0)

#ifdef AER_PROFILE
#define AER_PROFILE_HIT()                                                                                    \
    do {                                                                                                     \
        if (__builtin_expect(c->debug_hits != NULL, 0))                                                      \
            c->debug_hits[pc - c->code]++;                                                                   \
    } while (0)
#else
#define AER_PROFILE_HIT() ((void)0)
#endif

#define SYNC_IP() ((void)(vm->error_pc = pc))
#define error(...) (SYNC_IP(), (error)(__VA_ARGS__))
#define vm_binary_cold(...) (SYNC_IP(), (vm_binary_cold)(__VA_ARGS__))
#define vm_index_get_compute(...) (SYNC_IP(), (vm_index_get_compute)(__VA_ARGS__))
#define vm_index_set_compute(...) (SYNC_IP(), (vm_index_set_compute)(__VA_ARGS__))
#define vm_array_index_error(...) (SYNC_IP(), (vm_array_index_error)(__VA_ARGS__))
#define vm_in(...) (SYNC_IP(), (vm_in)(__VA_ARGS__))
#define vm_cast(...) (SYNC_IP(), (vm_cast)(__VA_ARGS__))
#define vm_to_str(...) (SYNC_IP(), (vm_to_str)(__VA_ARGS__))
#define vm_slice_bounds(...) (SYNC_IP(), (vm_slice_bounds)(__VA_ARGS__))
#define vm_call_builtin(...) (SYNC_IP(), (vm_call_builtin)(__VA_ARGS__))
#define vm_call_value(...) (SYNC_IP(), (vm_call_value)(__VA_ARGS__))
#define vm_call_module_dispatch(...) (SYNC_IP(), (vm_call_module_dispatch)(__VA_ARGS__))
#define vm_call_resolve_specialization(...) (SYNC_IP(), (vm_call_resolve_specialization)(__VA_ARGS__))
#define vm_resolve_field(...) (SYNC_IP(), (vm_resolve_field)(__VA_ARGS__))
#define vm_resolve_field_by_shape(...) (SYNC_IP(), (vm_resolve_field_by_shape)(__VA_ARGS__))
#define vm_check_narrow_field_write(...) (SYNC_IP(), (vm_check_narrow_field_write)(__VA_ARGS__))
#define vm_typed_array_check(...) (SYNC_IP(), (vm_typed_array_check)(__VA_ARGS__))
#define vm_default_value(...) (SYNC_IP(), (vm_default_value)(__VA_ARGS__))
#define aer_make_string_copy(...) (SYNC_IP(), (aer_make_string_copy)(__VA_ARGS__))
/* Only the collection path syncs: the threshold test runs on every allocating opcode. */
#define gc_maybe_collect(v)                                                                                  \
    do {                                                                                                     \
        if (gc_should_collect(v)) {                                                                          \
            SYNC_IP();                                                                                       \
            gc_run_collection_cycle(v);                                                                      \
        }                                                                                                    \
    } while (0)


HANDLER(jump)
    int target = READ();
    pc += (int32_t)target;
    /* Every loop's back-edge (while/for/plain jump alike) goes through here -- the one checkpoint
       that bounds an AER-level loop's slice length. pc already points at a complete instruction
       (this jump's own operand is fully consumed), so yielding here is always resumable. */
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

HANDLER(define_struct)
    int name_idx = (int)UNPACK_STRUCT_HEADER_NAME(op_word);
    int field_count = (int)UNPACK_STRUCT_HEADER_COUNT(op_word);
    Shape* shape = xmalloc(sizeof(Shape));
    shape->name = (unsigned int)name_idx;
    shape->field_count = (unsigned int)field_count;
    for (int i = 0; i < field_count; i++) {
        uint32_t name_default_word = READ();
        shape->field_names[i] = UNPACK_2X16_HI(name_default_word);
        shape->field_defaults[i] = c->pool[UNPACK_2X16_LO(name_default_word)];
        /* Low byte is the ValueType tag, bit 0x100 is the narrow (`i`/`f`-suffixed-literal) marker
           -- see emit_struct_field_type/parse_struct's own comment (parser.c). */
        uint32_t ftype_word = READ();
        shape->field_types[i] = (ValueType)(ftype_word & 0xFF);
        shape->field_narrow[i] = (ftype_word & 0x100) != 0;
    }
    /* Typed fields get 8 raw bytes (no tag -- the type is this Shape's own static knowledge), or 4
       if narrow (int32/float32); TYPE_ANY fields get a full boxed AerVal (16 bytes), since they can
       hold a reference type the GC must trace. See vm_struct_field_read/write. */
    {
        unsigned int offset = 0;
        for (unsigned int i = 0; i < shape->field_count; i++) {
            shape->field_offsets[i] = offset;
            offset += (shape->field_types[i] == TYPE_ANY) ? sizeof(AerVal) : (shape->field_narrow[i] ? 4 : 8);
        }
        shape->instance_bytes = offset;
    }
    if (c->shape_count >= c->shape_cap) {
        c->shape_cap = c->shape_cap ? c->shape_cap * 2 : 4;
        c->shapes = xrealloc(c->shapes, sizeof(Shape*) * c->shape_cap);
    }
    c->shapes[c->shape_count++] = shape;
    /* A redeclare must not keep resolving to the Shape it shadows -- see Chunk.shape_by_name. */
    if (c->shape_by_name)
        memset(c->shape_by_name, 0, sizeof(Shape*) * c->shape_by_name_cap);
    DISPATCH();
}

/* dest+pool_idx both fit in word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word needed,
   down from the original design's 2-word form. */
HANDLER(loadk)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int pool_idx = UNPACK_W16(op_word);
    SCALED_REG(dest) = c->pool[pool_idx];
    DISPATCH();
}

HANDLER(move)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int src = UNPACK_B(op_word);
    SCALED_REG(dest) = SCALED_REG(src);
    DISPATCH();
}

HANDLER(is_result)
    int dest = (int)UNPACK_A(op_word);
    int src = (int)UNPACK_B(op_word);
    registers[dest] = aer_bool(aer_type(registers[src]) == TYPE_RESULT);
    DISPATCH();
}

/* Dest + both RK operands already in op_word -- no further READ(). Each operator is its own
   top-level dispatch target, checks its own fast path, and falls back to vm_binary_cold() for
   anything else. BINARY_OP_INT_REAL has both fast paths; BINARY_OP_INT_ONLY (bitwise/shift) has
   no real/real meaning. */
/* `result` points straight at the destination register -- building a local AerVal and copying
   it measured as a 16-byte stack round-trip that was the single hottest instruction in the
   nbody profile. Safe even when dest aliases ra/rb (l/rv already snapshotted by value). */
/* Only the _any_values handlers collect: an int/int or real/real result never allocates. */
#define BINARY_OP_ANY_VALUES(NAME, OPENUM)                                                                   \
    SEPARATE_HANDLER(NAME##_any_values)                                                                      \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        registers[UNPACK_A(op_word)] = vm_binary_cold(c, *ra, *rb, OPENUM, ra->tag, rb->tag);                \
        gc_maybe_collect(vm);                                                                                \
        DISPATCH();                                                                                          \
    }
#define BINARY_OP_INT_REAL(NAME, OPENUM, INT_STMT, REAL_STMT)                                                \
    BINARY_OP_ANY_VALUES(NAME, OPENUM)                                                                       \
    static VmSliceResult h_##NAME(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {                 \
        const uint32_t op_word = pc[-1];                                                                     \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        ValueType ta = ra->tag, tb = rb->tag;                                                                \
        AerVal* result = &registers[dest];                                                                   \
        if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) {                                                      \
            int64_t l = ra->as.i, rv = rb->as.i;                                                             \
            INT_STMT                                                                                         \
        } else if (ta == TYPE_REAL && tb == TYPE_REAL) {                                                     \
            double l = ra->as.d, rv = rb->as.d;                                                              \
            REAL_STMT                                                                                        \
        } else {                                                                                             \
            __attribute__((musttail)) return h_##NAME##_any_values(vm, pc, registers, c);                    \
        }                                                                                                    \
        DISPATCH();                                                                                          \
    }
#define BINARY_OP_INT_ONLY(NAME, OPENUM, INT_STMT)                                                           \
    BINARY_OP_ANY_VALUES(NAME, OPENUM)                                                                       \
    static VmSliceResult h_##NAME(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {                 \
        const uint32_t op_word = pc[-1];                                                                     \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        AerVal* result = &registers[dest];                                                                   \
        if (ra->tag == TYPE_INTEGER && rb->tag == TYPE_INTEGER) {                                            \
            int64_t l = ra->as.i, rv = rb->as.i;                                                             \
            INT_STMT                                                                                         \
        } else {                                                                                             \
            __attribute__((musttail)) return h_##NAME##_any_values(vm, pc, registers, c);                    \
        }                                                                                                    \
        DISPATCH();                                                                                          \
    }

    BINARY_OP_INT_REAL(add, OP_ADD, { *result = aer_int(l + rv); }, { *result = aer_real(l + rv); })
    BINARY_OP_INT_REAL(sub, OP_SUB, { *result = aer_int(l - rv); }, { *result = aer_real(l - rv); })
    BINARY_OP_INT_REAL(mul, OP_MUL, { *result = aer_int(l * rv); }, { *result = aer_real(l * rv); })
    BINARY_OP_INT_REAL(
        div, OP_DIV,
        {
            if (rv == 0) {
                error("Division by zero");
                *result = aer_int(0);
            } else {
                *result = aer_real((double)l / (double)rv);
            }
        },
        {
            if (rv == 0.0) {
                error("Division by zero");
                *result = aer_real(0.0);
            } else {
                *result = aer_real(l / rv);
            }
        })
    BINARY_OP_INT_REAL(
        floor_div, OP_FLOOR_DIV,
        {
            if (rv == 0) {
                error("Division by zero");
                *result = aer_int(0);
            } else {
                *result = aer_int((int64_t)floor((double)l / (double)rv));
            }
        },
        {
            if (rv == 0.0) {
                error("Division by zero");
                *result = aer_real(0.0);
            } else {
                *result = aer_real(floor(l / rv));
            }
        })
    BINARY_OP_INT_REAL(
        mod, OP_MOD,
        {
            if (rv == 0) {
                error("Modulo by zero");
                *result = aer_int(0);
            } else {
                *result = aer_int(aer_mod_int64(l, rv));
            }
        },
        {
            if (rv == 0.0) {
                error("Modulo by zero");
                *result = aer_real(0.0);
            } else {
                *result = aer_real(aer_mod_double(l, rv));
            }
        })
    BINARY_OP_INT_REAL(eq, OP_EQ, { *result = aer_bool(l == rv); }, { *result = aer_bool(l == rv); })
    BINARY_OP_INT_REAL(neq, OP_NEQ, { *result = aer_bool(l != rv); }, { *result = aer_bool(l != rv); })
    BINARY_OP_INT_REAL(lt, OP_LT, { *result = aer_bool(l < rv); }, { *result = aer_bool(l < rv); })
    BINARY_OP_INT_REAL(gt, OP_GT, { *result = aer_bool(l > rv); }, { *result = aer_bool(l > rv); })
    BINARY_OP_INT_REAL(lte, OP_LTE, { *result = aer_bool(l <= rv); }, { *result = aer_bool(l <= rv); })
    BINARY_OP_INT_REAL(gte, OP_GTE, { *result = aer_bool(l >= rv); }, { *result = aer_bool(l >= rv); })
    BINARY_OP_INT_ONLY(bitwise_and, OP_BITWISE_AND, { *result = aer_int(l & rv); })
    BINARY_OP_INT_ONLY(bitwise_or, OP_BITWISE_OR, { *result = aer_int(l | rv); })
    BINARY_OP_INT_ONLY(bitwise_xor, OP_BITWISE_XOR, { *result = aer_int(l ^ rv); })
    BINARY_OP_INT_ONLY(lshift, OP_LSHIFT, { *result = aer_int(l << rv); })
    BINARY_OP_INT_ONLY(rshift, OP_RSHIFT, { *result = aer_int(l >> rv); })

#undef BINARY_OP_INT_REAL
#undef BINARY_OP_INT_ONLY
#undef BINARY_OP_ANY_VALUES

/* Fused comparison-and-branch (vm.h's own comment on OP_LT_JUMP_IF_FALSE et al. has the full
   rationale) -- no destination register at all, the truth value is consumed immediately by the
   branch decision below instead of being written out and read back. The cold (non-int/non-real)
   path skips gc_maybe_collect() unlike BINARY_OP_INT_REAL's shared cold path above -- a boolean
   result never allocates, on any operand type, so there is nothing here for a collection to ever
   need to run for. */
#define CMP_JUMP_IF_FALSE(NAME, OPENUM, INT_CMP, REAL_CMP)                                                   \
    SEPARATE_HANDLER(NAME##_jump_if_false_any_values)                                                        \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        int target = READ();                                                                                 \
        if (!vm_truthy(vm_binary_cold(c, *ra, *rb, OPENUM, ra->tag, rb->tag)))                               \
            pc += (int32_t)target;                                                                           \
        DISPATCH();                                                                                          \
    }                                                                                                        \
    static VmSliceResult h_##NAME##_jump_if_false(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) { \
        const uint32_t op_word = pc[-1];                                                                     \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        ValueType ta = ra->tag, tb = rb->tag;                                                                \
        bool cond;                                                                                           \
        if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) {                                                      \
            int64_t l = ra->as.i, rv = rb->as.i;                                                             \
            cond = (INT_CMP);                                                                                \
        } else if (ta == TYPE_REAL && tb == TYPE_REAL) {                                                     \
            double l = ra->as.d, rv = rb->as.d;                                                              \
            cond = (REAL_CMP);                                                                               \
        } else {                                                                                             \
            __attribute__((musttail)) return h_##NAME##_jump_if_false_any_values(vm, pc, registers, c);      \
        }                                                                                                    \
        int target = READ();                                                                                 \
        if (!cond)                                                                                           \
            pc += (int32_t)target;                                                                           \
        DISPATCH();                                                                                          \
    }

    CMP_JUMP_IF_FALSE(eq, OP_EQ, l == rv, l == rv)
    CMP_JUMP_IF_FALSE(neq, OP_NEQ, l != rv, l != rv)
    CMP_JUMP_IF_FALSE(lt, OP_LT, l < rv, l < rv)
    CMP_JUMP_IF_FALSE(gt, OP_GT, l > rv, l > rv)
    CMP_JUMP_IF_FALSE(lte, OP_LTE, l <= rv, l <= rv)
    CMP_JUMP_IF_FALSE(gte, OP_GTE, l >= rv, l >= rv)
#undef CMP_JUMP_IF_FALSE

/* No int/int or real/real fast path -- dispatches straight to the shared vm_in(). */
HANDLER(in)
    int dest = (int)UNPACK_A(op_word);
    AerVal a = *vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    AerVal b = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    registers[dest] = vm_in(a, b);
    DISPATCH();
}

/* Control flow -- stack-neutral, reads only registers[]/the pool. */
HANDLER(jump_if_false_reg)
    unsigned int reg = UNPACK_A(op_word);
    int target = READ();
    if (!vm_truthy(SCALED_REG(reg)))
        pc += (int32_t)target;
    DISPATCH();
}

/* Bulk-copies args from the caller's bank into the new frame's bank at register 0; overflow
   check mirrors vm_setup_call's own ceiling. */
HANDLER(call)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int callee_offset = READ();
    /* The target's BYTE offset into c->functions[] -- always present (emit_call always emits it,
       patched in later by func_register for a forward reference), letting this size the callee's
       frame from its real max_registers peak instead of a flat,
       function-agnostic ceiling. A byte offset rather than an index because ChunkFunction is not a
       power of two, so indexing cost a multiply on every call. */
    unsigned int func_byte_offset = (unsigned int)READ();
    ChunkFunction* target_f = (ChunkFunction*)((char*)c->functions + func_byte_offset);
    /* `registers` is the hoisted copy of this same frame's base -- every site that changes frames
       reassigns it -- so reading it back out of the frame is a redundant dependent load on the
       hottest path in the interpreter. Resolution below never touches the bank, so the base is
       final here. */
    AerVal* callee_regs = registers + vm->call_stack[vm->call_depth].frame_size;
    if (vm->call_depth + 1 >= vm->call_depth_limit || callee_regs > vm->push_base_limit) {
        if (!vm_grow_for_push(vm, callee_regs)) {
            error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
            DISPATCH();
        }
        /* Nothing is written yet, so the call reruns from its first word against the grown bank.
           Rebasing locals here instead keeps them live through the handler, and every call pays
           for that in spilled registers. */
        __attribute__((musttail)) return h_call(vm, pc - 2, vm->call_stack[vm->call_depth].registers, c);
    }

    /* Gated on shape_sensitive_mask (zero for most functions, one already-fetched field) rather than
       on a distinct call opcode: shape-sensitivity is only known once the whole body has compiled,
       but forward-referenced and self-recursive calls need their opcode chosen before that. */
    unsigned int chosen_offset = (unsigned int)callee_offset;
    unsigned int chosen_max_registers = target_f->max_registers;
    unsigned short chosen_frame_bounds = target_f->frame_bounds;

    if (!target_f->megamorphic && target_f->shape_sensitive_mask != 0) {
        /* The out-params are scoped to this branch on purpose. Taking the address of the chosen_*
           locals themselves forces both into memory for the WHOLE handler -- an address that escapes
           cannot live in a register -- so every ordinary call paid stores and address computations
           for a path it never takes. */
        unsigned int spec_offset = chosen_offset, spec_registers = chosen_max_registers;
        unsigned short spec_bounds = chosen_frame_bounds;
        unsigned int resume_at = (unsigned int)(pc - c->code);
        vm_call_resolve_specialization(c, target_f, registers, arg_reg_base, resume_at, &spec_offset,
                                       &spec_registers, &spec_bounds);
        chosen_offset = spec_offset;
        chosen_max_registers = spec_registers;
        chosen_frame_bounds = spec_bounds;
        /* Compiling a specialized body can realloc any of the chunk's growable arrays, so pc is
           rebuilt from the chunk rather than kept across the call. Everything else the loop reads
           already goes through c, so a move costs nothing to follow. */
        pc = c->code + resume_at;
    }

    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = caller + 1;
    /* Every scalar field first, so pc, c, dest_reg and chosen_offset stop being live across the
       two loops below. The compiler cannot sink them itself: call_stack and the register bank are
       separate allocations, but nothing in the types says so. */
    callee->registers = callee_regs;
    callee->frame_size = chosen_max_registers;
    callee->frame_bounds = chosen_frame_bounds;
    callee->return_ip =
        (unsigned int)(pc - c->code); /* already past this instruction's operands -- the correct resume point */
    callee->dest_reg = dest_reg;
    callee->code_offset = chosen_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = registers[arg_reg_base + i];
    /* mark_vm_roots traces every register below frame_size, so an unfilled one would still hold a
       popped frame's pointer. Only the tag matters -- value_has_cell reads nothing else. */
    frame_init_tags(callee_regs, (unsigned int)arg_count, chosen_frame_bounds);
    vm->call_depth++;
    registers = callee_regs;
    pc = c->code + chosen_offset;
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* Its own label, so an ordinary call carries none of this in its live range. Reuses the current
   frame rather than pushing, so none of h_call's sizing work applies. */
HANDLER(tail_call)
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int callee_offset = READ();
    /* Same byte-offset operand as h_call's -- emit_call produces both. */
    ChunkFunction* target_f = (ChunkFunction*)((char*)c->functions + (unsigned int)READ());
    /* The copy iterates with dest_i always <= source_i, the same always-safe-forward-shift pattern
       memmove uses when dest <= src, so no overlap special-casing is needed. */
    for (int i = 0; i < arg_count; i++)
        registers[i] = registers[arg_reg_base + i];
    pc = c->code + callee_offset;
    CallFrame* reused = &vm->call_stack[vm->call_depth];
    reused->code_offset = (unsigned int)callee_offset; /* reused frame now runs a different function */
    /* The reused frame's sizing belongs to whatever function last occupied it, and a tail call can
       land on one with a different peak. Left stale, a later real call from inside this frame
       computes its child's base from the wrong frame_size and overlaps still-live slots. The base
       pointers are untouched -- same frame, same memory, only the claim changes. */
    reused->frame_size = target_f->max_registers;
    /* Same as the non-tail push: the block's tags belong to the function now running here. */
    frame_init_tags(registers, (unsigned int)arg_count, target_f->frame_bounds);
    reused->frame_bounds = target_f->frame_bounds;
    reused->tail_calls_collapsed++;
    /* Every call (tail or not) is the other place a script can spend unbounded time (recursion
       instead of a loop) -- checked once pc already points at the callee's real entry point, so a
       yield here always resumes at a valid instruction boundary. */
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* Same frame-push shape as h_call, but the target is a runtime AerFunction read from a register.
   3 registers fit word0 (PACK3); callee_reg is the 4th and gets its own trailing word (no room
   left in a 32-bit word0 for a 4th 8-bit field alongside the opcode). */
HANDLER(call_value)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int callee_reg = (int)READ();
    /* vm_call_value writes vm->ip on success (or leaves it untouched on error) -- reload before
       the next READ(). pc is already past callee_reg's word, the correct resume address. */
    vm_call_value(vm, registers[callee_reg], dest_reg, arg_reg_base, arg_count, (op_word & 0xFF) == OP_TAIL_CALL_VALUE,
                  (unsigned int)(pc - c->code));
    pc = c->code + vm->ip;
    /* vm_call_value may have pushed a frame (non-tail) or reused this one (tail) -- refresh from
       whichever is now current, either way. */
    registers = vm->call_stack[vm->call_depth].registers;
    DISPATCH();
}

/* return_ip/dest_reg live in the callee's own frame, not a shared global -- what makes
   nested/recursive calls safe. */
HANDLER(return)
    int src_reg = (int)UNPACK_A(op_word);
    CallFrame* callee = &vm->call_stack[vm->call_depth];

    AerVal result = callee->registers[src_reg];
    unsigned int return_ip = callee->return_ip;
    int dest_reg = callee->dest_reg;
    vm->call_depth--;
    /* Refreshed BEFORE the write below -- registers still pointed at the callee's (now-popped)
       frame otherwise, corrupting whichever register of the CALLER's frame happens to share
       dest_reg's index instead of writing the return value where the caller expects it. */
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    registers = caller->registers;
    registers[dest_reg] = result;
    pc = c->code + return_ip;
    DISPATCH();
}

static VmSliceResult h_call_self(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c);

/* The push that does not fit. Separate because vm_grow_for_push and error() are the only calls
   h_call_self makes, and one call is what gives the whole handler a stack frame -- a recursive body
   runs the push path millions of times and this path almost never. */
SEPARATE_HANDLER(call_self_grow)
    AerVal* callee_regs = registers + vm->call_stack[vm->call_depth].frame_size;
    if (!vm_grow_for_push(vm, callee_regs)) {
        error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
        DISPATCH();
    }
    /* Same rerun as h_call's, and for the same reason. */
    __attribute__((musttail)) return h_call_self(vm, pc, vm->call_stack[vm->call_depth].registers, c);
}

/* See OP_CALL_SELF (vm.h). The frame is the caller's own size and entry point, both read from the
   caller frame rather than resolved, so this is a push and nothing else. */
HANDLER(call_self)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = caller + 1;
    unsigned int fsz = caller->frame_size, entry = caller->code_offset;
    unsigned short bounds = caller->frame_bounds;
    AerVal* callee_regs = registers + fsz;
    if (vm->call_depth + 1 >= vm->call_depth_limit || callee_regs > vm->push_base_limit)
        __attribute__((musttail)) return h_call_self_grow(vm, pc, registers, c);
    /* Every scalar field first, so pc, c, dest_reg and entry stop being live across the two loops
       below. The compiler cannot sink them itself: call_stack and the register bank are separate
       allocations, but nothing in the types says so. */
    callee->registers = callee_regs;
    callee->frame_size = fsz;
    callee->frame_bounds = bounds;
    callee->return_ip = (unsigned int)(pc - c->code);
    callee->dest_reg = dest_reg;
    callee->code_offset = entry;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = registers[arg_reg_base + i];
    /* mark_vm_roots traces every slot below frame_size, so the ones this call does not fill must not
       keep a popped frame's stale references. Self-call, so the callee's block layout is the
       caller's. */
    frame_init_tags(callee_regs, (unsigned int)arg_count, bounds);
    vm->call_depth++;
    registers = callee_regs;
    pc = c->code + entry;
    DISPATCH();
}

/* OP_CALL_SELF in tail position. The frame being reused is already this variant's own, so its
   size, bounds and entry stay as they are -- OP_TAIL_CALL would install the generic function's. */
HANDLER(tail_call_self)
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    CallFrame* frame = &vm->call_stack[vm->call_depth];
    for (int i = 0; i < arg_count; i++)
        registers[i] = registers[arg_reg_base + i];
    frame_init_tags(registers, (unsigned int)arg_count, frame->frame_bounds);
    frame->tail_calls_collapsed++;
    pc = c->code + frame->code_offset;
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* On a domain error aer_math_unary_raw has already raised it and longjmped, so the store is only
   reached with a real result. */
HANDLER(raw_math_real)
    int dest = (int)UNPACK_A(op_word);
    double x = registers[UNPACK_B(op_word)].as.d;
    /* sqrt is one machine instruction and the overwhelming majority of the traffic. A real call in
       the dispatch loop costs far more than the call itself -- every hoisted pointer becomes
       call-clobbered and the register allocator pessimizes accordingly, which on nbody was worth
       10% of cycles against a 13% instruction saving. The rest are rare enough to keep the call. */
    if (UNPACK_C(op_word) == FN_MATH_SQRT && x >= 0) {
        registers[dest] = aer_real(sqrt(x));
        DISPATCH();
    }
    double out;
    SYNC_IP();
    if (aer_math_unary_raw((int)UNPACK_C(op_word), x, &out))
        registers[dest] = aer_real(out);
    DISPATCH();
}

#undef CALL_RAW

/* Bridges to the same stack-based stdlib dispatch h_call_module uses. */
HANDLER(call_module)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int module_idx = (int)READ();
    int fn_idx = (int)READ();
    uint32_t ids_word = READ();
    int module_id = (int)UNPACK_2X16_HI(ids_word);
    int fn_id = (int16_t)UNPACK_2X16_LO(ids_word); /* sign-extend -- FN_ID_UNKNOWN is -1 */
    for (int i = 0; i < arg_count; i++)
        PUSH(registers[arg_reg_base + i]);
    /* vm_call_module_dispatch (below) never actually returns with the call unresolved -- it calls
       error() itself in that case, which unwinds before coming back here -- so by the time control
       reaches this line, the module function's result is already sitting on top of the value stack. */
    vm_call_module_dispatch(vm, c, module_idx, fn_idx, module_id, fn_id, arg_count);
    registers[dest_reg] = POP();
    gc_maybe_collect(vm); /* stdlib/module functions routinely allocate (new strings/arrays/etc.) */
    DISPATCH();
}

/* vm_call_builtin takes a plain AerVal* array -- no push/pop bridge needed. */
HANDLER(call_builtin)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int name_idx = (int)READ();
    int builtin_id = (int)READ();
    /* name is only resolved on the error paths -- the happy path never needs it. */
    if (arg_count > 4) {
        error("Too many arguments to '%s'", aer_as_string(c->pool[name_idx])->data);
        registers[dest_reg] = aer_null();
        DISPATCH();
    }
    AerVal args[4];
    for (int i = 0; i < arg_count; i++)
        args[i] = registers[arg_reg_base + i];
    bool handled = vm_call_builtin(c, builtin_id, args, arg_count, &registers[dest_reg]);
    if (!handled)
        error("'%s' is not defined, or was called with the wrong number of arguments",
              aer_as_string(c->pool[name_idx])->data);
    gc_maybe_collect(vm); /* vm_call_builtin: a struct_pools tier site + aer_make_string (type()) */
    DISPATCH();
}

/* Builds an array from an in-order register range -- mark_vm_roots's registers scan keeps the
   result alive across GC. */
HANDLER(array_new)
    int dest_reg = (int)UNPACK_A(op_word);
    int item_reg_base = (int)UNPACK_B(op_word);
    int item_count = (int)UNPACK_C(op_word);
    AerArray* a = heap_new_array(&vm->heap, item_count > 0 ? (unsigned int)item_count : 4);
    a->count = (unsigned int)item_count;
    for (int i = 0; i < item_count; i++)
        a->items[i] = registers[item_reg_base + i];
    registers[dest_reg] = aer_array_val(a);
    gc_maybe_collect(vm); /* pool_alloc(&array_pool) above; result already rooted */
    DISPATCH();
}

/* Every receiver and index form the plain-array read below does not cover -- a dict, a string, a
   shaped array, a negative or out-of-range index. Separate because the call is what makes the
   compiler give this opcode a stack frame, and the plain read needs none. */
__attribute__((cold)) SEPARATE_HANDLER(index_get_compute)
    unsigned int dest_reg = UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = SCALED_REG(UNPACK_B(op_word));
    vm_index_get_compute(obj, *idx, &SCALED_REG(dest_reg));
    /* Only single-char string indexing allocates -- array/dict indexing never touches the heap, so
       skip the check for the dominant common case. */
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm);
    DISPATCH();
}

/* Already type-generic (array/dict/string) with all bounds/negative-index logic. */
HANDLER(index_get)
    unsigned int dest_reg = UNPACK_A(op_word);
    unsigned int arr_reg = UNPACK_B(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = SCALED_REG(arr_reg);
    /* A negative index wraps to a huge unsigned value and falls through to vm_array_position, which
       counts it from the end -- so one compare covers both bounds and this skips nothing a plain
       in-range read needs. */
    if (aer_type(obj) == TYPE_ARRAY && aer_type(*idx) == TYPE_INTEGER) {
        AerArray* a = aer_as_array(obj);
        uint64_t at = (uint64_t)aer_as_int(*idx);
        if (!a->shape && at < (uint64_t)a->count) {
            SCALED_REG(dest_reg) = a->items[at];
            DISPATCH();
        }
    }
    __attribute__((musttail)) return h_index_get_compute(vm, pc, registers, c);
}

/* a, b = expr -- see vm_destructure_compute. Never allocates, unlike h_index_get, so no
   gc_maybe_collect needed. */
HANDLER(destructure)
    int t0 = (int)UNPACK_A(op_word);
    int t1 = (int)UNPACK_B(op_word);
    int src_reg = (int)UNPACK_C(op_word);
    vm_destructure_compute(registers[src_reg], &registers[t0], &registers[t1]);
    DISPATCH();
}

/* Includes the internal write barrier, now exercised against a register-held reference. */
HANDLER(index_set)
    unsigned int arr_reg = UNPACK_A(op_word);
    AerVal idx = *vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    AerVal val = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    vm_index_set_compute(vm, SCALED_REG(arr_reg), idx, val);
    DISPATCH();
}

/* Loop-bound-hoisting counterparts of h_index_get/set, same word layout as OP_INDEX_GET/SET.
   This opcode carries only an index-safety proof, never a container-identity one, so the
   TYPE_TYPED_ARRAY check below is not a defensive net -- it is the only thing deciding whether the
   fast path applies. A miss must fall through to the fully generic behavior for any other
   container, not error: the parser's loop-safety proof knows nothing about container type. */
HANDLER(typed_index_get_unchecked)
    unsigned int dest_reg = UNPACK_A(op_word);
    unsigned int arr_reg = UNPACK_B(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = SCALED_REG(arr_reg);
    if (aer_type(obj) == TYPE_ARRAY) {
        /* A plain array can shrink inside the loop, so the proof covers only a typed one's index. */
        AerArray* a = aer_as_array(obj);
        if (a->shape || idx->tag != TYPE_INTEGER || (uint64_t)idx->as.i >= a->count)
            __attribute__((musttail)) return h_index_get(vm, pc, registers, c);
        SCALED_REG(dest_reg) = a->items[idx->as.i];
        DISPATCH();
    }
    if (aer_type(obj) != TYPE_TYPED_ARRAY)
        __attribute__((musttail)) return h_index_get(vm, pc, registers, c);
    AerTypedArray* ta = aer_as_typed_array(obj);
    int64_t i = aer_as_int(*idx);
    unsigned int width = vm_typed_elem_width(ta->elem_kind);
    SCALED_REG(dest_reg) = vm_typed_elem_read(ta->data + (size_t)i * width, ta->elem_kind);
    DISPATCH();
}

SEPARATE_HANDLER(typed_index_set_reject_value)
    AerTypedArray* ta = aer_as_typed_array(SCALED_REG(UNPACK_A(op_word)));
    vm_typed_array_check(c, ta->elem_kind, *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word)));
    DISPATCH();
}

HANDLER(typed_index_set_unchecked)
    unsigned int arr_reg = UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    AerVal* val = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = SCALED_REG(arr_reg);
    if (aer_type(obj) != TYPE_TYPED_ARRAY)
        __attribute__((musttail)) return h_index_set(vm, pc, registers, c);
    AerTypedArray* ta = aer_as_typed_array(obj);
    /* The loop proves the index, not the value. */
    if (!vm_typed_array_accepts(ta->elem_kind, *val))
        __attribute__((musttail)) return h_typed_index_set_reject_value(vm, pc, registers, c);
    int64_t i = aer_as_int(*idx);
    unsigned int width = vm_typed_elem_width(ta->elem_kind);
    vm_typed_elem_write(ta->data + (size_t)i * width, ta->elem_kind, *val);
    DISPATCH();
}

SEPARATE_HANDLER(index_set_any_from_int)
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    vm_index_set_compute(vm, SCALED_REG(UNPACK_A(op_word)), *idx,
                         aer_int(SCALED_REG(UNPACK_C(op_word)).as.i));
    DISPATCH();
}

SEPARATE_HANDLER(index_set_any_from_real)
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    vm_index_set_compute(vm, SCALED_REG(UNPACK_A(op_word)), *idx,
                         aer_real(SCALED_REG(UNPACK_C(op_word)).as.d));
    DISPATCH();
}

SEPARATE_HANDLER(index_get_any_as_int)
    unsigned int dest = UNPACK_A(op_word);
    AerVal obj = SCALED_REG(UNPACK_B(op_word));
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal v;
    vm_index_get_compute(obj, *idx, &v);
    if (v.tag == TYPE_INTEGER)
        SCALED_REG(dest) = aer_int(v.as.i);
    else
        error("Expected an integer from this index, got %s", vm_type_name(c, v));
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm);
    DISPATCH();
}

SEPARATE_HANDLER(index_get_any_as_real)
    unsigned int dest = UNPACK_A(op_word);
    AerVal obj = SCALED_REG(UNPACK_B(op_word));
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal v;
    vm_index_get_compute(obj, *idx, &v);
    if (v.tag == TYPE_REAL)
        SCALED_REG(dest) = aer_real(v.as.d);
    else if (v.tag == TYPE_INTEGER)
        SCALED_REG(dest) = aer_real((double)v.as.i);
    else
        error("Expected a number from this index, got %s", vm_type_name(c, v));
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm);
    DISPATCH();
}

/* The non-typed-array receiver goes through the ordinary boxed compute and then unboxes, so these
   stay correct on any value -- a plain array of reals, a string index, a dict -- and only the
   typed-array case skips the AerVal entirely. A non-numeric result is the same error the raw
   arithmetic that consumes this slot would have raised one opcode later. */
HANDLER(index_set_raw_int)
    AerVal obj = SCALED_REG(UNPACK_A(op_word));
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    int64_t v = SCALED_REG(UNPACK_C(op_word)).as.i;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            if (ta->elem_kind == TYPED_ELEM_INT64) {
                memcpy(ta->data + (size_t)idx->as.i * 8, &v, 8);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_INT32 && v >= INT32_MIN && v <= INT32_MAX) {
                int32_t narrow = (int32_t)v;
                memcpy(ta->data + (size_t)idx->as.i * 4, &narrow, 4);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_set_any_from_int(vm, pc, registers, c);
}

HANDLER(index_set_raw_real)
    AerVal obj = SCALED_REG(UNPACK_A(op_word));
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    double v = SCALED_REG(UNPACK_C(op_word)).as.d;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            if (ta->elem_kind == TYPED_ELEM_FLOAT64) {
                memcpy(ta->data + (size_t)idx->as.i * 8, &v, 8);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_FLOAT32) {
                float narrow = (float)v;
                memcpy(ta->data + (size_t)idx->as.i * 4, &narrow, 4);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_set_any_from_real(vm, pc, registers, c);
}

HANDLER(index_get_raw_int)
    unsigned int dest = UNPACK_A(op_word);
    AerVal obj = SCALED_REG(UNPACK_B(op_word));
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            /* Only two kinds can reach the read, so decode it here rather than through the shared
               helpers, which each re-decide among five on an element read. */
            if (ta->elem_kind == TYPED_ELEM_INT64) {
                int64_t v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 8, 8);
                SCALED_REG(dest) = aer_int(v);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_INT32) {
                int32_t v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 4, 4);
                SCALED_REG(dest) = aer_int(v);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_get_any_as_int(vm, pc, registers, c);
}

HANDLER(index_get_raw_real)
    unsigned int dest = UNPACK_A(op_word);
    AerVal obj = SCALED_REG(UNPACK_B(op_word));
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            /* Same two-kind decode as index_get_raw_int above. */
            if (ta->elem_kind == TYPED_ELEM_FLOAT64) {
                double v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 8, 8);
                SCALED_REG(dest) = aer_real(v);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_FLOAT32) {
                float v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 4, 4);
                SCALED_REG(dest) = aer_real((double)v);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_get_any_as_real(vm, pc, registers, c);
}

/* Any receiver the fused compound below does not cover -- a dict, a plain array, a narrow element --
   as the ordinary boxed read-modify-write its three-opcode expansion would have done. */
static void vm_index_compound_slow(VM* vm, const uint32_t* pc, Chunk* c, AerVal obj, AerVal idx,
                                   AerVal rhs, Opcode bin) {
    /* An existing dict entry is read and written through ONE resolution -- the whole reason the read
       and the write are in the same handler. A missing key falls through so its error is the one the
       separate read and write would have raised. */
    if (aer_type(obj) == TYPE_DICT && aer_type(idx) == TYPE_STRING) {
        AerDict* d = aer_as_dict(obj);
        AerString* is = aer_as_string(idx);
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        HashValue khash = hashtable_string_hash(is, klen);
        int at = hashtable_get_index_hashed(&d->map, is->data, klen, khash);
        if (at >= 0) {
            AerVal held = d->map.dense[at].payload;
            bool done;
            AerVal updated = vm_binary_fast(held, rhs, bin, aer_type(held), aer_type(rhs), &done);
            if (!done)
                updated = vm_binary_cold(c, held, rhs, bin, aer_type(held), aer_type(rhs));
            gc_barrier_dict(vm, d, (unsigned int)at, updated);
            d->map.dense[at].payload = updated;
            return;
        }
    }
    AerVal cur;
    vm_index_get_compute(obj, idx, &cur);
    bool handled;
    AerVal res = vm_binary_fast(cur, rhs, bin, aer_type(cur), aer_type(rhs), &handled);
    if (!handled)
        res = vm_binary_cold(c, cur, rhs, bin, aer_type(cur), aer_type(rhs));
    vm_index_set_compute(vm, obj, idx, res);
}

/* `arr[rk] OP= rhs` on any receiver: the same read-modify-write the three-opcode expansion did, in
   one dispatch, and without boxing the element into a temp register in between. The read and the
   write still resolve separately, so a dict still hashes its key twice. */
HANDLER(index_compound)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    Opcode bin = (Opcode)UNPACK_B(op_word);
    uint32_t idx_rhs_word = READ();
    AerVal idx = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_HI(idx_rhs_word));
    AerVal rhs = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(idx_rhs_word));
    vm_index_compound_slow(vm, pc, c, obj, idx, rhs, bin);
    gc_maybe_collect(vm);
    DISPATCH();
}

/* Only ADD/SUB/MUL reach here (parser.c), so the three-way choice is exhaustive. */
HANDLER(index_compound_raw_int)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    Opcode bin = (Opcode)UNPACK_B(op_word);
    int64_t rhs = registers[READ()].as.i;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count && ta->elem_kind == TYPED_ELEM_INT64) {
            unsigned char* at = ta->data + (size_t)idx->as.i * 8;
            int64_t cur;
            memcpy(&cur, at, 8);
            cur = bin == OP_ADD ? cur + rhs : bin == OP_SUB ? cur - rhs : cur * rhs;
            memcpy(at, &cur, 8);
            DISPATCH();
        }
    }
    vm_index_compound_slow(vm, pc, c, obj, *idx, aer_int(rhs), bin);
    DISPATCH();
}

HANDLER(index_compound_raw_real)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    Opcode bin = (Opcode)UNPACK_B(op_word);
    double rhs = registers[READ()].as.d;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count && ta->elem_kind == TYPED_ELEM_FLOAT64) {
            unsigned char* at = ta->data + (size_t)idx->as.i * 8;
            double cur;
            memcpy(&cur, at, 8);
            cur = bin == OP_ADD ? cur + rhs : bin == OP_SUB ? cur - rhs : cur * rhs;
            memcpy(at, &cur, 8);
            DISPATCH();
        }
    }
    vm_index_compound_slow(vm, pc, c, obj, *idx, aer_real(rhs), bin);
    DISPATCH();
}

/* `arr[a:b]` -- vm_slice_bounds() resolves/clamps the bounds; a slice is always a fresh copy. */
HANDLER(slice_get)
    int dest_reg = (int)UNPACK_A(op_word);
    int arr_reg = (int)UNPACK_B(op_word);
    uint32_t bounds_word = READ();
    AerVal start_v = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_HI(bounds_word));
    AerVal end_v = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(bounds_word));
    AerVal obj = registers[arr_reg];
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) {
            error("Structs cannot be sliced");
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)a->count, &start, &end)) {
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        unsigned int n = (unsigned int)(end - start);
        /* a slice is always a plain array, even of a struct */
        AerArray* r = heap_new_array(&vm->heap, n > 0 ? n : 4);
        r->count = n;
        for (unsigned int i = 0; i < n; i++)
            r->items[i] = a->items[start + i];
        registers[dest_reg] = aer_array_val(r);
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)os->length, &start, &end)) {
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        unsigned int sub_len = (unsigned int)(end - start);
        registers[dest_reg] = aer_make_string_copy(
            os->data + start, sub_len); /* no chunk_add_pool interning -- see vm_to_str's comment */
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        /* A copy, like the array and string cases -- not a view. A view would have to keep its
           parent alive and know not to free a buffer it borrowed, and slicing is the same spelling
           on all three types, so it should not mean something structurally different on one. */
        AerTypedArray* ta = aer_as_typed_array(obj);
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)ta->count, &start, &end)) {
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        unsigned int n = (unsigned int)(end - start);
        AerTypedArray* r = vm_new_typed_array(ta->elem_kind, n);
        unsigned int w = vm_typed_elem_width(ta->elem_kind);
        if (n > 0)
            memcpy(r->data, ta->data + (size_t)start * w, (size_t)n * w);
        registers[dest_reg] = aer_typed_array_val(r);
    } else {
        error("Cannot slice this type");
        registers[dest_reg] = aer_null();
    }
    gc_maybe_collect(
        vm); /* array branch: pool_alloc(&array_pool); string branch: aer_make_string; the null-result branches above are harmless no-ops here too */
    DISPATCH();
}

/* Dict literal -- each key stored as an owned copy, never an alias into the source string. */
HANDLER(dict_new)
    int dest_reg = (int)UNPACK_A(op_word);
    int pair_reg_base = (int)UNPACK_B(op_word);
    int pair_count = (int)UNPACK_C(op_word);
    AerDict* d = heap_new_dict(&vm->heap);
    if (pair_count > 0)
        hashtable_reserve(&d->map, (unsigned int)pair_count);
    for (int i = 0; i < pair_count; i++) {
        AerVal key = registers[pair_reg_base + 2 * i];
        AerVal val = registers[pair_reg_base + 2 * i + 1];
        if (aer_type(key) != TYPE_STRING) {
            error("Hashtable keys must be strings");
            continue;
        }
        AerString* ks = aer_as_string(key);
        unsigned int klen = hashtable_key_true_len(ks->data, ks->length);
        HashValue khash = hashtable_string_hash(ks, klen);
        char* k = hashtable_key_dup_known(d->map.pools, ks->data, klen);
        hashtable_put_hashed(&d->map, k, klen, khash, val);
    }
    registers[dest_reg] = aer_dict_val(d);
    gc_maybe_collect(vm); /* pool_alloc(&dict_pool) above; result already rooted */
    DISPATCH();
}

/* Arrays yield items, dicts yield keys, strings yield 1-char strings. */
HANDLER(iter_next_array)
    int col_reg = (int)UNPACK_A(op_word);
    int idx_reg = (int)UNPACK_B(op_word);
    int item_dest_reg = (int)UNPACK_C(op_word);
    int end_target = READ();
    AerVal col = registers[col_reg];
    int64_t idx = aer_as_int(registers[idx_reg]);
    if (aer_type(col) == TYPE_DICT) {
        /* Single-variable `for k in dict:` yields keys. */
        AerVal key;
        if (!vm_dict_next_key(aer_as_dict(col), &idx, &key)) {
            pc += (int32_t)end_target;
            DISPATCH();
        }
        registers[item_dest_reg] = key;
        registers[idx_reg] = aer_int(idx + 1);
        gc_maybe_collect(vm); /* vm_dict_next_key's owned-copy key string allocates */
        DISPATCH();
    }
    if (aer_type(col) == TYPE_STRING) {
        /* Yields one-character strings, same shape as dict-key iteration. */
        AerString* cs = aer_as_string(col);
        if ((uint64_t)idx >= cs->length) {
            pc += (int32_t)end_target;
            DISPATCH();
        }
        registers[item_dest_reg] = aer_make_string_copy(
            cs->data + idx, 1); /* no chunk_add_pool interning -- see vm_to_str's comment */
        registers[idx_reg] = aer_int(idx + 1);
        gc_maybe_collect(vm);
        DISPATCH();
    }
    if (aer_type(col) == TYPE_TYPED_ARRAY) {
        /* Unlike a packed array's struct elements (no standalone `arr[i]` reference, so no
           iteration either -- see README), a typed array's elements ARE plain scalar values, so
           iteration works exactly like an ordinary array's. */
        AerTypedArray* ta = aer_as_typed_array(col);
        if ((uint64_t)idx >= ta->count) {
            pc += (int32_t)end_target;
            DISPATCH();
        }
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        registers[item_dest_reg] = vm_typed_elem_read(ta->data + (size_t)idx * width, ta->elem_kind);
        registers[idx_reg] = aer_int(idx + 1);
        DISPATCH();
    }
    if (aer_type(col) != TYPE_ARRAY) {
        error("'for x in ...' only supports arrays, dicts, strings, and typed arrays");
        DISPATCH();
    }
    AerArray* a = aer_as_array(col);
    if ((uint64_t)idx >= a->count) {
        pc += (int32_t)end_target;
        DISPATCH();
    }
    registers[item_dest_reg] = a->items[idx];
    registers[idx_reg] = aer_int(idx + 1);
    DISPATCH();
}

/* `for k, v in dict:` -- see vm_dict_next_key (above) for the shared bucket-scan/copy-key logic. */
HANDLER(iter_next_pair)
    int col_reg = (int)UNPACK_A(op_word);
    int idx_reg = (int)UNPACK_B(op_word);
    int key_dest_reg = (int)UNPACK_C(op_word);
    int val_dest_reg = (int)READ();
    int end_target = READ();
    AerVal col = registers[col_reg];
    if (aer_type(col) != TYPE_DICT) {
        error("for k, v requires a hashtable");
        pc += (int32_t)end_target;
        DISPATCH();
    }
    AerDict* d = aer_as_dict(col);
    int64_t idx = aer_as_int(registers[idx_reg]);
    AerVal key;
    if (!vm_dict_next_key(d, &idx, &key)) {
        pc += (int32_t)end_target;
        DISPATCH();
    }
    registers[key_dest_reg] = key;
    registers[val_dest_reg] = d->map.dense[idx].payload;
    registers[idx_reg] = aer_int(idx + 1);
    gc_maybe_collect(vm); /* vm_dict_next_key's owned-copy key string allocates */
    DISPATCH();
}

/* Runs once before the loop -- same checks as OP_ITER_RANGE_LOOP below, but never advances
   cur_reg (nothing to prepare yet). */
HANDLER(iter_range_prep)
    int cur_reg = (int)UNPACK_A(op_word);
    int end_reg = (int)UNPACK_B(op_word);
    int step_reg = (int)UNPACK_C(op_word);
    uint32_t item_word = READ();
    int item_dest_reg = RANGE_PREP_ITEM_REG(item_word);
    int empty_target = READ();
    AerVal cur_v = registers[cur_reg];
    AerVal end_v = registers[end_reg];
    AerVal step_v = registers[step_reg];
    if (aer_type(cur_v) != TYPE_INTEGER || aer_type(end_v) != TYPE_INTEGER ||
        aer_type(step_v) != TYPE_INTEGER) {
        error("Range bounds and step must be integers");
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    int64_t cur = aer_as_int(cur_v), rng_end = aer_as_int(end_v), step = aer_as_int(step_v);
    if (step <= 0) {
        error("Range step must be a positive integer (direction is inferred from the bounds, not the step's "
              "sign)");
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    /* The body indexes unchecked on a proof that every value lands in [0, length), which holds only
       while the loop ascends from a non-negative start -- so verify that precondition once here
       rather than per iteration. Neither half is hypothetical: direction is inferred from the
       bounds, so `for i in 200..length(a)` on a short array descends off the end (this segfaulted
       before the check existed), and overflow in a computed start can wrap it negative. */
    /* One unsigned compare covers both halves: rng_end is a length so it is never negative, and a
       negative cur reinterprets as a huge unsigned, failing the same test. Splitting the cases
       costs nothing because it only happens on the way to raising. */
    if ((item_word & RANGE_PREP_GUARD_NONNEG) && (uint64_t)cur > (uint64_t)rng_end) {
        if (cur < 0)
            error("Range start is negative (%lld) -- an index computation overflowed", (long long)cur);
        else
            error("Range start %lld is past its end %lld, so this loop would count downwards out of "
                  "the collection it indexes",
                  (long long)cur, (long long)rng_end);
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    /* Precomputes the iteration count once (ceiling division, matching Lua's FORLOOP) instead of
       re-deriving it every dispatch. count==0 means empty range. */
    bool ascending = cur < rng_end;
    int64_t diff = ascending ? (rng_end - cur) : (cur - rng_end);
    /* step==1 (the common case) skips the division -- this target has no fast hardware divide,
       and short re-entered loops pay that cost often enough for it to be a real win. */
    int64_t count = (step == 1) ? diff : (diff + step - 1) / step;
    if (count == 0) {
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    /* end_reg/step_reg repurposed for this loop's life -- arg_materialize's snapshot guarantees
       they're fresh, loop-owned registers nothing else reads. */
    registers[end_reg] = aer_int(count - 1); /* iterations remaining AFTER this one */
    registers[step_reg] =
        aer_int(ascending ? step : -step); /* direction baked in once, not re-inferred every iteration */
    registers[item_dest_reg] = cur_v;
    DISPATCH();
}

/* Runs at the bottom of the loop body. end_reg/step_reg hold PREP's repurposed countdown/signed
   step, not the original bound, so no comparison against the original limit is ever needed (Lua's
   FORLOOP shape); on exhaustion it falls through leaving cur_reg/item_dest_reg at their last value.
   cur/remaining/step are written value-only -- PREP validated all three as integers and they are
   loop-owned snapshots, so nothing can have retagged them. body_target is always a resolved
   address, never a patch placeholder. No gc_maybe_collect -- nothing here allocates. */
HANDLER(iter_range_loop)
    unsigned int cur_reg = UNPACK_A(op_word);
    unsigned int remaining_reg = UNPACK_B(op_word);
    unsigned int signed_step_reg = UNPACK_C(op_word);
    int item_dest_reg = (int)READ();
    int body_target = READ();
    int64_t remaining = SCALED_REG(remaining_reg).as.i;
    if (remaining == 0) {
        DISPATCH(); /* exhausted -- fall through to the exit code, cur_reg/item_dest_reg untouched */
    }
    int64_t new_cur = SCALED_REG(cur_reg).as.i + SCALED_REG(signed_step_reg).as.i;
    SCALED_REG(cur_reg).as.i = new_cur;
    SCALED_REG(remaining_reg).as.i = remaining - 1;
    /* Equal when the parser proved this loop's body never writes the loop variable, so cur_reg IS
       item_dest_reg and the store above already published this iteration's value. */
    if (cur_reg != 2u * (unsigned int)item_dest_reg)
        registers[item_dest_reg] = aer_int(new_cur);
    pc += (int32_t)body_target;
    /* range-for's own dedicated back-edge -- h_jump's check doesn't cover this loop shape since
       it never goes through a plain OP_JUMP. */
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* chunk_find_shape() by name, arity check, one struct_pools[] allocation (sized to the tier this
   shape's instance_bytes fits), trailing fields default-filled. */
HANDLER(struct_new)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int type_name_pool_idx = (int)READ();
    const char* name = aer_as_string(c->pool[type_name_pool_idx])->data;
    Shape* shape = chunk_shape_for_pool_idx(c, (unsigned int)type_name_pool_idx);
    if (!shape) {
        error("'%s' is not defined", name);
        DISPATCH();
    }
    if ((unsigned int)arg_count > shape->field_count) {
        error("'%s' takes at most %u argument%s, got %d", name, shape->field_count,
              shape->field_count == 1 ? "" : "s", arg_count);
        DISPATCH();
    }
    /* Positional args must match a typed field too -- the fused field-arithmetic fast path trusts
       every field unconditionally, not just ones set via `.field =`. Checked before allocating. */
    for (int i = 0; i < arg_count; i++) {
        ValueType declared = shape->field_types[i];
        if (declared != TYPE_ANY && registers[arg_reg_base + i].tag != declared) {
            error(
                "'%s' field '%s' is declared as a fixed type and cannot be constructed with a different type",
                name, aer_as_string(c->pool[shape->field_names[i]])->data);
            DISPATCH();
        }
        if (!vm_check_narrow_field_write(declared, shape->field_narrow[i], registers[arg_reg_base + i]))
            DISPATCH();
    }
    AerStruct* s = heap_new_struct(&vm->heap, shape);
    /* The _at form, with the shape already in hand: vm_struct_field_write re-reads s->shape twice
       per field through a non-inlinable external call, and this loop runs for every field of every
       construction -- 8.19% of bench/binary_trees.aer sat in it. */
    for (unsigned int i = 0; i < (unsigned int)arg_count; i++)
        vm_struct_field_write_at(s, shape->field_offsets[i], shape->field_types[i], shape->field_narrow[i],
                                 registers[arg_reg_base + i]);
    for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
        vm_struct_field_write_at(s, shape->field_offsets[i], shape->field_types[i], shape->field_narrow[i],
                                 vm_default_value(vm, shape->field_defaults[i]));
    registers[dest_reg] = aer_struct_val(s);
    gc_maybe_collect(
        vm); /* pool_alloc(&struct_pools[tier]) above, plus any vm_default_value array/dict defaults -- all rooted now that the struct itself is stored */
    DISPATCH();
}

SEPARATE_HANDLER(field_get_by_name)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int dest_reg = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    int field_idx = (int)READ();
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    registers[dest_reg] = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    DISPATCH();
}

HANDLER(field_get)
    AerVal* obj = &registers[(int)UNPACK_B(op_word)];
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (obj->tag != TYPE_STRUCT || entry->shape != ((AerStruct*)obj->as.ptr)->shape)
        __attribute__((musttail)) return h_field_get_by_name(vm, pc, registers, c);
    (void)READ();
    registers[(int)UNPACK_A(op_word)] =
        vm_struct_field_read_at((AerStruct*)obj->as.ptr, entry->offset, entry->ftype, entry->narrow);
    DISPATCH();
}

/* `x OP y.field` reaches here too: parse_binary_ops canonicalizes it into this opcode's `field OP' x`
   form at compile time (see that function's own comment, and OP_FIELD_BINARY's, vm.h, for why that
   is exact, not an approximation, for every operator that reaches here). */
HANDLER(field_binary)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int dest_reg = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    Opcode bin_op = (Opcode)UNPACK_C(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* rhs = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    registers[dest_reg] = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        registers[dest_reg] = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    DISPATCH();
}

/* `struct.field OP= rhs` -- one vm_resolve_field/cache lookup covers the read, the compute, the
   type check and the write back, where a separate read and write would each pay their own. */
HANDLER(field_compound)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int struct_reg = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* rhs = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        result = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    /* Same enforcement as OP_FIELD_SET's own -- the only other place a field's value changes. */
    if (ftype != TYPE_ANY && result.tag != ftype) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(ftype, narrow, result))
        DISPATCH();
    gc_barrier_struct(vm, oa, result);
    vm_struct_field_write_at(oa, foffset, ftype, narrow, result);
    DISPATCH();
}

/* Fuses `(A op1 B) op2 C` into one pass when A/B/C are all matching-shape typed arrays --
   vm_typed_array_chain2's own comment has the full "why" and the measured win. Any other operand
   shape (a plain int/real, a mismatched typed array, a different-length one) falls back to
   computing the EXACT unfused result: op1 first, then op2 on that intermediate -- the same value
   this would have produced as two separate statements, just without the fusion win for this one
   call. Correctness never depends on which path runs. */
HANDLER(typed_array_chain2)
    int dest_reg = (int)UNPACK_A(op_word);
    int a_reg = (int)UNPACK_B(op_word);
    int b_reg = (int)UNPACK_C(op_word);
    uint32_t word1 = READ();
    Opcode op1 = (Opcode)UNPACK_2X16_HI(word1);
    int c_reg = (int)UNPACK_2X16_LO(word1);
    Opcode op2 = (Opcode)READ();

    AerVal av = registers[a_reg], bv = registers[b_reg], cv = registers[c_reg];
    if (aer_type(av) == TYPE_TYPED_ARRAY && aer_type(bv) == TYPE_TYPED_ARRAY &&
        aer_type(cv) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(av);
        AerTypedArray* tb = aer_as_typed_array(bv);
        AerTypedArray* tcc = aer_as_typed_array(cv);
        int i1 = op_chain2_index(op1), i2 = op_chain2_index(op2);
        if (i1 >= 0 && i2 >= 0 &&
            (ta->elem_kind == TYPED_ELEM_FLOAT32 || ta->elem_kind == TYPED_ELEM_FLOAT64) &&
            ta->elem_kind == tb->elem_kind && ta->elem_kind == tcc->elem_kind && ta->count == tb->count &&
            ta->count == tcc->count) {
            registers[dest_reg] = vm_typed_array_chain2(ta, tb, tcc, op1, op2);
            gc_maybe_collect(vm); /* vm_new_typed_array (inside) allocates */
            DISPATCH();
        }
    }
    /* Fallback: exact unfused computation, still fully correct. */
    bool handled;
    ValueType ta_ = aer_type(av), tb_ = aer_type(bv);
    AerVal tmp = vm_binary_fast(av, bv, op1, ta_, tb_, &handled);
    if (!handled) {
        tmp = vm_binary_cold(c, av, bv, op1, ta_, tb_);
        gc_maybe_collect(vm);
    }
    ValueType ttmp = aer_type(tmp), tc_ = aer_type(cv);
    AerVal result = vm_binary_fast(tmp, cv, op2, ttmp, tc_, &handled);
    if (!handled) {
        result = vm_binary_cold(c, tmp, cv, op2, ttmp, tc_);
        gc_maybe_collect(vm);
    }
    registers[dest_reg] = result;
    DISPATCH();
}

/* Shell mode auto-print: a bare statement's result is printed unless null. */
HANDLER(print_repl)
    int src_reg = (int)UNPACK_A(op_word);
    AerVal v = registers[src_reg];
    if (aer_type(v) != TYPE_NULL) {
        vm_print_value(c, v, false);
        printf("\n");
    }
    DISPATCH();
}

/* gc_barrier_struct is the write barrier every mutating struct field-set needs. */
SEPARATE_HANDLER(field_set_by_name)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int struct_reg = (int)UNPACK_A(op_word);
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    int field_idx = (int)READ();
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType declared;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &declared,
                          &narrow))
        DISPATCH();
    /* Enforced once here (the only place a field's value changes), trusted everywhere else
       including the fused fast path. TYPE_ANY means untyped. */
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(declared, narrow, *val))
        DISPATCH();
    gc_barrier_struct(vm, oa, *val);
    vm_struct_field_write_at(oa, foffset, declared, narrow, *val);
    DISPATCH();
}

HANDLER(field_set)
    AerVal* obj = &registers[(int)UNPACK_A(op_word)];
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (obj->tag != TYPE_STRUCT || entry->shape != ((AerStruct*)obj->as.ptr)->shape ||
        (entry->ftype != TYPE_ANY && val->tag != entry->ftype) ||
        !vm_fits_narrow_field(entry->ftype, entry->narrow, *val))
        __attribute__((musttail)) return h_field_set_by_name(vm, pc, registers, c);
    (void)READ();
    AerStruct* s = (AerStruct*)obj->as.ptr;
    gc_barrier_struct(vm, s, *val);
    vm_struct_field_write_at(s, entry->offset, entry->ftype, entry->narrow, *val);
    DISPATCH();
}

/* Shape-specialized raw field access -- only in a specialized body, where the parser resolved the
   field offset against a proven Shape, so there is no runtime lookup and neither side is boxed.
   The aer_type() check is a safety net: the reg_known_shape[] audit is not proven exhaustive, and a
   gap should be loud rather than silent corruption. Storage kind stays in the opcode: moving it into
   an operand measured 1.5-2.2% slower. */
/* Which storage a specialized field uses. Every handler below passes a literal, so each switch here
   folds to the single load or store that field needs -- the kind never reaches a runtime branch. */
typedef enum { RAWW_INT, RAWW_REAL, RAWW_INT32, RAWW_FLOAT32 } RawWidth;

static inline __attribute__((always_inline)) void raw_field_read(AerVal* dst, unsigned char* src, RawWidth w) {
    switch (w) {
        case RAWW_INT:
            dst->tag = TYPE_INTEGER;
            memcpy(&dst->as.i, src, 8);
            break;
        case RAWW_REAL:
            dst->tag = TYPE_REAL;
            memcpy(&dst->as.d, src, 8);
            break;
        case RAWW_INT32: *dst = aer_int(vm_raw_read_int32(src)); break;
        case RAWW_FLOAT32: *dst = aer_real(vm_raw_read_float32(src)); break;
    }
}

static inline __attribute__((always_inline)) void raw_field_write(VM* vm, const uint32_t* pc,
                                                                  unsigned char* dst, AerVal src,
                                                                  RawWidth w) {
    switch (w) {
        case RAWW_INT: memcpy(dst, &src.as.i, 8); break;
        case RAWW_REAL: memcpy(dst, &src.as.d, 8); break;
        case RAWW_INT32:
            if (vm_fits_narrow_field(TYPE_INTEGER, true, src))
                vm_raw_write_int32(dst, src.as.i);
            else
                vm_check_narrow_field_write(TYPE_INTEGER, true, src); /* reports */
            break;
        case RAWW_FLOAT32: vm_raw_write_float32(dst, src.as.d); break;
    }
}

/* The struct a specialized access names, or NULL once it has raised -- callers dispatch either way.
   The check is a safety net rather than a real possibility: a gap in the reg_known_shape[] audit
   should be loud instead of silently reinterpreting some other value's bits as a field. */
static inline __attribute__((always_inline)) AerStruct* raw_field_struct(VM* vm, const uint32_t* pc, AerVal obj) {
    if (aer_type(obj) == TYPE_STRUCT)
        return aer_as_struct(obj);
    error("internal error: specialized struct field access on a non-struct value");
    return NULL;
}

/* The raw field bytes a specialized packed-array access names, or NULL once it has raised. Below the
   `error` macro, so a raised error records this instruction rather than the caller's. */
static inline __attribute__((always_inline)) unsigned char* vm_packed_raw_elem(VM* vm, const uint32_t* pc,
                                                                               AerVal obj, AerVal* idx,
                                                                               unsigned int foffset) {
    if (aer_type(obj) != TYPE_PACKED_ARRAY) {
        error("internal error: specialized packed-array field access on a non-packed-array value");
        return NULL;
    }
    AerPackedArray* pa = aer_as_packed_array(obj);
    uint64_t i;
    if (!vm_array_position(*idx, pa->count, &i)) {
        vm_array_index_error(*idx, pa->count);
        return NULL;
    }
    return pa->data + (size_t)i * pa->shape->instance_bytes + foffset;
}

/* vm_packed_raw_elem for an index a loop proved in range (parser.c's index_safe_unchecked): only the array's
   own type check remains, as a defensive net. */
static inline __attribute__((always_inline)) unsigned char*
vm_packed_raw_elem_unchecked(VM* vm, const uint32_t* pc, AerVal obj, AerVal* idx, unsigned int foffset) {
    if (aer_type(obj) != TYPE_PACKED_ARRAY) {
        error("internal error: specialized packed-array field access on a non-packed-array value");
        return NULL;
    }
    AerPackedArray* pa = aer_as_packed_array(obj);
    int64_t i = aer_as_int(*idx);
    return pa->data + (size_t)i * pa->shape->instance_bytes + foffset;
}

static inline __attribute__((always_inline)) unsigned char* raw_elem(VM* vm, const uint32_t* pc, AerVal arr,
                                                                     AerVal* idx, unsigned int foffset,
                                                                     bool checked) {
    return checked ? vm_packed_raw_elem(vm, pc, arr, idx, foffset)
                   : vm_packed_raw_elem_unchecked(vm, pc, arr, idx, foffset);
}

/* `+=`, `-=` and `*=` are the whole specialized compound set; anything else raises and leaves the
   field alone. Int and real never share a field, so the width picks the arithmetic too. */
static inline __attribute__((always_inline)) bool raw_field_compound(VM* vm, const uint32_t* pc, unsigned char* elem,
                                                 AerVal rhs, Opcode bin_op, RawWidth w) {
    if (w == RAWW_INT || w == RAWW_INT32) {
        int64_t lhs, result;
        if (w == RAWW_INT)
            memcpy(&lhs, elem, 8);
        else
            lhs = vm_raw_read_int32(elem);
        switch (bin_op) {
            case OP_ADD: result = lhs + rhs.as.i; break;
            case OP_SUB: result = lhs - rhs.as.i; break;
            case OP_MUL: result = lhs * rhs.as.i; break;
            default: error("internal error: unsupported raw compound-assign op"); return false;
        }
        if (w == RAWW_INT) {
            memcpy(elem, &result, 8);
        } else if (vm_fits_narrow_field(TYPE_INTEGER, true, aer_int(result))) {
            vm_raw_write_int32(elem, result);
        } else {
            vm_check_narrow_field_write(TYPE_INTEGER, true, aer_int(result));
            return false;
        }
    } else {
        double lhs, result;
        if (w == RAWW_REAL)
            memcpy(&lhs, elem, 8);
        else
            lhs = vm_raw_read_float32(elem);
        switch (bin_op) {
            case OP_ADD: result = lhs + rhs.as.d; break;
            case OP_SUB: result = lhs - rhs.as.d; break;
            case OP_MUL: result = lhs * rhs.as.d; break;
            default: error("internal error: unsupported raw compound-assign op"); return false;
        }
        if (w == RAWW_REAL)
            memcpy(elem, &result, 8);
        else
            vm_raw_write_float32(elem, result);
    }
    return true;
}

static inline __attribute__((always_inline)) void field_get_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                            uint32_t op_word, unsigned int foffset, RawWidth w) {
    AerStruct* s = raw_field_struct(vm, pc, registers[UNPACK_B(op_word)]);
    if (s)
        raw_field_read(&registers[UNPACK_A(op_word)], s->fields + foffset, w);
}

static inline __attribute__((always_inline)) void field_set_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                            uint32_t op_word, unsigned int foffset, int src_slot,
                                            RawWidth w) {
    AerStruct* s = raw_field_struct(vm, pc, registers[UNPACK_A(op_word)]);
    if (s)
        raw_field_write(vm, pc, s->fields + foffset, registers[src_slot], w);
}

static inline __attribute__((always_inline)) void field_compound_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                                 uint32_t op_word, unsigned int foffset,
                                                 int rhs_slot, RawWidth w) {
    AerStruct* s = raw_field_struct(vm, pc, registers[UNPACK_A(op_word)]);
    if (s)
        raw_field_compound(vm, pc, s->fields + foffset, registers[rhs_slot],
                           (Opcode)UNPACK_B(op_word), w);
}

static inline __attribute__((always_inline)) void index_field_get_raw(VM* vm, const uint32_t* pc,
                                                                      AerVal* registers, Chunk* c,
                                                                      uint32_t op_word,
                                                                      uint32_t field_rk_word, RawWidth w,
                                                                      bool checked) {
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_B(op_word)], idx, foffset, checked);
    if (elem)
        raw_field_read(&registers[UNPACK_A(op_word)], elem, w);
}

static inline __attribute__((always_inline)) void index_field_set_raw(VM* vm, const uint32_t* pc,
                                                                      AerVal* registers, Chunk* c,
                                                                      uint32_t op_word,
                                                                      uint32_t off_slot_word, RawWidth w,
                                                                      bool checked) {
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    unsigned int foffset = UNPACK_2X16_HI(off_slot_word);
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_A(op_word)], idx, foffset, checked);
    if (elem)
        raw_field_write(vm, pc, elem, registers[UNPACK_2X16_LO(off_slot_word)], w);
}

static inline __attribute__((always_inline)) void index_field_compound_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                                       Chunk* c, uint32_t op_word,
                                                       uint32_t field_rk_word, int rhs_slot,
                                                       RawWidth w, bool checked) {
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_A(op_word)], idx, foffset, checked);
    if (elem)
        raw_field_compound(vm, pc, elem, registers[rhs_slot], (Opcode)UNPACK_B(op_word), w);
}

/* `+=` with bin_op already known at compile time, so it skips the switch the general form runs. */
static inline __attribute__((always_inline)) void index_field_add_raw(VM* vm, const uint32_t* pc,
                                                                      AerVal* registers, Chunk* c,
                                                                      uint32_t op_word,
                                                                      uint32_t field_rk_word, int rhs_slot,
                                                                      RawWidth w, bool checked) {
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_A(op_word)], idx, foffset, checked);
    if (!elem)
        return;
    if (w == RAWW_REAL) {
        double lhs;
        memcpy(&lhs, elem, 8);
        lhs += registers[rhs_slot].as.d;
        memcpy(elem, &lhs, 8);
    } else {
        vm_raw_write_float32(elem, vm_raw_read_float32(elem) + registers[rhs_slot].as.d);
    }
}


HANDLER(index_field_get_raw_int)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_real)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_REAL, true);
    DISPATCH();
}

HANDLER(field_get_raw_int)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_INT);
    DISPATCH();
}

HANDLER(field_get_raw_real)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_REAL);
    DISPATCH();
}

HANDLER(index_field_get_raw_int32)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT32, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_float32)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_FLOAT32, true);
    DISPATCH();
}

HANDLER(field_get_raw_int32)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_INT32);
    DISPATCH();
}

HANDLER(field_get_raw_float32)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_FLOAT32);
    DISPATCH();
}

HANDLER(index_field_set_raw_int)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT, true);
    DISPATCH();
}

HANDLER(index_field_set_raw_real)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_REAL, true);
    DISPATCH();
}

HANDLER(field_set_raw_int)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_INT);
    DISPATCH();
}

HANDLER(field_set_raw_real)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_REAL);
    DISPATCH();
}

HANDLER(index_field_set_raw_int32)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT32, true);
    DISPATCH();
}

HANDLER(index_field_set_raw_float32)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_FLOAT32, true);
    DISPATCH();
}

HANDLER(field_set_raw_int32)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_INT32);
    DISPATCH();
}

HANDLER(field_set_raw_float32)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_FLOAT32);
    DISPATCH();
}

HANDLER(field_compound_raw_int)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_INT);
    DISPATCH();
}

HANDLER(field_compound_raw_real)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_REAL);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT, true);
    DISPATCH();
}

HANDLER(index_field_compound_raw_real)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_REAL, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_int_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT, false);
    DISPATCH();
}

HANDLER(index_field_get_raw_real_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_REAL, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_int_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_real_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_REAL, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_real_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_REAL, false);
    DISPATCH();
}

HANDLER(field_compound_raw_int32)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_INT32);
    DISPATCH();
}

HANDLER(field_compound_raw_float32)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_FLOAT32);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int32)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT32, true);
    DISPATCH();
}

HANDLER(index_field_compound_raw_float32)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_FLOAT32, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_int32_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT32, false);
    DISPATCH();
}

HANDLER(index_field_get_raw_float32_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_FLOAT32, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_int32_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT32, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_float32_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_FLOAT32, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int32_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT32, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_float32_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_FLOAT32, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_real_unchecked_add)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_add_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot, RAWW_REAL, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_float32_unchecked_add)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_add_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot, RAWW_FLOAT32, false);
    DISPATCH();
}


/* `field += a*b` in one dispatch -- see OP_FIELD_COMPOUND_RAW_FLOAT32_FMA (vm.h). The multiply is
   parenthesised to keep the same rounding the unfused pair had. */
HANDLER(field_compound_raw_float32_fma)
    int struct_reg = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    unsigned int foffset = READ();
    AerVal obj = registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) {
        error("internal error: specialized struct field access on a non-struct value");
        DISPATCH();
    }
    unsigned char* slot = aer_as_struct(obj)->fields + foffset;
    vm_raw_write_float32(slot, vm_raw_read_float32(slot) + (registers[a].as.d * registers[b].as.d));
    DISPATCH();
}

HANDLER(index_field_compound_raw_real_unchecked_fma)
    int arr_reg = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    uint32_t field_rk_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = vm_packed_raw_elem_unchecked(vm, pc, registers[arr_reg], idx, foffset);
    if (!elem)
        DISPATCH();
    double lhs;
    memcpy(&lhs, elem, 8);
    lhs = lhs + (registers[a].as.d * registers[b].as.d);
    memcpy(elem, &lhs, 8);
    DISPATCH();
}

/* `[value; count]` -- fill_reg is already evaluated exactly once by the parser, so this branches on
   its RUNTIME type. Eligibility (every field a fixed primitive) has to be checked here rather than
   at compile time: a Shape is only fully known once its OP_DEFINE_STRUCT has run. */
HANDLER(array_repeat)
    int dest_reg = (int)UNPACK_A(op_word);
    int fill_reg = (int)UNPACK_B(op_word);
    int narrow_flag = (int)UNPACK_C(op_word);
    uint32_t count_word = READ();
    AerVal* count_v = vm_rk_ptr16(registers, const_pool, (uint16_t)count_word);
    if (aer_type(*count_v) != TYPE_INTEGER) {
        error("Repeat-literal array count must be an integer");
        DISPATCH();
    }
    int64_t count = aer_as_int(*count_v);
    if (count < 0) {
        error("Repeat-literal array count must not be negative");
        DISPATCH();
    }

    AerVal fill = registers[fill_reg];
    if (aer_type(fill) == TYPE_STRUCT) {
        AerStruct* src = aer_as_struct(fill);
        Shape* shape = src->shape;
        for (unsigned int i = 0; i < shape->field_count; i++) {
            ValueType ft = shape->field_types[i];
            if (ft != TYPE_INTEGER && ft != TYPE_REAL && ft != TYPE_BOOLEAN) {
                const char* got = ft == TYPE_ANY ? "untyped (no annotation)" : aer_value_type_names[ft];
                error("'%s' cannot be packed into an array: field '%s' must be integer/float/boolean, not %s",
                      aer_as_string(c->pool[shape->name])->data,
                      aer_as_string(c->pool[shape->field_names[i]])->data, got);
                DISPATCH();
            }
        }
        /* A narrow (int32/float32) field packs fine here -- every packed-array element read/write
           (this handler and OP_INDEX_FIELD_GET/SET/COMPOUND's packed branches) uses
           shape->instance_bytes as the real per-element stride, not a hardcoded 8-bytes-per-field
           assumption, so a mix of narrow and wide fields lays out correctly either way. */
        unsigned int element_size = shape->instance_bytes;
        AerPackedArray* pa = heap_new_packed_array(&vm->heap, shape, (unsigned int)count);
        /* Every eligible field is raw -- 8 bytes, or 4 if narrow (TYPE_ANY, the only field kind
           needing a full boxed AerVal, was already rejected above) -- so src->fields IS one
           element's worth of bytes at exactly instance_bytes, laid out identically to a packed
           element: a straight memcpy per element, not a field-by-field copy. src's OWN field values
           are replicated, not the Shape's defaults, so `[Particle(1.0, 2.0); n]` differs from
           `[Particle(); n]`. */
        for (int64_t e = 0; e < count; e++)
            memcpy(pa->data + (size_t)e * element_size, src->fields, element_size);
        registers[dest_reg] = aer_packed_array_val(pa);
    } else if (aer_type(fill) == TYPE_INTEGER || aer_type(fill) == TYPE_REAL ||
               aer_type(fill) == TYPE_BOOLEAN) {
        /* narrow_flag is a pure parse-time decision (was the fill expression written as an
           `i`/`f`-suffixed literal directly in this position?) -- by construction, that always
           agrees with fill's own runtime tag (an `i`-suffixed literal is always a TYPE_INTEGER
           token, `f` always TYPE_REAL), so no cross-check against narrow_flag is needed here. */
        TypedArrayElemKind kind;
        if (narrow_flag == 1)
            kind = TYPED_ELEM_INT32;
        else if (narrow_flag == 2)
            kind = TYPED_ELEM_FLOAT32;
        else if (aer_type(fill) == TYPE_BOOLEAN)
            kind = TYPED_ELEM_BOOL;
        else
            kind = (aer_type(fill) == TYPE_INTEGER) ? TYPED_ELEM_INT64 : TYPED_ELEM_FLOAT64;
        unsigned int width = vm_typed_elem_width(kind);
        AerTypedArray* ta = heap_new_typed_array(&vm->heap, kind, (unsigned int)count);
        for (int64_t e = 0; e < count; e++)
            vm_typed_elem_write(ta->data + (size_t)e * width, kind, fill);
        registers[dest_reg] = aer_typed_array_val(ta);
    } else if (aer_type(fill) == TYPE_TYPED_ARRAY) {
        /* `[[0.0; cols]; rows]`. Each row is its own COPY of the fill, never one array referenced
           `count` times -- sharing it would make `m[0][0] = x` write every row, which is the trap
           Python's `[[0] * cols] * rows` is famous for. Nothing here collects, so the rows are safe
           to build before the outer array roots them (gc_maybe_collect runs after). */
        AerTypedArray* src = aer_as_typed_array(fill);
        unsigned int width = vm_typed_elem_width(src->elem_kind);
        AerArray* rows = heap_new_array(&vm->heap, count > 0 ? (unsigned int)count : 4);
        rows->count = (unsigned int)count;
        for (int64_t e = 0; e < count; e++) {
            AerTypedArray* row = vm_new_typed_array(src->elem_kind, src->count);
            if (src->count > 0)
                memcpy(row->data, src->data, (size_t)src->count * width);
            rows->items[e] = aer_typed_array_val(row);
        }
        registers[dest_reg] = aer_array_val(rows);
    } else {
        error("Cannot build a repeat-literal array from a %s value -- the fill value must be a "
              "number, a boolean, a struct instance, or a typed array",
              vm_type_name(c, fill));
        DISPATCH();
    }
    gc_maybe_collect(vm);
    DISPATCH();
}

/* Handles both a packed array and an ordinary struct array (the parser can't know which --
   functions are untyped). The non-packed branch reproduces the plain index+field path exactly,
   just without needing a scratch register for the intermediate. */
SEPARATE_HANDLER(index_field_get_any_container)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int dest_reg = (int)UNPACK_A(op_word);
    int obj_reg = (int)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        uint64_t i;
        if (!vm_array_position(*idx, pa->count, &i)) {
            vm_array_index_error(*idx, pa->count);
            DISPATCH();
        }
        int slot;
        unsigned int foffset;
        ValueType ftype;
        bool narrow;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
            DISPATCH();
        unsigned int element_size = pa->shape->instance_bytes;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        registers[dest_reg] =
            narrow ? vm_narrow_field_read(elem, ftype)
                   : vm_packed_slot_read(elem, ftype);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm); /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    registers[dest_reg] = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    DISPATCH();
}

HANDLER(index_field_get)
    AerVal obj = registers[(int)UNPACK_B(op_word)];
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(pc[0]));
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (aer_type(obj) != TYPE_PACKED_ARRAY || idx->tag != TYPE_INTEGER)
        __attribute__((musttail)) return h_index_field_get_any_container(vm, pc, registers, c);
    AerPackedArray* pa = aer_as_packed_array(obj);
    if ((uint64_t)idx->as.i >= pa->count || entry->shape != pa->shape)
        __attribute__((musttail)) return h_index_field_get_any_container(vm, pc, registers, c);
    (void)READ();
    unsigned char* elem = pa->data + (size_t)idx->as.i * pa->shape->instance_bytes + entry->offset;
    registers[(int)UNPACK_A(op_word)] =
        entry->narrow
            ? vm_narrow_field_read(elem, entry->ftype)
            : vm_packed_slot_read(elem, entry->ftype);
    DISPATCH();
}

/* Mirror of h_index_field_get -- same dual dispatch, same reason no scratch register is needed. */
SEPARATE_HANDLER(index_field_set_any_container)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int obj_reg = (int)UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    uint32_t field_val_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_val_word);
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_val_word));
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        uint64_t i;
        if (!vm_array_position(*idx, pa->count, &i)) {
            vm_array_index_error(*idx, pa->count);
            DISPATCH();
        }
        int slot;
        unsigned int foffset;
        ValueType declared;
        bool narrow;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &declared, &narrow))
            DISPATCH();
        if (val->tag != declared) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        if (!vm_check_narrow_field_write(declared, narrow, *val))
            DISPATCH();
        unsigned int element_size = pa->shape->instance_bytes;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        if (narrow)
            vm_narrow_field_write(elem, declared, *val);
        else
            vm_packed_slot_write(elem, declared, *val);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm); /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot;
    unsigned int foffset;
    ValueType declared;
    bool narrow;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &declared, &narrow))
        DISPATCH();
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(declared, narrow, *val))
        DISPATCH();
    gc_barrier_struct(vm, oa, *val);
    vm_struct_field_write_at(oa, foffset, declared, narrow, *val);
    DISPATCH();
}

HANDLER(index_field_set)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(pc[0]));
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (aer_type(obj) != TYPE_PACKED_ARRAY || idx->tag != TYPE_INTEGER)
        __attribute__((musttail)) return h_index_field_set_any_container(vm, pc, registers, c);
    AerPackedArray* pa = aer_as_packed_array(obj);
    if ((uint64_t)idx->as.i >= pa->count || entry->shape != pa->shape || val->tag != entry->ftype ||
        !vm_fits_narrow_field(entry->ftype, entry->narrow, *val))
        __attribute__((musttail)) return h_index_field_set_any_container(vm, pc, registers, c);
    (void)READ();
    unsigned char* elem = pa->data + (size_t)idx->as.i * pa->shape->instance_bytes + entry->offset;
    if (entry->narrow)
        vm_narrow_field_write(elem, entry->ftype, *val);
    else
        vm_packed_slot_write(elem, entry->ftype, *val);
    DISPATCH();
}

/* `obj[index].field OP= rk_rhs` -- resolves the index+field exactly once (same dual packed-array/
   struct-instance dispatch as h_index_field_get/set), reads, computes, type-checks, and writes
   back in one dispatch. One resolution, not the two a separate read and write would each pay --
   the nbody-style `bodies[j].vx += dx * mi` pattern. */
HANDLER(index_field_compound)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int obj_reg = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_idx_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_idx_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_idx_word));
    uint32_t rhs_word = READ();
    AerVal* rhs = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(rhs_word));
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        uint64_t i;
        if (!vm_array_position(*idx, pa->count, &i)) {
            vm_array_index_error(*idx, pa->count);
            DISPATCH();
        }
        int slot;
        unsigned int foffset;
        ValueType ftype;
        bool narrow;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
            DISPATCH();
        unsigned int element_size = pa->shape->instance_bytes;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        AerVal lhs =
            narrow ? vm_narrow_field_read(elem, ftype)
                   : vm_packed_slot_read(elem, ftype);
        bool handled;
        AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ftype, aer_type(*rhs), &handled);
        if (!handled) {
            result = vm_binary_cold(c, lhs, *rhs, bin_op, ftype, aer_type(*rhs));
            gc_maybe_collect(vm);
        }
        if (result.tag != ftype) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        if (!vm_check_narrow_field_write(ftype, narrow, result))
            DISPATCH();
        if (narrow)
            vm_narrow_field_write(elem, ftype, result);
        else
            vm_packed_slot_write(elem, ftype, result);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm); /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        result = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    if (ftype != TYPE_ANY && result.tag != ftype) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(ftype, narrow, result))
        DISPATCH();
    gc_barrier_struct(vm, oa, result);
    vm_struct_field_write_at(oa, foffset, ftype, narrow, result);
    DISPATCH();
}

/* Keyed by unary_op, same tag convention as h_binary. Also folds in OP_TO_STR
   (interpolation's string conversion) via the shared vm_to_str(). */
HANDLER(interp)
    int dest = (int)UNPACK_A(op_word);
    unsigned int count = UNPACK_B(op_word);
    AerVal parts[INTERP_MAX_PARTS];
    for (unsigned int i = 0; i < count; i++)
        parts[i] = *vm_rk_ptr16(registers, const_pool, READ());
    registers[dest] = vm_interp_build(vm, parts, count);
    gc_maybe_collect(vm);
    DISPATCH();
}

HANDLER(unary)
    int dest = (int)UNPACK_A(op_word);
    Opcode unary_op = (Opcode)UNPACK_B(op_word);
    AerVal v = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal* result = &registers[dest];
    switch (unary_op) {
        case OP_NEGATE:
            if (aer_type(v) == TYPE_INTEGER)
                *result = aer_int(-aer_as_int(v));
            else if (aer_type(v) == TYPE_REAL)
                *result = aer_real(-aer_as_real(v));
            else {
                error("Negation requires a numeric type");
                *result = aer_null();
            }
            break;
        case OP_NOT: *result = aer_bool(!vm_truthy(v)); break;
        case OP_BITWISE_NOT:
            if (aer_type(v) != TYPE_INTEGER) {
                error("Bitwise NOT requires an integer");
                *result = aer_null();
            } else
                *result = aer_int(~aer_as_int(v));
            break;
        case OP_TO_STR:
            /* The only allocating case -- NEGATE/NOT/BITWISE_NOT only ever produce plain tagged-union
               values. */
            *result = vm_to_str(c, v);
            gc_maybe_collect(vm);
            break;
        default: *result = aer_null(); break;
    }
    DISPATCH();
}

/* No gc_maybe_collect -- every cast_type only ever produces a plain tagged-union value
   (confirmed by inspection, including vm_cast's string-parsing sub-cases). */
HANDLER(cast)
    int dest = (int)UNPACK_A(op_word);
    int cast_type = (int)UNPACK_B(op_word);
    AerVal v = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    registers[dest] = vm_cast(v, cast_type);
    DISPATCH();
}

HANDLER(raw_int_to_real)
    SCALED_REG(UNPACK_A(op_word)) = aer_real((double)SCALED_REG(UNPACK_B(op_word)).as.i);
    DISPATCH();
}

HANDLER(raw_real_to_int)
    registers[UNPACK_A(op_word)] = aer_int((int64_t)registers[UNPACK_B(op_word)].as.d);
    DISPATCH();
}

/* Skip both the RK-flag check and the tag check -- the parser already proved every operand's
   type at compile time. None allocate: integers/reals/booleans never have heap cells. */
HANDLER(raw_load_int)
    unsigned int dest = UNPACK_A(op_word);
    /* Full 32-bit signed immediate in its own dedicated word, so no literal reaching here can
       truncate -- a narrower field silently turned a 20M-iteration bound into 77056. */
    SCALED_REG(dest) = aer_int((int32_t)READ());
    DISPATCH();
}

HANDLER(raw_load_real)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int idx = (unsigned int)READ();
    SCALED_REG(dest) = aer_real(c->rawk_d[idx]);
    DISPATCH();
}

/* Source-level dedup only: each invocation still expands to its own full label and body. NOT a
   shared noinline function, which would put a call in the dispatch loop for a code-size win these
   workloads do not need. #undef'd after the last family that uses them.

   RAW_I/RAW_D: the RK8 const flag picks the raw constant table, so a literal needs no slot. A slot
   index is at most FRAME_REGISTERS-1 (127) and the flag is 0x80, so the two never collide. */
#define RAW_I(x) (RK8_IS_CONST(x) ? c->rawk_i[RK8_INDEX(x)] : registers[x].as.i)
#define RAW_D(x) (RK8_IS_CONST(x) ? c->rawk_d[RK8_INDEX(x)] : registers[x].as.d)

#define RAW_ARITH_INT(name, op)                                                                              \
    static VmSliceResult h_raw_##name##_int(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {       \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        unsigned int dest = UNPACK_A(op_word);                                                               \
        unsigned int a = UNPACK_B(op_word);                                                                  \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        SCALED_REG(dest) = aer_int(SCALED_REG(a).as.i op SCALED_REG(b).as.i);                                \
        DISPATCH();                                                                                          \
    }
/* The C field is a bare rawk_i index, not an RK -- the opcode itself already says "constant", so
   all 8 bits are index and no flag needs testing. */
#define RAW_ARITH_INT_K(name, op)                                                                            \
    static VmSliceResult h_raw_##name##_int_k(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {     \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        unsigned int dest = UNPACK_A(op_word);                                                               \
        unsigned int a = UNPACK_B(op_word);                                                                  \
        unsigned int k = UNPACK_C(op_word);                                                                  \
        SCALED_REG(dest) = aer_int(SCALED_REG(a).as.i op c->rawk_i[k]);                                      \
        DISPATCH();                                                                                          \
    }
/* Writes the payload and leaves the tag: the destination is a real slot, whose tag frame entry
   already set and nothing since can have changed (frame_init_tags, vm.h). One 8-byte store instead
   of a 16-byte one, on the instruction a numeric loop spends most of its dispatches in. */
#define RAW_ARITH_REAL(name, op)                                                                             \
    static VmSliceResult h_raw_##name##_real(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {      \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        unsigned int dest = UNPACK_A(op_word);                                                               \
        unsigned int a = UNPACK_B(op_word);                                                                  \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        SCALED_REG(dest).as.d = SCALED_REG(a).as.d op SCALED_REG(b).as.d;                                    \
        DISPATCH();                                                                                          \
    }
#define RAW_CMP_INT(name, op)                                                                                \
    static VmSliceResult h_raw_##name##_int(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {       \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        unsigned int dest = UNPACK_A(op_word);                                                               \
        unsigned int a = UNPACK_B(op_word);                                                                  \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        SCALED_REG(dest) = aer_bool(SCALED_REG(a).as.i op RAW_I(b));                                         \
        DISPATCH();                                                                                          \
    }
#define RAW_CMP_REAL(name, op)                                                                               \
    static VmSliceResult h_raw_##name##_real(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {      \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        unsigned int dest = UNPACK_A(op_word);                                                               \
        unsigned int a = UNPACK_B(op_word);                                                                  \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        SCALED_REG(dest) = aer_bool(SCALED_REG(a).as.d op RAW_D(b));                                         \
        DISPATCH();                                                                                          \
    }
    RAW_ARITH_INT(add, +)
    RAW_ARITH_INT(sub, -)
    RAW_ARITH_INT(mul, *)
    RAW_ARITH_INT_K(add, +)
    RAW_ARITH_INT_K(sub, -)

/* The zero divisor for all three integer division forms. Separate because error() is variadic and
   one call frames the whole handler, which the arithmetic path should not pay for; the store after
   it stays because error() only longjmps when an unwind target is set, and returns otherwise. */
SEPARATE_HANDLER(raw_int_div_zero)
    Opcode op = (Opcode)(op_word & 0xFF);
    unsigned int dest = UNPACK_A(op_word);
    error(op == OP_RAW_MOD_INT ? "Modulo by zero" : "Division by zero");
    SCALED_REG(dest) = (op == OP_RAW_DIV_INT) ? aer_real(0.0) : aer_int(0);
    DISPATCH();
}

/* Matches OP_DIV's own semantics: int/int division always promotes to float, so this is the one
   OP_RAW_*_INT opcode whose dest is registers[].as.d, not registers[].as.i. */
HANDLER(raw_div_int)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int a = UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    int64_t rv = SCALED_REG(b).as.i;
    if (rv == 0)
        __attribute__((musttail)) return h_raw_int_div_zero(vm, pc, registers, c);
    SCALED_REG(dest) = aer_real((double)SCALED_REG(a).as.i / (double)rv);
    DISPATCH();
}

HANDLER(raw_mod_int)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int a = UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    int64_t rv = SCALED_REG(b).as.i;
    if (rv == 0)
        __attribute__((musttail)) return h_raw_int_div_zero(vm, pc, registers, c);
    SCALED_REG(dest) = aer_int(aer_mod_int64(SCALED_REG(a).as.i, rv));
    DISPATCH();
}

HANDLER(raw_floor_div_int)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int a = UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    int64_t rv = SCALED_REG(b).as.i;
    if (rv == 0)
        __attribute__((musttail)) return h_raw_int_div_zero(vm, pc, registers, c);
    SCALED_REG(dest) = aer_int((int64_t)floor((double)SCALED_REG(a).as.i / (double)rv));
    DISPATCH();
}

    RAW_ARITH_REAL(add, +)
    RAW_ARITH_REAL(sub, -)
    RAW_ARITH_REAL(mul, *)

/* Superinstruction for `x += a*b` / `x -= a*b` on raw real locals -- see OP_RAW_FMA_REAL's own
   comment (vm.h) for what emits this and why it's still bit-identical to the unfused two-opcode
   form (two roundings, not a hardware single-rounding FMA). dest doubles as the accumulator's
   read source and write destination, same in-place convention RAW_ARITH_REAL's own dest==a
   callers already rely on. */
#define RAW_FUSED_MULACC_REAL(name, op)                                                                      \
    static VmSliceResult h_raw_##name##_real(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {      \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        unsigned int dest = UNPACK_A(op_word);                                                               \
        unsigned int a = UNPACK_B(op_word);                                                                  \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        SCALED_REG(dest).as.d = SCALED_REG(dest).as.d op(SCALED_REG(a).as.d * SCALED_REG(b).as.d);           \
        DISPATCH();                                                                                          \
    }
    RAW_FUSED_MULACC_REAL(fma, +)
    RAW_FUSED_MULACC_REAL(fms, -)
#undef RAW_FUSED_MULACC_REAL

HANDLER(raw_div_real)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int a = UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    double rv = SCALED_REG(b).as.d;
    if (rv == 0.0) {
        error("Division by zero");
        SCALED_REG(dest) = aer_real(0.0);
    } else
        SCALED_REG(dest) = aer_real(SCALED_REG(a).as.d / rv);
    DISPATCH();
}

    /* Comparisons produce a boxed boolean (no raw boolean type exists) -- dest is 7 bits, not 5. */
    RAW_CMP_INT(lt, <)
    RAW_CMP_INT(lte, <=)
    RAW_CMP_INT(eq, ==)
    RAW_CMP_INT(neq, !=)
    RAW_CMP_REAL(lt, <)
    RAW_CMP_REAL(lte, <=)
    RAW_CMP_REAL(eq, ==)
    RAW_CMP_REAL(neq, !=)

/* Checks a slot whose type nothing proved, so every opcode after it can skip the check. The error
   wording names the operator, not "unbox", because that is what the source line reads as. */
HANDLER(unbox_int)
    unsigned int dest = UNPACK_A(op_word);
    AerVal* v = &SCALED_REG(UNPACK_B(op_word));
    if (v->tag == TYPE_INTEGER)
        SCALED_REG(dest) = aer_int(v->as.i);
    else
        error("Cannot apply this operator to integer and %s", vm_type_name(c, *v));
    DISPATCH();
}

HANDLER(unbox_real)
    unsigned int dest = UNPACK_A(op_word);
    AerVal* v = &SCALED_REG(UNPACK_B(op_word));
    if (v->tag == TYPE_REAL)
        SCALED_REG(dest) = aer_real(v->as.d);
    else if (v->tag == TYPE_INTEGER)
        SCALED_REG(dest) = aer_real((double)v->as.i);
    else
        error("Cannot apply this operator to float and %s", vm_type_name(c, *v));
    DISPATCH();
}

HANDLER(raw_move_int)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int src = UNPACK_B(op_word);
    SCALED_REG(dest) = aer_int(SCALED_REG(src).as.i);
    DISPATCH();
}

HANDLER(raw_move_real)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int src = UNPACK_B(op_word);
    SCALED_REG(dest) = aer_real(SCALED_REG(src).as.d);
    DISPATCH();
}

#undef RAW_ARITH_INT
#undef RAW_ARITH_REAL
#undef RAW_CMP_INT
#undef RAW_CMP_REAL

HANDLER(raw_load_int_pool)
    unsigned int dest = UNPACK_A(op_word);
    unsigned int idx = (unsigned int)READ();
    SCALED_REG(dest) = aer_int(c->rawk_i[idx]);
    DISPATCH();
}

/* Both operands raw: no tag, no table, no error path -- the comparison is the two loads the
   hardware would do anyway. */
#define RAW_CMP_JUMP_IF_FALSE(name, member, rhs, op)                                                            \
    static VmSliceResult h_raw_##name##_jump_if_false(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {\
        const uint32_t op_word = pc[-1];                                                                        \
        (void)op_word;                                                                                          \
        unsigned int a = UNPACK_B(op_word);                                                                     \
        unsigned int b = UNPACK_C(op_word);                                                                     \
        int target = READ();                                                                                    \
        if (!(SCALED_REG(a).as.member op rhs(b)))                                                               \
            pc += (int32_t)target;                                                                              \
        DISPATCH();                                                                                             \
    }

    RAW_CMP_JUMP_IF_FALSE(lt_int, i, RAW_I, <)
    RAW_CMP_JUMP_IF_FALSE(lte_int, i, RAW_I, <=)
    RAW_CMP_JUMP_IF_FALSE(eq_int, i, RAW_I, ==)
    RAW_CMP_JUMP_IF_FALSE(neq_int, i, RAW_I, !=)
    RAW_CMP_JUMP_IF_FALSE(lt_real, d, RAW_D, <)
    RAW_CMP_JUMP_IF_FALSE(lte_real, d, RAW_D, <=)
    RAW_CMP_JUMP_IF_FALSE(eq_real, d, RAW_D, ==)
    RAW_CMP_JUMP_IF_FALSE(neq_real, d, RAW_D, !=)

#undef RAW_CMP_JUMP_IF_FALSE

/* See OP_RAW_INC_LTE_INT_JUMP_IF_FALSE (vm.h). The increment writes the tag for the same reason
   RAW_ARITH_INT does -- a raw int slot doubles as a boxed integer. The limit is a plain slot, not
   an RK: the form this fuses always had it in one. */
HANDLER(raw_inc_lte_int_jump_if_false)
    unsigned int counter = UNPACK_A(op_word);
    unsigned int step = UNPACK_B(op_word);
    unsigned int limit = UNPACK_C(op_word);
    int64_t next = SCALED_REG(counter).as.i + SCALED_REG(step).as.i;
    SCALED_REG(counter) = aer_int(next);
    int target = READ();
    if (!(SCALED_REG(limit).as.i <= next))
        pc += (int32_t)target;
    DISPATCH();
}

/* The operand words are read by the helpers straight out of the instruction stream rather than
   into a local array. Both helpers read every part before dest is written, so dest may alias a
   part's register, which emit_interp deliberately arranges. */
HANDLER(index_get_interp)
    int dest_reg = (int)UNPACK_A(op_word);
    int obj_reg = (int)UNPACK_B(op_word);
    unsigned int count = UNPACK_C(op_word);
    const uint32_t* rks = pc;
    pc += count;
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_DICT &&
        vm_dict_get_interp(aer_as_dict(obj), rks, count, registers, const_pool, &registers[dest_reg]))
        DISPATCH();
    registers[dest_reg] = vm_index_get_interp_slow(vm, obj, rks, count, registers, const_pool);
    gc_maybe_collect(vm);
    DISPATCH();
}

/* The only handler that ends the chain rather than continuing it, so it is the only one that
   touches none of the four. */
static VmSliceResult h_halt(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {
    (void)vm;
    (void)pc;
    (void)registers;
    (void)c;
    return VM_SLICE_DONE;
}

#undef SYNC_IP
#undef error
#undef vm_binary_cold
#undef vm_index_get_compute
#undef vm_index_set_compute
#undef vm_array_index_error
#undef vm_in
#undef vm_cast
#undef vm_to_str
#undef vm_slice_bounds
#undef vm_call_builtin
#undef vm_call_value
#undef vm_call_module_dispatch
#undef vm_call_resolve_specialization
#undef vm_resolve_field
#undef vm_resolve_field_by_shape
#undef vm_check_narrow_field_write
#undef vm_typed_array_check
#undef vm_default_value
#undef aer_make_string_copy
#undef gc_maybe_collect
#undef HANDLER
#undef READ
#undef PUSH
#undef POP
#undef DISPATCH
#undef AER_PROFILE_HIT
#undef const_pool

/* A parser tag's slot stays NULL: no instruction ever carries it. */
#define h_none NULL
#define OPCODE(name, handler, ...) [OP_##name] = h_##handler,
static const OpHandler aer_handlers[256] = {
#include "opcodes.def"
};
#undef OPCODE
#undef h_none

/* max_instructions == 0 means unlimited (every existing caller via the vm_run() wrapper below) --
   the budget decrement only happens at loop-back-edge and call opcodes (the only places a script
   can spend unbounded time), not on every DISPATCH(), so the check costs nothing on the common
   unlimited path and stays cheap even when a budget is active. See aer_scheduler.c for the caller
   that actually uses a nonzero budget. */
VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions) {
    Chunk* c = vm->chunk;
    /* Hoisted once -- c->pool is only mutated at parse time, stable for the whole call. */
    /* Installs this call's error catch point, saving the previous one so nested vm_run calls
       catch their own errors and unwind no further than here; restored on every return. */
    AerJmpBuf catch_point;
    AerJmpBuf* saved_unwind_target = runtime_error_unwind_target;
    runtime_error_unwind_target = &catch_point;
    /* One store here + one restore at each exit, instead of every DISPATCH() -- the VM never
       changes mid-call. */
    VM* saved_active_vm = vm_active_error_vm();
    vm_set_active_error_vm(vm);
    /* Same save/restore shape as vm_active_error_vm() just above -- a nested vm_run_slice (module
       instantiation, actor.call) must allocate into ITS OWN heap while it runs, then hand
       allocation back to whichever heap was active before it, once it returns. */
    VmHeap* saved_current_heap = vm_current_heap();
    vm_set_current_heap(&vm->heap);
/* Every exit restores all three, yields included: a stale unwind target longjmps into dead stack,
   and a stale heap sends the next allocation somewhere this call no longer owns. */
#define SLICE_RETURN(result)                                                                                 \
    do {                                                                                                     \
        runtime_error_unwind_target = saved_unwind_target;                                                   \
        vm_set_active_error_vm(saved_active_vm);                                                             \
        vm_set_current_heap(saved_current_heap);                                                             \
        return (result);                                                                                     \
    } while (0)
    if (AER_SETJMP(catch_point) != 0)
        SLICE_RETURN(VM_SLICE_ERROR);
    /* The program counter is a moving pointer, not a `code` + offset pair: READ() is then a single
       post-indexed load instead of a base reload, an index-scaled load and a separate increment.
       Only vm_call_resolve_specialization can realloc `code` mid-slice, and it converts to an
       offset and back across that one call. vm->ip stays an offset, which is what everything
       outside this function -- error lines, return addresses, yields -- expects. */
    const uint32_t* pc = c->code + vm->ip;
    /* The active frame's three windows, hoisted: stable for the whole call, refreshed only at the
       3 call/return sites below. Reloading them per opcode was among the hottest instructions in
       the dispatch loop (perf annotate, nbody). The backing stacks are fixed-size inline VM
       arrays, never reallocated, so caching them across dispatches is safe. */
    AerVal* registers = vm->call_stack[vm->call_depth].registers;
    vm->slice_max = max_instructions;
    vm->slice_budget = max_instructions;
    chunk_ensure_debug_hits(c);
    chunk_ensure_field_cache(c);

    /* An ordinary call, not a tail call: this frame owns catch_point, so it has to outlive the
       whole chain -- every handler longjmps back here. */
    uint32_t first_word = *pc++;
    VmSliceResult result = aer_handlers[first_word & 0xFF](vm, pc, registers, c);
    SLICE_RETURN(result);
}

bool vm_run(VM* vm) {
    return vm_run_slice(vm, 0) == VM_SLICE_DONE;
}
