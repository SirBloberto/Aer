#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_host.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"
#include "parser.h"
#include "typed_array.h"
#include "value_ops.h"
#include "vm.h"
#include "vm_internal.h"

/* Call setup                                                       */

/* A baked default (struct field or function parameter): primitives copy as-is; an array/dict
   default must become a FRESH empty container, or every omitted call/instance would alias the
   same one (Python's mutable-default bug). parser.c only ever bakes an empty '[]'/'{}' as such
   a default, so a fresh empty one is always correct -- no deep copy needed. */
AerVal vm_default_value(VM* vm, AerVal dflt) {
    if (aer_type(dflt) == TYPE_ARRAY && !aer_as_array(dflt)->shape)
        return aer_array_val(heap_new_array(&vm->heap, 0));
    if (aer_type(dflt) == TYPE_DICT) {
        return aer_dict_val(heap_new_dict(&vm->heap));
    }
    return dflt;
}


/* Makes room for a frame starting at base: a free call-stack entry, and FRAME_REGISTERS slots of bank
   from base. False only when the depth ceiling is reached, the caller's overflow error. Growing the
   bank moves every frame's registers, so the caller recomputes anything it derived from them. */
bool vm_grow_for_push(VM* vm, AerVal* base) {
    if (vm->call_depth + 1 >= vm->call_depth_limit) {
        if (vm->call_depth_limit >= VM_CALL_MAX)
            return false;
        int old_frames = vm->call_depth_limit;
        int frames = old_frames * 2 > VM_CALL_MAX ? VM_CALL_MAX : old_frames * 2;
        vm->call_stack = xrealloc(vm->call_stack, (size_t)frames * sizeof(CallFrame));
        memset(vm->call_stack + old_frames, 0, (size_t)(frames - old_frames) * sizeof(CallFrame));
        vm->call_depth_limit = frames;
    }
    size_t needed = (size_t)(base - vm->register_stack) + FRAME_REGISTERS;
    if (needed > vm->register_capacity) {
        size_t capacity = vm->register_capacity * 2;
        while (capacity < needed)
            capacity *= 2;
        AerVal* old_bank = vm->register_stack;
        AerVal* bank = xmalloc(capacity * sizeof(AerVal));
        memcpy(bank, old_bank, vm->register_capacity * sizeof(AerVal));
        /* Rebased against the old bank while it is still allocated, so no offset has to be stashed. */
        for (int i = 0; i <= vm->call_depth; i++)
            vm->call_stack[i].registers = bank + (vm->call_stack[i].registers - old_bank);
        free(old_bank);
        vm->register_stack = bank;
        vm->register_capacity = capacity;
        vm->push_base_limit = bank + (capacity - FRAME_REGISTERS);
    }
    return true;
}

/* Cross-module call setup (aer_module_call): mirrors h_call_value's frame-push, standalone
   since this isn't inside vm_run's dispatch loop. dest_reg fixed at 0 sets up the frame right
   above target's own frame 0, so once vm_run(target) drains back to depth 0, the result sits
   in target->call_stack[0].registers[0]. */
bool setup_call(VM* target, ChunkFunction* fn, int arg_count, AerVal* args, unsigned int return_ip) {
    if (arg_count < (int)fn->min_arity || arg_count > (int)fn->arity) {
        if (fn->min_arity == fn->arity)
            error("Function expects %u arguments, got %d", fn->arity, arg_count);
        else
            error("Function expects between %u and %u arguments, got %d", fn->min_arity, fn->arity,
                  arg_count);
        return false;
    }
    CallFrame* caller = &target->call_stack[target->call_depth];
    AerVal* callee_regs = caller->registers + caller->frame_size;
    if (target->call_depth + 1 >= target->call_depth_limit || callee_regs > target->push_base_limit) {
        if (!vm_grow_for_push(target, callee_regs)) {
            error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
            return false;
        }
        caller = &target->call_stack[target->call_depth];
        callee_regs = caller->registers + caller->frame_size;
    }
    CallFrame* callee = caller + 1;
    callee->registers = callee_regs;
    callee->frame_size = fn->max_registers;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = args[i];
    for (int i = arg_count; i < (int)fn->arity; i++)
        callee_regs[i] = vm_default_value(target, fn->defaults[i - fn->min_arity]);
    /* Same reason as h_call's own -- everything below frame_size gets traced. */
    frame_init_tags(callee_regs, fn->arity, fn->frame_bounds);
    callee->frame_bounds = fn->frame_bounds;
    callee->return_ip = return_ip;
    callee->dest_reg = 0;
    callee->code_offset = fn->code_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = true;
    target
        ->call_depth++; /* same rooting rule as vm_call_value's non-tail branch (above) -- the defaults loop wrote into callee->registers[] before this point */
    gc_maybe_collect(target);
    target->ip = fn->code_offset;
    return true;
}

/* Factored out of h_call_value since a plain function can't itself jump to a vm_run-local
   label -- it does the work and lets the caller DISPATCH(). return_ip is the caller's own
   resume address, passed explicitly rather than read from vm->ip. */
void vm_call_value(VM* vm, AerVal fv, int dest_reg, int arg_reg_base, int arg_count, bool is_tail_call,
                          unsigned int return_ip) {
    if (aer_type(fv) != TYPE_FUNCTION) {
        return error("Value of type '%s' is not callable", vm_type_name(vm->chunk, fv));
    }
    AerFunction* f = aer_as_function(fv);
    if (arg_count < (int)f->min_arity || arg_count > (int)f->arity) {
        if (f->min_arity == f->arity)
            error("Function expects %u arguments, got %d", (unsigned int)f->arity, arg_count);
        else
            return error("Function expects between %u and %u arguments, got %d", (unsigned int)f->min_arity,
                         (unsigned int)f->arity, arg_count);
    }
    /* Tail-call reuse -- `f` is already a plain pointer, so overwriting its source register during
       the copy can't invalidate it. Args copied before defaults, so no source register is
       overwritten before it's read. */
    if (is_tail_call) {
        AerVal* cur = vm->call_stack[vm->call_depth].registers;
        for (int i = 0; i < arg_count; i++)
            cur[i] = cur[arg_reg_base + i];
        for (int i = arg_count; i < (int)f->arity; i++)
            cur[i] = vm_default_value(vm, f->defaults[i - f->min_arity]);
        gc_maybe_collect(
            vm); /* defaults just written into the CURRENT frame (tail call, call_depth unchanged) -- already rooted */
        vm->ip = f->code_offset;
        CallFrame* reused = &vm->call_stack[vm->call_depth];
        reused->code_offset = f->code_offset; /* reused frame now runs a different function */
        /* Same stale-sizing hazard as h_call's OP_TAIL_CALL branch (see its own comment) -- `f`
           may need a different max_registers than whatever function last occupied this frame.
           Must be refreshed here too, not just on the non-tail push path above. */
        reused->frame_size = f->max_registers;
        /* Growing the reused frame exposes registers the previous occupant never wrote, which
           mark_vm_roots would still trace -- same retagging as the two push paths. */
        frame_init_tags(reused->registers, f->arity, f->frame_bounds);
        reused->frame_bounds = f->frame_bounds;
        reused->tail_calls_collapsed++;
        return;
    }
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    AerVal* callee_regs = caller->registers + caller->frame_size;
    if (vm->call_depth + 1 >= vm->call_depth_limit || callee_regs > vm->push_base_limit) {
        if (!vm_grow_for_push(vm, callee_regs))
            return error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
        caller = &vm->call_stack[vm->call_depth];
        callee_regs = caller->registers + caller->frame_size;
    }
    CallFrame* callee = caller + 1;
    callee->registers = callee_regs;
    callee->frame_size = f->max_registers;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = caller->registers[arg_reg_base + i];
    for (int i = arg_count; i < (int)f->arity; i++)
        callee_regs[i] = vm_default_value(vm, f->defaults[i - f->min_arity]);
    /* Same reason as h_call's own -- everything below frame_size gets traced. */
    frame_init_tags(callee_regs, f->arity, f->frame_bounds);
    callee->frame_bounds = f->frame_bounds;
    callee->return_ip = return_ip;
    callee->dest_reg = dest_reg;
    callee->code_offset = f->code_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    vm->call_depth++; /* the defaults loop above wrote into callee->registers[] BEFORE this point, when mark_vm_roots's 0..call_depth scan didn't yet cover that frame -- gc_maybe_collect() must run AFTER this increment, not before, or a collection could reclaim a fresh default array/dict as unreachable */
    gc_maybe_collect(vm);
    vm->ip = f->code_offset;
}

/* Builtin function calls                                           */

/* builtin_id resolved at parse time. Returns true if arg_count matched, result in *out. */
bool vm_call_builtin(Chunk* c, int builtin_id, AerVal* args, int arg_count, AerVal* out) {
    *out = aer_null();

    switch (builtin_id) {
        case CALL_BUILTIN_LENGTH: {
            if (arg_count != 1)
                return false;
            AerVal a = args[0];
            if (aer_type(a) == TYPE_ARRAY)
                *out = aer_int((int64_t)aer_as_array(a)->count);
            else if (aer_type(a) == TYPE_STRING)
                *out = aer_int((int64_t)aer_as_string(a)->length);
            else if (aer_type(a) == TYPE_DICT)
                *out = aer_int((int64_t)aer_as_dict(a)->map.count);
            else if (aer_type(a) == TYPE_PACKED_ARRAY)
                *out = aer_int((int64_t)aer_as_packed_array(a)->count);
            else if (aer_type(a) == TYPE_TYPED_ARRAY)
                *out = aer_int((int64_t)aer_as_typed_array(a)->count);
            else
                error("length() requires an array, dict, string, packed array, or typed array");
            return true;
        }
        case CALL_BUILTIN_PRINT: {
            if (arg_count != 1)
                return false;
            vm_print_value(c, args[0], false);
            printf("\n");
            return true;
        }
        case CALL_BUILTIN_TYPE: {
            if (arg_count != 1)
                return false;
            const char* tn = vm_type_name(c, args[0]);
            /* Copies: an AerString always owns its data, with no exceptions. */
            *out = aer_make_string_copy(
                tn, (unsigned int)strlen(tn)); /* no chunk_add_pool interning -- see vm_to_str's comment */
            return true;
        }
        case CALL_BUILTIN_ASSERT: {
            if (arg_count != 2)
                return false;
            AerVal cond = args[0], msg = args[1];
            if (aer_type(msg) != TYPE_STRING) {
                error("assert() requires a string message as its second argument");
                return true;
            }
            if (!vm_truthy(cond)) {
                assert_failure_count++;
                AerString* ms = aer_as_string(msg);
                printf("ASSERT FAILED: %.*s\n", (int)ms->length, ms->data);
            }
            return true;
        }
        case CALL_BUILTIN_PANIC: {
            if (arg_count != 1)
                return false;
            AerVal msg = args[0];
            if (aer_type(msg) != TYPE_STRING) {
                error("panic() requires a string message");
                return true;
            }
            AerString* ms = aer_as_string(msg);
            error("panic: %.*s", (int)ms->length, ms->data);
            return true;
        }
        case CALL_BUILTIN_RESULT: {
            if (arg_count != 2)
                return false;
            bool value_is_null = aer_type(args[0]) == TYPE_NULL;
            bool err_is_null = aer_type(args[1]) == TYPE_NULL;
            if (value_is_null == err_is_null) {
                error("Result() requires exactly one of its two arguments to be null (the value on success, "
                      "the err on failure)");
                return true;
            }
            *out = aer_make_result(args[0], args[1]);
            return true;
        }
    }
    return false;
}

/* h_call's cold path. noinline is required: a static function with one call site is a prime
   candidate for LTO to inline right back in, which grows h_call from 848 bytes to 3KB
   for a path an ordinary call never takes. */
static void __attribute__((noinline))
vm_call_resolve_specialization_full(Chunk* c, ChunkFunction* target_f, AerVal* registers, int arg_reg_base,
                                    unsigned int ip, unsigned int* chosen_offset,
                                    unsigned int* chosen_max_registers, unsigned short* chosen_frame_bounds) {
    unsigned int site =
        ip - 3; /* this instruction's own word0 offset -- ip already advanced past all 3 words by now */
    /* Which argument register keys specialization. Only the first shape-sensitive parameter does;
       the others still work, they just never specialize. */
    int param_index = __builtin_ctz(target_f->shape_sensitive_mask);
    AerVal arg = registers[arg_reg_base + param_index];
    Shape* observed = NULL;
    SpecKind kind = SPEC_KIND_STRUCT;
    if (aer_type(arg) == TYPE_PACKED_ARRAY) {
        observed = aer_as_packed_array(arg)->shape;
        kind = SPEC_KIND_PACKED_ARRAY;
    } else if (aer_type(arg) == TYPE_STRUCT) {
        observed = aer_as_struct(arg)->shape;
        kind = SPEC_KIND_STRUCT;
    } else if (aer_type(arg) == TYPE_ARRAY) {
        /* A plain array carries no structural shape guarantee of its own (AerArray.shape is
           dead, always NULL) -- peek element 0 as a CANDIDATE shape only; the homogeneity scan
           just below is what actually earns the right to trust it, every single call, since
           unlike a packed array or a lone struct instance, a plain array's contents can differ
           from call to call (or even be heterogeneous outright). */
        AerArray* arr = aer_as_array(arg);
        if (arr->count > 0 && aer_type(arr->items[0]) == TYPE_STRUCT) {
            observed = aer_as_struct(arr->items[0])->shape;
            kind = SPEC_KIND_ARRAY_OF_STRUCTS;
        }
    }

    /* SPEC_KIND_ARRAY_OF_STRUCTS has no structural guarantee, so it re-verifies every call: the
       same array can be mutated to hold a different shape between calls. A mismatch falls back to
       the generic body for that call only, leaving the cache untouched.
       The scan is skipped when this site already verified this exact array, by pointer, at its
       current AerArray.generation -- generation only changes on restructuring, so a prior
       verification still holds. That is what makes a repeatedly-scanned array pay O(n) once. */
    if (kind == SPEC_KIND_ARRAY_OF_STRUCTS) {
        AerArray* arr = aer_as_array(arg);
        chunk_ensure_call_spec_cache(c);
        CallSpecCacheEntry* site_entry = &c->call_spec_cache[site];
        bool already_verified = site_entry->last_verified_array == arr &&
                                site_entry->last_verified_generation == arr->generation &&
                                site_entry->last_shape == observed;
        if (!already_verified) {
            for (unsigned int idx = 0; idx < arr->count; idx++) {
                AerVal item = arr->items[idx];
                if (aer_type(item) != TYPE_STRUCT || aer_as_struct(item)->shape != observed) {
                    observed = NULL;
                    break;
                }
            }
            if (observed) {
                site_entry->last_verified_array = arr;
                site_entry->last_verified_generation = arr->generation;
            }
        }
    }

    if (observed) {
        chunk_ensure_call_spec_cache(c);
        CallSpecCacheEntry* site_entry = &c->call_spec_cache[site];
        SpecEntry* entry = NULL;
        if (site_entry->last_shape == observed) {
            *chosen_offset = site_entry->last_code_offset;
            *chosen_max_registers = site_entry->last_max_registers;
            *chosen_frame_bounds = site_entry->last_frame_bounds;
            entry = site_entry->last_entry;
        } else {
            SpecEntry* found = NULL;
            for (int i = 0; i < target_f->specialization_count; i++)
                if (target_f->specializations[i].shape == observed) {
                    found = &target_f->specializations[i];
                    break;
                }
            if (!found && target_f->specialization_count < SPEC_MAX && !target_f->specializations)
                target_f->specializations = xcalloc(SPEC_MAX, sizeof(SpecEntry));
            if (!found && target_f->specialization_count < SPEC_MAX) {
                SpecEntry fresh;
                if (parser_specialize_function(c, target_f, observed, kind, param_index, &fresh, NULL, NULL,
                                               0)) {
                    /* The recompile appended code to this running chunk, so every per-word cache
                       must cover the new size before any site dispatches. debug_hits[] included:
                       DISPATCH() writes it per opcode while profiling, so skipping the resize is a
                       silent out-of-bounds write under --debug-path. */
                    chunk_ensure_debug_hits(c);
                    chunk_ensure_field_cache(c);
                    chunk_ensure_call_spec_cache(c);
                    site_entry =
                        &c->call_spec_cache
                             [site]; /* re-fetch: chunk_ensure_call_spec_cache may have reallocated the array */
                    target_f->specializations[target_f->specialization_count++] = fresh;
                    found = &target_f->specializations[target_f->specialization_count - 1];
                } else {
                    target_f->megamorphic =
                        true; /* treat a recompile failure like exhausting the table -- stop retrying every call */
                }
            } else if (!found) {
                target_f->megamorphic =
                    true; /* table full and still a new shape -- stop specializing this function */
            }
            if (found) {
                site_entry->last_shape = found->shape;
                site_entry->last_code_offset = found->code_offset;
                site_entry->last_max_registers = found->max_registers;
                site_entry->last_frame_bounds = found->frame_bounds;
                site_entry->last_entry = found;
                *chosen_offset = found->code_offset;
                *chosen_max_registers = found->max_registers;
                *chosen_frame_bounds = found->frame_bounds;
                entry = found;
            }
        }

        /* An axis independent of the shape lookup above, re-derived each call rather than cached --
           at most SPEC_MAX_RAW_PARAMS aer_type() reads, and `entry` already exposes the relevant
           raw_variant_* fields. See SpecEntry (vm.h) for why it is a separate variant. */
        /* SPEC_KIND_ARRAY_OF_STRUCTS was once excluded here over an apparent regression that perf
           counters did not reproduce -- cycles, instructions and branch-misses were all lower with
           the variant enabled, and the guarded build's own branch-misses varied 2x between
           identical runs. The guard was removed. */
        if (entry && entry->raw_param_count >= 0) {
            int cand_regs[SPEC_MAX_RAW_PARAMS];
            ValueType cand_types[SPEC_MAX_RAW_PARAMS];
            int cand_count = 0;
            for (unsigned int pi = 0; pi < target_f->arity && cand_count < SPEC_MAX_RAW_PARAMS; pi++) {
                if ((int)pi == param_index)
                    continue;
                AerVal pv = registers[arg_reg_base + pi];
                ValueType pt = aer_type(pv);
                if (pt != TYPE_INTEGER && pt != TYPE_REAL) {
                    cand_count = 0;
                    break;
                } /* not all-numeric -- no raw variant applies this call */
                cand_regs[cand_count] = (int)pi;
                cand_types[cand_count] = pt;
                cand_count++;
            }

            if (cand_count > 0) {
                bool matches_existing = entry->raw_param_count == cand_count;
                if (matches_existing) {
                    for (int k = 0; k < cand_count; k++)
                        if (entry->raw_param_regs[k] != cand_regs[k] ||
                            entry->raw_param_types[k] != cand_types[k]) {
                            matches_existing = false;
                            break;
                        }
                }
                if (matches_existing) {
                    *chosen_offset = entry->raw_variant_code_offset;
                    *chosen_max_registers = entry->raw_variant_max_registers;
                    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                } else if (entry->raw_param_count == 0) {
                    /* Never attempted for THIS entry -- try to compile it now. A failure here
                       (raw_ints/raw_reals budget exhausted -- realistic, since this shape's own
                       raw field usage already competes for the same 32-slot budget) sets
                       raw_param_count to -1, a PERMANENT bailout for this one SpecEntry only --
                       never retried every call, but also never affecting `megamorphic` or the
                       shape table, since this axis is fully decoupled from those. */
                    SpecEntry variant;
                    if (parser_specialize_function(c, target_f, observed, kind, param_index, &variant,
                                                   cand_regs, cand_types, cand_count)) {
                        chunk_ensure_debug_hits(c);
                        chunk_ensure_field_cache(c);
                        chunk_ensure_call_spec_cache(c);
                        entry->raw_variant_code_offset = variant.code_offset;
                        entry->raw_variant_max_registers = variant.max_registers;
                        entry->raw_variant_frame_bounds = variant.frame_bounds;
                        entry->raw_variant_frame_bounds = variant.frame_bounds;
                        for (int k = 0; k < cand_count; k++) {
                            entry->raw_param_regs[k] = cand_regs[k];
                            entry->raw_param_types[k] = cand_types[k];
                        }
                        entry->raw_param_count = cand_count;
                        *chosen_offset = entry->raw_variant_code_offset;
                        *chosen_max_registers = entry->raw_variant_max_registers;
                        *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                        *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                    } else {
                        entry->raw_param_count = -1;
                    }
                }
                /* else: entry already has a DIFFERENT raw-numeric variant than this call's
                   observed types -- don't attempt a second one, just use the baseline
                   offset/sizes already chosen above (still fully correct, just boxed dt again
                   for this one call). */
            }
        }
    }
}

/* A numeric-only function has no shape to key on, so the whole decision is whether every argument
   arrived with the types the variant was compiled for -- read straight from the argument registers,
   needing neither the site cache nor the shape table. specializations[0] holds it; shape stays NULL
   because nothing ever looks this entry up by shape. */
static void __attribute__((noinline)) vm_call_resolve_numeric(Chunk* c, ChunkFunction* target_f,
                                                              AerVal* registers, int arg_reg_base,
                                                              unsigned int* chosen_offset,
                                                              unsigned int* chosen_max_registers,
                                                              unsigned short* chosen_frame_bounds) {
    int cand_regs[SPEC_MAX_RAW_PARAMS];
    ValueType cand_types[SPEC_MAX_RAW_PARAMS];
    int cand_count = 0;
    /* Binds whichever parameters arrived numeric and leaves the rest boxed -- a collection
       parameter alongside a scalar one (scale_and_accumulate(nums, factor)) is an ordinary shape,
       and bailing on it left the scalar boxed through the whole body. */
    for (unsigned int pi = 0; pi < target_f->arity && cand_count < SPEC_MAX_RAW_PARAMS; pi++) {
        ValueType pt = aer_type(registers[arg_reg_base + pi]);
        if (pt != TYPE_INTEGER && pt != TYPE_REAL)
            continue;
        cand_regs[cand_count] = (int)pi;
        cand_types[cand_count] = pt;
        cand_count++;
    }
    if (cand_count == 0)
        return;

    if (!target_f->specializations)
        target_f->specializations = xcalloc(SPEC_MAX, sizeof(SpecEntry));
    SpecEntry* entry = &target_f->specializations[0];
    if (entry->raw_param_count < 0)
        return; /* permanently declined for this function */

    if (entry->raw_param_count == cand_count) {
        for (int k = 0; k < cand_count; k++)
            if (entry->raw_param_regs[k] != cand_regs[k] || entry->raw_param_types[k] != cand_types[k])
                return; /* a different numeric signature than the one compiled -- stay boxed */
        *chosen_offset = entry->raw_variant_code_offset;
        *chosen_max_registers = entry->raw_variant_max_registers;
        *chosen_frame_bounds = entry->raw_variant_frame_bounds;
        return;
    }
    if (entry->raw_param_count != 0)
        return;
    /* Warm up first -- see numeric_call_count (vm.h). Counted here rather than in h_call so an
       ordinary call pays nothing for it. */
    if (++target_f->numeric_call_count < NUMERIC_SPECIALIZE_AFTER)
        return;

    SpecEntry variant;
    if (!parser_specialize_function(c, target_f, NULL, SPEC_KIND_STRUCT, -1, &variant, cand_regs, cand_types,
                                    cand_count)) {
        entry->raw_param_count = -1;
        return;
    }
    chunk_ensure_debug_hits(c);
    chunk_ensure_field_cache(c);
    chunk_ensure_call_spec_cache(c);
    entry->raw_variant_code_offset = variant.code_offset;
    entry->raw_variant_max_registers = variant.max_registers;
    entry->raw_variant_frame_bounds = variant.frame_bounds;
    for (int k = 0; k < cand_count; k++) {
        entry->raw_param_regs[k] = cand_regs[k];
        entry->raw_param_types[k] = cand_types[k];
    }
    entry->raw_param_count = cand_count;
    *chosen_offset = entry->raw_variant_code_offset;
    *chosen_max_registers = entry->raw_variant_max_registers;
    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
}

/* The monomorphic case, split off so it does not pay for the full resolver's frame: that one is
   sized for the raw-variant block's candidate arrays and so also carries a stack-protector canary,
   both on every call regardless of which path runs. */
void __attribute__((noinline))
vm_call_resolve_specialization(Chunk* c, ChunkFunction* target_f, AerVal* registers, int arg_reg_base,
                               unsigned int ip, unsigned int* chosen_offset,
                               unsigned int* chosen_max_registers, unsigned short* chosen_frame_bounds) {
    if (target_f->shape_sensitive_mask == SHAPE_MASK_NUMERIC_ONLY) {
        vm_call_resolve_numeric(c, target_f, registers, arg_reg_base, chosen_offset, chosen_max_registers,
                                chosen_frame_bounds);
        return;
    }
    unsigned int site = ip - 3;
    /* arity 1 with a non-zero mask puts the shape-sensitive parameter at index 0, and leaves the
       raw-variant block inert (it skips that one parameter and there is no other), so a site-cache
       hit is the whole answer. */
    if (target_f->arity == 1 && site < c->call_spec_cache_cap &&
        aer_type(registers[arg_reg_base]) == TYPE_STRUCT &&
        c->call_spec_cache[site].last_shape == aer_as_struct(registers[arg_reg_base])->shape) {
        const CallSpecCacheEntry* hit = &c->call_spec_cache[site];
        *chosen_offset = hit->last_code_offset;
        *chosen_max_registers = hit->last_max_registers;
        *chosen_frame_bounds = hit->last_frame_bounds;
        return;
    }
    vm_call_resolve_specialization_full(c, target_f, registers, arg_reg_base, ip, chosen_offset,
                                        chosen_max_registers, chosen_frame_bounds);
}

/* h_call_module's cold path, split for the same reason as vm_call_resolve_specialization: letting
   LTO inline it grows h_call_module from 336 bytes to 13KB, all of it the ~10 external calls'
   argument shuffling. POP is safe here (no early-return, unlike PUSH), and the
   io-disabled error() longjmps before anything after it runs. Always returns true by the time it
   returns; the compiler just cannot prove it. */
bool __attribute__((noinline)) vm_call_module_dispatch(VM* vm, Chunk* c, int module_idx, int fn_idx,
                                                              int module_id, int fn_id, int arg_count) {
    bool handled;
    /* module_id/fn_id resolved at parse time -- a switch on two ints instead of a strcmp chain.
       CALL_MODULE_DYNAMIC (host/file module) still resolves by name at runtime. */
    switch (module_id) {
#define AER_MODULE_DISPATCH(id, str, call)                                                                   \
    case CALL_MODULE_##id: handled = call; break;
        AER_NATIVE_MODULES(AER_MODULE_DISPATCH)
#undef AER_MODULE_DISPATCH
        default: {
            const char* module = aer_as_string(c->pool[module_idx])->data;
            const char* fn = aer_as_string(c->pool[fn_idx])->data;
            /* io is the one module still reached through the generic host-call path
               (aer_host_call) rather than a fixed CALL_MODULE_* case -- gated here by name
               specifically so --no-io never touches a real embedding host's own custom modules,
               which go through this exact same path. */
            if (!vm->io_enabled && strcmp(module, "io") == 0) {
                /* POP itself is defined inside vm_run_slice (needs its DISPATCH-loop-local
                   `vm` binding for the overflow/underflow checks the macro shares with PUSH) --
                   this early-drain has no early-return hazard PUSH's own version does, so
                   inlining its one-line body directly here is exactly as safe as calling the
                   macro would be. */
                for (int i = 0; i < arg_count; i++) {
                    if (vm->stack_top > 0)
                        --vm->stack_top;
                    else
                        error("Stack underflow");
                }
                error("io is disabled for this run (--no-io)"); /* never returns -- unwinds instead */
            }
            if (aer_host_is_module(module, (unsigned int)strlen(module)))
                handled = aer_host_call(vm, module, fn, arg_count);
            else
                handled = aer_module_call(vm, module, fn, arg_count);
            break;
        }
    }
    if (!handled)
        error("'%s' has no function '%s'", aer_as_string(c->pool[module_idx])->data,
              aer_as_string(c->pool[fn_idx])->data);
    return handled;
}

/* Measurement-only: shifts every following function's address, so a benchmark can be run across
   several deliberately different code layouts instead of the single one a build happens to produce.
   Interpreter cycle counts swing several percent purely on how vm_run_slice's ~153 dispatch sites
   alias in the branch-target buffer (5.44), which is not attributable to any source change -- and
   comparing one layout against one layout silently folds that in. Never defined by a normal build. */
