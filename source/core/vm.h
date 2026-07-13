#ifndef VM_H
#define VM_H

#include "hashmap.h"
#include "value.h"
#include "dictmap.h"
#include "value_box.h"

/* Defined here (after dictmap.h) using DictMap not HashMap: dict values are stored inline with
   their key. */
struct AerDict {
    DictMap map;
};

typedef enum {
    /* Binary arithmetic */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_FLOOR_DIV,

    /* Binary comparison */
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_IN,              /* key in dict → key existence; value in array → element scan */

    /* Binary logical — never actually dispatched (parse_binary_ops intercepts `and`/`or`
       before emitting a real OP_BINARY, routing them to short-circuiting jumps instead, since
       they can't evaluate both sides unconditionally the way every other binary op does); kept as
       enum values purely because parser.c's operator-token lookup table still tags `and`/`or`
       tokens with these, and vm_binary() itself still has a (dead but harmless) branch for them. */
    OP_AND, OP_OR,
    OP_PIPE,            /* never dispatched either — `x |> f(args)` desugars to a call at parse time; kept as a lookup-table tag only, same reason as OP_AND/OP_OR */

    /* Binary bitwise */
    OP_BITWISE_AND, OP_BITWISE_OR, OP_BITWISE_XOR, OP_LSHIFT, OP_RSHIFT,

    /* Unary */
    OP_NEGATE, OP_NOT, OP_BITWISE_NOT,

    /* Control flow. Genuinely dispatched, unlike the family above — OP_JUMP's handler (lbl_jump)
       only ever touches vm->ip, so every register-based opcode below reuses it as-is for
       unconditional jumps instead of having its own register-specific version. */
    OP_JUMP,            /* operand: absolute code index */

    /* Structs — instances are TYPE_ARRAY with a non-NULL AerArray.shape. OP_DEFINE_STRUCT's own
       handler (lbl_define_struct) only ever reads operands and writes to chunk->shapes[], never
       any register/GC-sensitive state, so it's reused as-is for struct *definitions*
       (instantiation/field access are separate, register-based opcodes below). */
    OP_DEFINE_STRUCT,    /* operands: name pool idx, field count, then that many (field-name, default-value) pool-idx pairs — registers a Shape in the chunk's shape table */

    /* Misc */
    OP_TO_STR,  /* used as OP_UNARY's unary_op tag (string interpolation's value-to-string step) — see vm_to_str() */
    OP_HALT,

    /* ---- register-VM opcodes. See the register-based bytecode plan for the full design;
       PACK3's own comment below explains the packed-instruction encoding these use. */
    OP_LOADK,  /* operands: dest_reg, pool_idx — registers[dest_reg] = chunk pool constant */
    OP_MOVE,   /* operands: dest_reg, src_reg — registers[dest_reg] = registers[src_reg] */
    /* RK-encoded operand: a register index, or (with bit 30 set) a constant-pool index — see
       vm_rk_value (vm.c). One opcode per operator's runtime `bin_op`, not a family of opcodes
       per operand-kind combination — that's the entire point of RK encoding. */
    OP_BINARY, /* operands: dest_reg, rk_b, bin_op, rk_c — registers[dest_reg] = rk_b OP rk_c */

    /* Control flow. These two ARE their own opcodes (rather than reusing something else) because
       their condition is read directly out of a register (or an RK-encoded constant), never
       popped off any stack. */
    OP_JUMP_IF_FALSE_REG, /* operands: reg, target — jump to target if registers[reg] is falsy */
    /* RK-encoded fused comparison + branch — a loop/if condition is virtually always a bare
       comparison, so fusing it with the branch avoids a separate "compute bool, then branch on
       it" step. */
    OP_CMP_JUMP_FALSE,    /* operands: rk_a, cmp_op, rk_b, target */

    /* Function calls — real per-call register windowing (vm.h's CallFrame/VM.call_stack):
       every call gets its own isolated register bank, so nested/recursive calls can't clobber
       each other. */
    OP_CALL,   /* operands: dest_reg, callee_offset, arg_reg_base, arg_count — bulk-copies
                     registers[arg_reg_base..+arg_count) (caller's frame) into the new callee
                     frame's own registers starting at 0, in one dispatch, saves the return
                     address + dest_reg in the callee's frame, jumps to callee_offset */
    OP_RETURN, /* operand: src_reg (0-based, in the callee's own frame) — writes
                     registers[src_reg] to the caller's saved dest_reg, pops the callee's frame,
                     jumps back */

    /* Functions as values (bare-name case: `f = square; f(5)`, and calling through a container
       index/key — see parse_postfix_chain, parser.c). Same bulk-copy/frame-push mechanism
       as OP_CALL, but callee_reg is read at DISPATCH time (a register holding a runtime
       TYPE_FUNCTION AerVal) instead of callee_offset being a compile-time constant. Also fills
       any omitted trailing arguments from the AerFunction's own defaults, mirroring
       setup_call's (vm.c) default-filling. */
    OP_CALL_VALUE, /* operands: dest_reg, arg_reg_base, arg_count, callee_reg — arity-checks
                          against aer_as_function(registers[callee_reg]), bulk-copies
                          registers[arg_reg_base..+arg_count) into the new callee frame same as
                          OP_CALL, fills [arg_count, arity) from the function's own defaults[],
                          jumps to its code_offset */

    /* Tail-call optimization: when `return f(args)` is the WHOLE return expression (nothing wraps
       the call — checked at compile time, parser.c's own last_bare_call_end), the call's own
       already-emitted OP_CALL/OP_CALL_VALUE descriptor word is patched in place to this
       opcode instead (same operand shape — only the low byte, the opcode itself, changes;
       PACK's encoding keeps A/B/C in separate bits, so this is a blind byte-level OR/mask, no
       re-encoding needed).
         Handled by literally the SAME dispatch label as OP_CALL (see the dispatch table, vm.c)
       — cur_op distinguishes them once the callee is confirmed valid: with the CURRENT frame's
       own defer_count == 0 (a pending defer must run before this frame's storage is reused for
       someone else's locals, and there's no return value yet to hand back mid-call), the current
       call_stack[call_depth]'s OWN registers[0..arg_count) are overwritten with the new args
       (always safe: a call's argument registers are always temps, hence numerically above every
       permanent parameter/local register a would-be overlapping destination slot could be) and
       vm->ip jumps straight to callee_offset — call_depth, dest_reg, and return_ip are left
       completely untouched, so whatever the ORIGINAL (pre-recursion) caller expected back still
       arrives in the right place once the chain of tail calls finally returns. If defer_count > 0
       (or this is a genuine, non-tail OP_CALL), falls through to the ordinary push-a-new-frame
       path unchanged. */
    OP_TAIL_CALL,        /* same operands as OP_CALL; dest_reg is unused (ignored) here */
    OP_TAIL_CALL_VALUE,  /* same operands as OP_CALL_VALUE; dest_reg is unused (ignored) here */

    /* A call to a name that, at the point it was compiled, wasn't yet a variable, a known
       function, a struct, or a builtin (parse_call's forward-reference/pending-call path,
       parser.c) — but by the end of THIS parse() call turned out to be an ordinary top-level
       (global) VARIABLE, later holding a function value (`function call_greet(n): return
       greet(n)` compiled BEFORE `greet = greet_v1` ever runs). The pending-call mechanism can only
       patch a bytecode OFFSET (patch_jump) once a matching NAME is registered as a FUNCTION —
       it never was here, since `greet` is never declared `function greet(...)`, only ever assigned
       a function value. Fixed by retargeting, not by extending that mechanism: at drain time
       (parse's own end-of-call check), if the still-unresolved name turns out to be a known
       global after all, the call's ALREADY-EMITTED OP_CALL/OP_TAIL_CALL word is patched (same
       byte-level opcode swap OP_TAIL_CALL's own patch uses) to this opcode instead, and its wide
       word — originally a placeholder callee_offset — becomes a global register index instead.
       This fits in exactly the same 2-word shape a plain OP_CALL already reserved, so no
       bytecode needs to shift and no other jump target needs re-patching.
         Same runtime shape as OP_CALL_VALUE otherwise (arity check, receiver check,
       default-filling, frame push) — just resolving `fv` from call_stack[0].registers[global_reg]
       (mirroring OP_LOAD_GLOBAL's own read) instead of a register in the CURRENT frame, so a
       later reassignment of the global (`greet = greet_v2`) is picked up immediately on the very
       next call through this same call site — it's read fresh every dispatch, never cached. */
    OP_CALL_GLOBAL_VALUE,      /* operands: dest_reg, arg_reg_base, arg_count, global_reg */
    OP_TAIL_CALL_GLOBAL_VALUE, /* same operands; dest_reg is unused (ignored) here */

    /* Module/stdlib calls (module.function(args)). Bridges into shared stdlib infrastructure
       (aer_math_call() and friends, vm.c) rather than reimplementing every stdlib function for a
       register calling convention: pushes arg_count values from the registers onto vm->stack,
       calls the same aer_*_call() (pops arg_count args, pushes exactly one result on success),
       then pops that single result back into dest_reg. Also the bridge for a FILE-BASED import's
       function (aer_module_call, aer_module.c) — that path runs the imported file's OWN call
       chain (VM.call_stack), not this one. Stack-neutral from the caller's perspective —
       vm->stack_top ends exactly where it started. */
    OP_CALL_MODULE, /* operands: dest_reg, module_pool_idx, fn_pool_idx, arg_reg_base, arg_count */

    /* Global builtins (length/delete/append/print/type/assert/panic) called bare, e.g.
       `length(arr)`. Bridges to vm_call_builtin() (vm.c), which takes a plain AerVal* array, so
       unlike OP_CALL_MODULE this needs no push/pop bridge to vm->stack at all — registers are
       copied into a small local array and passed straight through. Struct construction
       (vm_call_builtin's other fallback branch, via chunk_find_shape) is never reached here —
       struct construction is already resolved at compile time (is_struct_name/OP_STRUCT_NEW), so
       only the seven builtin names above are ever checked at parse time (is_builtin_name,
       parser.c) before this opcode is emitted. */
    OP_CALL_BUILTIN, /* operands: dest_reg, name_pool_idx, arg_reg_base, arg_count */

    /* Reading a top-level ("global") variable from inside a function body. Per-call register
       windowing means a function's own registers are never the top-level frame's registers, so
       this always reads call_stack[0] specifically (vm.c), regardless of which frame is
       currently executing, since the top-level frame is never popped mid-run. */
    OP_LOAD_GLOBAL, /* operands: dest_reg, global_reg — registers[dest_reg] =
                          call_stack[0].registers[global_reg] */

    /* Write counterpart to OP_LOAD_GLOBAL, needed for `name OP= expr` inside a function when
       `name` isn't a local of the CURRENT function but IS an existing top-level variable: a plain
       `name = expr` is always local in this language (a first assignment always DEFINES a fresh
       local, full stop) — but `name += expr` falls back to a global read-modify-write via this
       opcode when the name isn't a local (parse_assignment's compound-assignment branch,
       parser.c). */
    OP_STORE_GLOBAL, /* operands: global_reg, rk_val — call_stack[0].registers[global_reg] =
                            vm_rk_value(rk_val) */

    /* `defer name(args)`. callee_offset is resolved at COMPILE time, consistent with every call
       target being resolved at compile time. Snapshots arg_count values out of the CURRENT
       frame's registers into that frame's own deferred-call list (CallFrame.defers, vm.c)
       immediately; lbl_return drains this list LIFO before the frame actually unwinds. */
    OP_DEFER_PUSH, /* operands: callee_offset, arg_reg_base, arg_count */

    /* Registers can hold heap-allocated values (arrays/dicts/strings/functions), which is why
       mark_vm_roots (vm.c) scans all of registers[] for every live frame. */
    OP_ARRAY_NEW,  /* operands: dest_reg, item_reg_base, item_count — bulk-copies
                          registers[item_reg_base..+item_count) into a freshly allocated
                          AerArray's items buffer in one dispatch, registers[dest_reg] = the new array */
    OP_INDEX_GET,  /* operands: dest_reg, arr_reg, rk_idx — registers[dest_reg] =
                          registers[arr_reg][rk_idx], via the existing, type-generic
                          vm_index_get_compute() (vm.c) — arr_reg is always a register (a
                          collection can't be a pool constant); rk_idx is RK-encoded like
                          OP_BINARY's operands */
    OP_INDEX_SET,  /* operands: arr_reg, rk_idx, rk_val — registers[arr_reg][rk_idx] =
                          rk_val, via the existing vm_index_set_compute() (vm.c), which also
                          runs the GC write barrier */

    /* `arr[a:b]`/`arr[a:]`/`arr[:b]`/`arr[:]` (array or string; a struct-shaped array can't be
       sliced). Reuses the shared vm_slice_bounds() helper — no logic duplicated, just reading the
       collection/bounds from registers and writing the result to one instead of stack pop/push. A
       missing start or end compiles to an RK-encoded null constant, so this opcode itself never
       needs to know which bound was actually written by the user. */
    OP_SLICE_GET,  /* operands: dest_reg, arr_reg, rk_start, rk_end — registers[dest_reg] =
                          the sub-range of registers[arr_reg] from rk_start to rk_end */

    /* `x as Point` where Point is a known STRUCT type (not integer/float/boolean/string,
       OP_CAST's own scope) — errors unless src_reg holds exactly that struct type, else passes
       the value through unchanged (never converts). Also used, at compile time, by
       parse_function/parse_call whenever a function has a struct-shape-checked first
       ("receiver") parameter: see AerFunction.has_receiver/receiver_type's own check inside
       lbl_call_value, which performs the equivalent test at call setup instead of via this
       opcode (a receiver check happens once per call, not as a separate expression a user wrote). */
    OP_CHECK_SHAPE, /* operands: dest_reg, src_reg, type_name_pool_idx — errors unless
                           registers[src_reg] is an instance of that struct type, else
                           registers[dest_reg] = registers[src_reg] unchanged */

    /* Same bulk-copy-from-registers mechanism as OP_ARRAY_NEW, applied to key/value pairs
       instead of a flat item list; get/set already work on dicts for free through
       OP_INDEX_GET/SET's shared vm_index_get_compute()/vm_index_set_compute() calls. Mirrors
       the dict-construction key-must-be-string validation and owned-copy-of-the-key discipline
       exactly, just reading pairs from registers instead of popping them off a stack in reverse. */
    OP_DICT_NEW,   /* operands: dest_reg, pair_reg_base, pair_count — reads pair_count
                          (key,val) register pairs starting at pair_reg_base (key at
                          pair_reg_base + 2*i, val at + 2*i + 1), builds a fresh AerDict,
                          registers[dest_reg] = the new dict */

    /* Array/dict/string iteration, single-loop-variable form. col_reg/idx_reg are just two more
       registers the caller already owns — no exit-time cleanup needed, and `break` out of a
       for-loop is just a plain jump. item_dest_reg IS the loop variable's register directly. */
    OP_ITER_NEXT_ARRAY, /* operands: col_reg, idx_reg, item_dest_reg, end_target — if
                              registers[idx_reg] >= array(col_reg).count: jump to end_target
                              (loop exit, item_dest_reg untouched); else registers[item_dest_reg]
                              = array.items[idx], registers[idx_reg] += 1, fall through.
                              Despite the name, also accepts a dict (single-variable `for k in
                              dict:` yields keys) or a string (yields one-character strings),
                              mirroring the identical dict/string branches vm_index_get_compute's
                              own dispatch shape uses — kept as one opcode/one name rather than a
                              separate opcode per type, since all three cases differ only in how
                              idx_reg maps to "the next item" (array slot, hashmap bucket scan, or
                              string byte offset). */

    /* `for k, v in dict:`, the two-loop-variable form. Genuinely dict-only (unlike
       OP_ITER_NEXT_ARRAY's single-var form, which accepts either type) since there is no
       second value to bind a loop variable to when iterating an array or string. key_dest_reg
       packs alongside the opcode; val_dest_reg needs its own word (only 3 narrow fields fit
       alongside an opcode in one packed word). */
    OP_ITER_NEXT_PAIR, /* operands: col_reg, idx_reg, key_dest_reg, val_dest_reg, end_target —
                              if the dict's buckets starting at idx_reg are exhausted: jump to
                              end_target (loop exit, dest regs untouched); else
                              registers[key_dest_reg] = the next live bucket's key (a fresh
                              owned string, never an alias), registers[val_dest_reg] = that
                              bucket's value, registers[idx_reg] = the bucket index past it,
                              fall through */

    /* Integer-range iteration (`for i in a..b..step:`) — direction inferred from cur vs end, not
       step's sign; step must be positive; >=/<= exit check so a step that doesn't evenly divide
       the range still stops cleanly. cur_reg MUST be a register the loop owns exclusively (never
       an aliased existing variable's register — see parse_for_in's use of arg_materialize,
       not materialize, to guarantee this), since this opcode mutates it every iteration;
       end_reg/step_reg are read-only and may safely alias an existing variable's register. */
    OP_ITER_RANGE, /* operands: cur_reg, end_reg, step_reg, item_dest_reg, end_target — if the
                          range is exhausted: jump to end_target (item_dest_reg untouched); else
                          registers[item_dest_reg] = registers[cur_reg], registers[cur_reg]
                          advances by +-step, fall through */

    /* Structs. OP_DEFINE_STRUCT itself (above) is reused unmodified for struct *definitions*;
       these three ARE their own opcodes because instantiation, field get, and field set all need
       register operands. */
    OP_STRUCT_NEW, /* operands: dest_reg, type_name_pool_idx, arg_reg_base, arg_count — mirrors
                          struct instantiation (vm.c): chunk_find_shape() by name, arity check
                          (arg_count <= field_count), one struct_pool allocation (items inline
                          right after the header), bulk-copies registers[arg_reg_base..+arg_count)
                          into the leading fields, vm_default_value()-fills any trailing omitted
                          fields */
    OP_FIELD_GET,  /* operands: dest_reg, struct_reg, field_name_pool_idx — pool-index
                          field-name scan (vm.c), registers[dest_reg] = the matching field */
    OP_FIELD_SET,  /* operands: struct_reg, field_name_pool_idx, rk_val — includes the
                          gc_barrier_array call */

    /* Expression-grammar completions. unary_op reuses OP_NEGATE/OP_NOT/OP_BITWISE_NOT as its
       operand tag, same convention OP_BINARY already uses for bin_op — one opcode per operator
       FAMILY, not one opcode per operator. and/or need no opcode at all: they compile to existing
       OP_JUMP_IF_FALSE_REG/OP_JUMP/OP_LOADK. */
    OP_UNARY, /* operands: dest_reg, unary_op, rk_operand — registers[dest_reg] =
                    unary_op(rk_operand); also folds in OP_TO_STR (string interpolation's
                    value-to-string step), calling the existing vm_to_str() helper — same
                    one-opcode-per-family shape. */

    /* `x as integer/float/boolean`. Separate from OP_UNARY since OP_CAST's own operand
       (CAST_INTEGER/FLOAT/BOOLEAN, below) isn't an Opcode value the way unary_op/bin_op are, so it
       doesn't fit that family's tag convention — but the actual conversion logic is shared via the
       extracted vm_cast() helper (vm.c), not duplicated. `x as string` still goes through
       OP_UNARY's OP_TO_STR case; `x as SomeStructType` is OP_CHECK_SHAPE, above. */
    OP_CAST, /* operands: dest_reg, cast_type, rk_operand — registers[dest_reg] =
                   vm_cast(rk_operand, cast_type) */

    /* Fusion, found via a real per-opcode dispatch audit on nbody.aer: `x OP y.field` (e.g. this
       exact benchmark's `dx = bix - bj.x`) always compiled as OP_FIELD_GET (into a fresh temp)
       immediately followed by OP_BINARY reading that temp — two dispatches for something that's
       structurally one operation. Recognized at emit time in parse_binary_ops (parser.c) by
       truncating the just-emitted OP_FIELD_GET and re-encoding it as this opcode's last two
       operands. */
    OP_BINARY_FIELD, /* operands: dest_reg, rk_lhs, bin_op, struct_reg, field_name_pool_idx —
                            registers[dest_reg] = rk_lhs OP struct_reg.field */

    /* Mirror of OP_BINARY_FIELD for the other operand order — `y.field OP x` (e.g. this exact
       benchmark's `mj = bj.mass * mag`), field on the LEFT. No operand swap/commutativity
       reasoning needed: this opcode encodes the field as the LEFT operand directly, so it's
       correct for every operator, commutative or not. Recognized the same way, by checking
       whether the LHS the caller already parsed was itself a bare OP_FIELD_GET. */
    OP_FIELD_BINARY, /* operands: dest_reg, struct_reg, field_name_pool_idx, bin_op, rk_rhs —
                            registers[dest_reg] = struct_reg.field OP rk_rhs */

    /* REPL/shell-mode support. A bare call/module-call/pipe-chain statement's result, when
       mode == MODE_SHELL, is printed (unless null) instead of just sitting unread in a register. */
    OP_PRINT_REPL, /* operand: src_reg — prints registers[src_reg] unless it's TYPE_NULL */
} Opcode;

/* RK encoding for most opcodes' operands — see vm_rk_value (vm.c). Pool indices are always
   small non-negative ints in practice, nowhere near this bit, so reusing it as a "this is a
   constant, not a register" flag is safe and simple (matches Lua's own BITRK convention). */
#define RK_CONST_FLAG (1 << 30)

/* Per-call register bank size (vm.h's CallFrame) — also visible here since parser.c's
   variable table needs to know the ceiling on distinct variable names in one frame. 128
   comfortably covers every real test file found so far with room to grow, and stays well under
   the packed instruction encoding's 8-bit/255-per-field ceiling (PACK1/2/3 below) — a register
   index is still one packed byte, this only widens how many distinct values that byte can name.
   A program needing more than 128 would still need a spill mechanism this prototype doesn't have. */
#define FRAME_REGISTERS 128

/* Packed-instruction descriptor word — every operand used to get its own full 32-bit word
   regardless of how small its value actually was (a register index only ever needs a handful of
   bits), each one costing its own separate array-index-and-increment fetch. Every OP_* opcode's
   NARROW fields — register
   indices and small enums (bin_op/unary_op/cast_type) — now pack into one shared 32-bit
   descriptor word alongside the opcode itself: [C:8][B:8][A:8][opcode:8], opcode in the low byte
   so extracting it is a single mask, no shift. WIDE fields — RK-encoded operands
   (register-or-constant, RK_CONST_FLAG unchanged), bare pool indices, and jump targets — keep
   their own dedicated word exactly as before this change; nothing about how those are read
   (READ(), vm_rk_value()) is any different. Jump targets specifically are NEVER packed
   alongside anything else, on purpose — patch_jump (parser.c) does a blind word overwrite
   at dozens of call sites, and keeping every patchable field in its own dedicated word is what
   lets that stay a blind overwrite instead of needing read-modify-write.
   Unused fields are passed as 0 by convention (PACK1/2 wrap PACK3 for opcodes with fewer
   than 3 narrow fields, purely for readability at the call site — the encoding is identical). */
#define PACK3(op, a, b, cc) \
    (((int)(op) & 0xFF) | (((a) & 0xFF) << 8) | (((b) & 0xFF) << 16) | (((cc) & 0xFF) << 24))
#define PACK2(op, a, b)   PACK3(op, a, b, 0)
#define PACK1(op, a)      PACK3(op, a, 0, 0)
#define UNPACK_A(word) (((word) >> 8)  & 0xFF)
#define UNPACK_B(word) (((word) >> 16) & 0xFF)
#define UNPACK_C(word) (((word) >> 24) & 0xFF)

/* OP_BINARY alone gets a dedicated single-64-bit-word encoding: it's the single hottest opcode in
   arithmetic-heavy code (over a third of all dispatches on nbody.aer), and the ordinary PACK3/RK
   scheme above still costs it 3 separate code-array fetches per dispatch (op_word, rk_b, rk_c) —
   one DISPATCH() fetch of op_word, then two more READ()s just to find out what to compute. Once
   code[] is a 64-bit-word array (see Chunk's own comment above), opcode(8) + dest(8) + bin_op(8) +
   two 20-bit RK operands (52 bits) fits in one word with room to spare, cutting OP_BINARY to the
   ONE fetch DISPATCH() already does for every opcode, unconditionally.
     This compact RK operand needs its own (narrower) flag/index split — RK20_CONST_FLAG at bit 19
   rather than RK_CONST_FLAG's bit 30 — since 20 bits total has to hold both the flag and the
   index. 19 index bits (524288 slots) is still enormous headroom over both FRAME_REGISTERS (128)
   and any realistic constant-pool size, but genuinely large generated programs could in principle
   exceed it — see emit_binary's own guard (parser.c), which reports a clean compile error rather
   than silently truncating a resolved rk_lhs/rk_rhs from parse_binary_ops's ordinary (wider)
   RK_CONST_FLAG scheme down into this one. */
#define RK20_CONST_FLAG (1ULL << 19)
#define RK20_INDEX_MASK 0x7FFFFULL
#define RK20_MAX_INDEX  0x7FFFF
#define PACK_RK20(rk) \
    (((rk) & RK_CONST_FLAG) \
        ? (RK20_CONST_FLAG | ((unsigned long long)((rk) & ~RK_CONST_FLAG) & RK20_INDEX_MASK)) \
        : ((unsigned long long)(rk) & RK20_INDEX_MASK))
#define PACK_BINARY(dest, bin_op, rk_b, rk_c) \
    ( ((unsigned long long)(OP_BINARY) & 0xFF) \
    | (((unsigned long long)(dest)   & 0xFF) << 8) \
    | (((unsigned long long)(bin_op) & 0xFF) << 16) \
    | ((PACK_RK20(rk_b) & 0xFFFFFULL) << 24) \
    | ((PACK_RK20(rk_c) & 0xFFFFFULL) << 44) )
#define UNPACK_RK_B20(word) (((word) >> 24) & 0xFFFFFULL)
#define UNPACK_RK_C20(word) (((word) >> 44) & 0xFFFFFULL)

/* Slice A of the same single-word treatment, extended to the next-hottest opcodes. A register
   field only needs 7 bits here (not RK's 8) — exactly FRAME_REGISTERS, no headroom wasted —
   since a plain register index (unlike an RK operand) never needs a flag bit and is provably
   always < FRAME_REGISTERS (reg_alloc/reg_reserve already refuse to hand out more). This is what
   makes 4 registers fit alongside an opcode with bits to spare (PACK_REG4 below).
     Patchable jump targets are deliberately EXCLUDED from every packing here, same rule OP_JUMP's
   family already followed before this change (see PACK3's own comment above): patch_jump does a
   blind word overwrite at the target's own offset, so any field sharing that word would be
   clobbered. OP_ITER_RANGE/OP_ITER_NEXT_PAIR's target word is untouched by PACK_REG4 — only their
   OTHER (non-target) fields get packed together. */
#define PACK_REG4(op, a, b, cc, d) \
    ( ((unsigned long long)(op) & 0xFF) \
    | (((unsigned long long)(a)  & 0x7F) << 8) \
    | (((unsigned long long)(b)  & 0x7F) << 15) \
    | (((unsigned long long)(cc) & 0x7F) << 22) \
    | (((unsigned long long)(d)  & 0x7F) << 29) )
#define UNPACK_REG4_A(word) (((word) >> 8)  & 0x7F)
#define UNPACK_REG4_B(word) (((word) >> 15) & 0x7F)
#define UNPACK_REG4_C(word) (((word) >> 22) & 0x7F)
#define UNPACK_REG4_D(word) (((word) >> 29) & 0x7F)

/* OP_FIELD_GET: dest/struct_reg (7 bits each) + field_idx (a bare pool index, not RK — always a
   constant name, never a register) get the whole remaining 42 bits, comfortably more than any
   real pool will ever hold, so no overflow guard is needed here the way OP_BINARY's RK20 needs
   one (a 30-bit-plus pool would already have failed elsewhere first). */
#define PACK_FIELD_GET(dest, struct_reg, field_idx) \
    ( ((unsigned long long)(OP_FIELD_GET)  & 0xFF) \
    | (((unsigned long long)(dest)         & 0x7F) << 8) \
    | (((unsigned long long)(struct_reg)   & 0x7F) << 15) \
    | (((unsigned long long)(field_idx)    & 0x3FFFFFFFFFFULL) << 22) )
#define UNPACK_FIELD_GET_DEST(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_FIELD_GET_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_FIELD_GET_FIELD(word)  (((word) >> 22) & 0x3FFFFFFFFFFULL)

/* OP_FIELD_SET: struct_reg(7) + field_idx(29, same "always a name, always small" reasoning as
   OP_FIELD_GET) + rk_val (RK20:20) = 64 bits exactly. */
#define PACK_FIELD_SET(struct_reg, field_idx, rk_val) \
    ( ((unsigned long long)(OP_FIELD_SET) & 0xFF) \
    | (((unsigned long long)(struct_reg)  & 0x7F) << 8) \
    | (((unsigned long long)(field_idx)   & 0x1FFFFFFFULL) << 15) \
    | ((PACK_RK20(rk_val) & 0xFFFFFULL) << 44) )
#define UNPACK_FIELD_SET_STRUCT(word) (((word) >> 8)  & 0x7F)
#define UNPACK_FIELD_SET_FIELD(word)  (((word) >> 15) & 0x1FFFFFFFULL)
#define UNPACK_FIELD_SET_RK(word)     (((word) >> 44) & 0xFFFFFULL)

/* OP_INDEX_GET: dest/arr_reg (7 bits each) + rk_idx (RK20:20). */
#define PACK_INDEX_GET(dest, arr_reg, rk_idx) \
    ( ((unsigned long long)(OP_INDEX_GET) & 0xFF) \
    | (((unsigned long long)(dest)    & 0x7F) << 8) \
    | (((unsigned long long)(arr_reg) & 0x7F) << 15) \
    | ((PACK_RK20(rk_idx) & 0xFFFFFULL) << 22) )
#define UNPACK_INDEX_GET_DEST(word) (((word) >> 8)  & 0x7F)
#define UNPACK_INDEX_GET_ARR(word)  (((word) >> 15) & 0x7F)
#define UNPACK_INDEX_GET_RK(word)   (((word) >> 22) & 0xFFFFFULL)

/* Slice C: OP_STORE_GLOBAL/OP_UNARY/OP_CAST/OP_CHECK_SHAPE/OP_STRUCT_NEW — none of these have a
   patchable target (unlike OP_JUMP_IF_FALSE_REG/OP_DEFER_PUSH, which stay 2 words: see
   emit_jump_if_false_reg/emit_defer_push's own comments — a `defer` callee_offset can be a
   forward-reference placeholder exactly like OP_CALL's, so it needs the same dedicated word), so
   all their fields safely fold into one word. */
#define PACK_STORE_GLOBAL(global_reg, rk_val) \
    ( ((unsigned long long)(OP_STORE_GLOBAL) & 0xFF) \
    | (((unsigned long long)(global_reg) & 0x7F) << 8) \
    | ((PACK_RK20(rk_val) & 0xFFFFFULL) << 15) )
#define UNPACK_STORE_GLOBAL_REG(word) (((word) >> 8)  & 0x7F)
#define UNPACK_STORE_GLOBAL_RK(word)  (((word) >> 15) & 0xFFFFFULL)

/* Shared by OP_UNARY and OP_CAST — dest/reg (7) + op-or-cast-type tag (8, same width as
   OP_BINARY's bin_op tag) + rk (RK20:20). */
#define PACK_UNARY(dest, unary_op, rk) \
    ( ((unsigned long long)(OP_UNARY) & 0xFF) \
    | (((unsigned long long)(dest)     & 0x7F) << 8) \
    | (((unsigned long long)(unary_op) & 0xFF) << 15) \
    | ((PACK_RK20(rk) & 0xFFFFFULL) << 23) )
#define UNPACK_UNARY_DEST(word) (((word) >> 8)  & 0x7F)
#define UNPACK_UNARY_OP(word)   (((word) >> 15) & 0xFF)
#define UNPACK_UNARY_RK(word)   (((word) >> 23) & 0xFFFFFULL)

#define PACK_CAST(dest, cast_type, rk) \
    ( ((unsigned long long)(OP_CAST) & 0xFF) \
    | (((unsigned long long)(dest)      & 0x7F) << 8) \
    | (((unsigned long long)(cast_type) & 0xFF) << 15) \
    | ((PACK_RK20(rk) & 0xFFFFFULL) << 23) )
#define UNPACK_CAST_DEST(word) (((word) >> 8)  & 0x7F)
#define UNPACK_CAST_TYPE(word) (((word) >> 15) & 0xFF)
#define UNPACK_CAST_RK(word)   (((word) >> 23) & 0xFFFFFULL)

/* dest/lhs_reg (7 bits each) + type_name_idx — a bare pool index, not RK (always a compile-time
   struct type name, never a register) — gets the remaining 42 bits, same reasoning as
   OP_FIELD_GET's field_idx. */
#define PACK_CHECK_SHAPE(dest, lhs_reg, type_name_idx) \
    ( ((unsigned long long)(OP_CHECK_SHAPE) & 0xFF) \
    | (((unsigned long long)(dest)          & 0x7F) << 8) \
    | (((unsigned long long)(lhs_reg)       & 0x7F) << 15) \
    | (((unsigned long long)(type_name_idx) & 0x3FFFFFFFFFFULL) << 22) )
#define UNPACK_CHECK_SHAPE_DEST(word) (((word) >> 8)  & 0x7F)
#define UNPACK_CHECK_SHAPE_LHS(word)  (((word) >> 15) & 0x7F)
#define UNPACK_CHECK_SHAPE_NAME(word) (((word) >> 22) & 0x3FFFFFFFFFFULL)

/* dest/arg_reg_base/arg_count (7 bits each) + type_name_pool_idx, gets the remaining 35 bits —
   also a bare compile-time-only pool index, never patched (struct construction always resolves at
   parse time, never a forward-reference placeholder the way a function call can be). */
#define PACK_STRUCT_NEW(dest, arg_reg_base, arg_count, type_name_idx) \
    ( ((unsigned long long)(OP_STRUCT_NEW) & 0xFF) \
    | (((unsigned long long)(dest)          & 0x7F) << 8) \
    | (((unsigned long long)(arg_reg_base)  & 0x7F) << 15) \
    | (((unsigned long long)(arg_count)     & 0x7F) << 22) \
    | (((unsigned long long)(type_name_idx) & 0x7FFFFFFFFULL) << 29) )
#define UNPACK_STRUCT_NEW_DEST(word)     (((word) >> 8)  & 0x7F)
#define UNPACK_STRUCT_NEW_ARG_BASE(word) (((word) >> 15) & 0x7F)
#define UNPACK_STRUCT_NEW_ARG_COUNT(word) (((word) >> 22) & 0x7F)
#define UNPACK_STRUCT_NEW_NAME(word)     (((word) >> 29) & 0x7FFFFFFFFULL)

/* Slice E — the last, tightest-budget batch: OP_CMP_JUMP_FALSE/OP_INDEX_SET/OP_SLICE_GET have no
   patchable target issue (OP_CMP_JUMP_FALSE's target still gets its own dedicated word, same rule
   as everywhere else) but need two RK20 operands in one word; OP_CALL_MODULE/OP_CALL_BUILTIN have
   no patchable target at all (module/function/builtin names are always literal identifiers
   resolved at parse time, never a forward-reference placeholder); OP_FIELD_BINARY/OP_BINARY_FIELD
   are the tightest of all — dest+struct_reg+bin_op+one RK operand already use 42 bits, leaving
   only 14 for field_idx (16384 slots) instead of the 29-42 bits every other opcode's name/field
   index got. Still comfortably more than any real program's field-name count specifically (as
   opposed to its total pool size, which these two don't need to address) — guarded the same way. */
#define PACK_CMP_JUMP_FALSE(cmp_op, rk_a, rk_b) \
    ( ((unsigned long long)(OP_CMP_JUMP_FALSE) & 0xFF) \
    | (((unsigned long long)(cmp_op) & 0xFF) << 8) \
    | ((PACK_RK20(rk_a) & 0xFFFFFULL) << 16) \
    | ((PACK_RK20(rk_b) & 0xFFFFFULL) << 36) )
#define UNPACK_CMP_JUMP_OP(word)   (((word) >> 8)  & 0xFF)
#define UNPACK_CMP_JUMP_RK_A(word) (((word) >> 16) & 0xFFFFFULL)
#define UNPACK_CMP_JUMP_RK_B(word) (((word) >> 36) & 0xFFFFFULL)

#define PACK_INDEX_SET(arr_reg, rk_idx, rk_val) \
    ( ((unsigned long long)(OP_INDEX_SET) & 0xFF) \
    | (((unsigned long long)(arr_reg) & 0x7F) << 8) \
    | ((PACK_RK20(rk_idx) & 0xFFFFFULL) << 15) \
    | ((PACK_RK20(rk_val) & 0xFFFFFULL) << 35) )
#define UNPACK_INDEX_SET_ARR(word) (((word) >> 8)  & 0x7F)
#define UNPACK_INDEX_SET_IDX(word) (((word) >> 15) & 0xFFFFFULL)
#define UNPACK_INDEX_SET_VAL(word) (((word) >> 35) & 0xFFFFFULL)

#define PACK_SLICE_GET(dest, arr_reg, rk_start, rk_end) \
    ( ((unsigned long long)(OP_SLICE_GET) & 0xFF) \
    | (((unsigned long long)(dest)    & 0x7F) << 8) \
    | (((unsigned long long)(arr_reg) & 0x7F) << 15) \
    | ((PACK_RK20(rk_start) & 0xFFFFFULL) << 22) \
    | ((PACK_RK20(rk_end)   & 0xFFFFFULL) << 42) )
#define UNPACK_SLICE_GET_DEST(word)  (((word) >> 8)  & 0x7F)
#define UNPACK_SLICE_GET_ARR(word)   (((word) >> 15) & 0x7F)
#define UNPACK_SLICE_GET_START(word) (((word) >> 22) & 0xFFFFFULL)
#define UNPACK_SLICE_GET_END(word)   (((word) >> 42) & 0xFFFFFULL)

#define CALL_MODULE_NAME_MASK  0x1FFFFULL
#define CALL_MODULE_NAME_MAX   0x1FFFF
#define PACK_CALL_MODULE(dest, arg_reg_base, arg_count, module_idx, fn_idx) \
    ( ((unsigned long long)(OP_CALL_MODULE) & 0xFF) \
    | (((unsigned long long)(dest)         & 0x7F) << 8) \
    | (((unsigned long long)(arg_reg_base) & 0x7F) << 15) \
    | (((unsigned long long)(arg_count)    & 0x7F) << 22) \
    | (((unsigned long long)(module_idx) & CALL_MODULE_NAME_MASK) << 29) \
    | (((unsigned long long)(fn_idx)     & CALL_MODULE_NAME_MASK) << 46) )
#define UNPACK_CALL_MODULE_DEST(word)     (((word) >> 8)  & 0x7F)
#define UNPACK_CALL_MODULE_ARG_BASE(word) (((word) >> 15) & 0x7F)
#define UNPACK_CALL_MODULE_ARG_COUNT(word) (((word) >> 22) & 0x7F)
#define UNPACK_CALL_MODULE_MODULE(word)  (((word) >> 29) & CALL_MODULE_NAME_MASK)
#define UNPACK_CALL_MODULE_FN(word)      (((word) >> 46) & CALL_MODULE_NAME_MASK)

#define PACK_CALL_BUILTIN(dest, arg_reg_base, arg_count, name_idx) \
    ( ((unsigned long long)(OP_CALL_BUILTIN) & 0xFF) \
    | (((unsigned long long)(dest)         & 0x7F) << 8) \
    | (((unsigned long long)(arg_reg_base) & 0x7F) << 15) \
    | (((unsigned long long)(arg_count)    & 0x7F) << 22) \
    | (((unsigned long long)(name_idx)     & 0x7FFFFFFFFULL) << 29) )
#define UNPACK_CALL_BUILTIN_DEST(word)     (((word) >> 8)  & 0x7F)
#define UNPACK_CALL_BUILTIN_ARG_BASE(word) (((word) >> 15) & 0x7F)
#define UNPACK_CALL_BUILTIN_ARG_COUNT(word) (((word) >> 22) & 0x7F)
#define UNPACK_CALL_BUILTIN_NAME(word)    (((word) >> 29) & 0x7FFFFFFFFULL)

#define FUSED_FIELD_NAME_MASK 0x3FFFULL
#define FUSED_FIELD_NAME_MAX  0x3FFF
#define PACK_FIELD_BINARY(dest, struct_reg, bin_op, field_idx, rk_rhs) \
    ( ((unsigned long long)(OP_FIELD_BINARY) & 0xFF) \
    | (((unsigned long long)(dest)       & 0x7F) << 8) \
    | (((unsigned long long)(struct_reg) & 0x7F) << 15) \
    | (((unsigned long long)(bin_op)     & 0xFF) << 22) \
    | (((unsigned long long)(field_idx) & FUSED_FIELD_NAME_MASK) << 30) \
    | ((PACK_RK20(rk_rhs) & 0xFFFFFULL) << 44) )
#define UNPACK_FIELD_BINARY_DEST(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_FIELD_BINARY_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_FIELD_BINARY_OP(word)     (((word) >> 22) & 0xFF)
#define UNPACK_FIELD_BINARY_NAME(word)   (((word) >> 30) & FUSED_FIELD_NAME_MASK)
#define UNPACK_FIELD_BINARY_RK(word)     (((word) >> 44) & 0xFFFFFULL)

#define PACK_BINARY_FIELD(dest, struct_reg, bin_op, rk_lhs, field_idx) \
    ( ((unsigned long long)(OP_BINARY_FIELD) & 0xFF) \
    | (((unsigned long long)(dest)       & 0x7F) << 8) \
    | (((unsigned long long)(struct_reg) & 0x7F) << 15) \
    | (((unsigned long long)(bin_op)     & 0xFF) << 22) \
    | ((PACK_RK20(rk_lhs) & 0xFFFFFULL) << 30) \
    | (((unsigned long long)(field_idx) & FUSED_FIELD_NAME_MASK) << 50) )
#define UNPACK_BINARY_FIELD_DEST(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_BINARY_FIELD_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_BINARY_FIELD_OP(word)     (((word) >> 22) & 0xFF)
#define UNPACK_BINARY_FIELD_RK(word)     (((word) >> 30) & 0xFFFFFULL)
#define UNPACK_BINARY_FIELD_NAME(word)   (((word) >> 50) & FUSED_FIELD_NAME_MASK)

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

/* A function's persistent, runtime-visible registration — appended to Chunk.functions by
   func_register (parser.c) at the same moment it updates its own parse-time-only lookup
   tables, so a function stays FINDABLE by name after compilation finishes, not just during it.
   Needed specifically for cross-module calls (aer_module.c): a file-based `import` compiles the
   imported file into its own Chunk via its own parse() call, whose parser-side tables are
   reset/reused for the NEXT compile the moment this one returns — they cannot answer "does this
   already-compiled Chunk export a function named X" later, when `module.fn(args)` is actually
   called. Mirrors Shape's own precedent (chunk->shapes[], appended by OP_DEFINE_STRUCT's handler,
   outlives the parser state that created it) for exactly the same reason. */
typedef struct {
    unsigned int name;           /* pool index of the function's name */
    unsigned int code_offset;
    unsigned int arity;
    unsigned int min_arity;
    AerVal*      defaults;       /* xmalloc'd array of (arity - min_arity) values, or NULL — owned, never freed (matches Shape's own no-cleanup precedent) */
    bool         has_receiver;
    unsigned int receiver_type;  /* meaningful only when has_receiver */
} ChunkFunction;

/* ------------------------------------------------------------------ */
/* Bytecode chunk                                                       */
/* code[] is a flat 64-bit-word array: each instruction is one opcode word, optionally followed by
   one or more operand words (or, for a packed OP_* instruction, further fields packed into that
   same word — see PACK3/PACK_BINARY). Widened from a 32-bit int array so OP_BINARY (the single
   hottest opcode — see PACK_BINARY's own comment) can pack its whole instruction, RK operands
   included, into ONE word instead of three. */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long long* code;
    unsigned int count, capacity;

    AerVal*      pool;           /* constants and variable names — all deduplicated by value */
    unsigned int pool_count, pool_cap;

    /* name -> pool index, for O(1) dedup of TYPE_STRING pool entries (chunk_add_pool, vm.c); owns an independent copy of each key. */
    HashMap      name_index;

    /* Struct type registry appended to by OP_DEFINE_STRUCT; redeclaring a struct appends rather than replaces so old Shape pointers stay valid, and chunk_find_shape() searches newest-first. */
    Shape**      shapes;
    unsigned int shape_count, shape_cap;

    /* Function registry — see ChunkFunction's own comment above. Appended by func_register
       (parser.c); searched newest-first by chunk_find_function, same convention as shapes. */
    ChunkFunction* functions;
    unsigned int   function_count, function_cap;

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

    /* One inline cache slot per OP_FIELD_GET/OP_FIELD_SET site, indexed by the offset the
       opcode itself starts at (same indexing scheme as debug_hits below) — remembers the last
       Shape* seen at that site plus the field's resolved index within that shape, so a
       monomorphic site (the overwhelming common case: the same struct type hitting the same
       `.field` expression on every iteration of a hot loop) skips straight to the slot instead of
       re-scanning shape->field_names[] every single access. A polymorphic site just keeps missing
       the cache (one wasted pointer compare) and falls back to the existing linear scan — never
       incorrect, only sometimes not-sped-up. Shape* is never reallocated once created (see struct
       Shape's own comment), so a cached pointer never goes stale; grown in lockstep with `code` by
       chunk_ensure_field_cache (vm.c), called once at the top of vm_run. NULL shape means "not
       cached yet". */
    Shape**      field_cache_shape;
    int*         field_cache_slot;
    unsigned int field_cache_cap;

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
#define VM_KEY_MAX      4096   /* max dict key length for stack-buffered lookups */

/* A `defer name(args)` statement. Unlike a plain call, the target is resolved at COMPILE time via
   func_lookup — consistent with every other call site's target being resolved at compile time.
   `args` are snapshotted at the defer statement — a deferred call's arguments are evaluated once,
   now, not re-evaluated at replay time. */
typedef struct {
    unsigned int callee_offset;
    AerVal       args[MAX_DEFER_ARGS];
    int          arg_count;
} DeferredCall;

/* Per-call register frame — every active call gets its own isolated FRAME_REGISTERS-sized
   bank, mirroring how a stack-based VM would give every call its own call-frame; nested/recursive
   calls can't clobber each other. Lives inside the VM struct (not a file-scope static) so a
   nested file-module VM (aer_module.c, a file-based `import`'s own separate VM/Chunk) gets its
   own isolated call chain instead of sharing — and thereby corrupting — the calling VM's own
   in-progress one. */
typedef struct {
    AerVal       registers[FRAME_REGISTERS];
    unsigned int return_ip;   /* where to resume in the CALLER */
    int          dest_reg;    /* which of the CALLER's registers gets the return value */

    /* Pending `defer` calls, drained LIFO by lbl_return before the frame unwinds. Each call gets
       its own isolated register bank, so a frame's real return value just sits in its own src_reg
       untouched while nested deferred calls run in deeper, separate frames — there is no shared
       mutable slot for a deferred call's own result to collide with, so no separate "draining"
       flag is needed. */
    DeferredCall* defers;
    int             defer_count;
} CallFrame;

typedef struct {
    Chunk*       chunk;
    unsigned int ip;

    /* Used only as a scratch argument-passing channel for calls that bridge out of the register
       calling convention: OP_CALL_MODULE's native-stdlib bridge (aer_math_call() and friends)
       and aer_module_call()'s cross-file-module argument hand-off. Never touched by any
       register-native opcode itself. */
    AerVal       stack[VM_STACK_MAX];
    int          stack_top;

    /* registers always points at call_stack[call_depth].registers — repointed on every
       call/return. */
    CallFrame  call_stack[VM_CALL_MAX];
    AerVal*      registers;
    int          call_depth;
} VM;

void         chunk_init(Chunk* c);
void         chunk_free(Chunk* c);
void         chunk_emit(Chunk* c, unsigned long long word);

/* Records that bytecode from `offset` onward belongs to source `line`, once per statement not instruction (see line_mark_offsets); no-op if offset doesn't strictly increase from the last mark. */
void         chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line);

/* The source line whose statement contains `offset` (the largest recorded mark at or before it), or 0 if the chunk has no marks yet. */
unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset);

unsigned int chunk_add_pool(Chunk* c, AerVal v);
Shape*       chunk_find_shape(Chunk* c, const char* name);

/* See ChunkFunction's own comment above. `defaults` is taken by ownership (never copied), matching
   how AerFunction.defaults and Shape.field_defaults are already handled. */
void           chunk_add_function(Chunk* c, unsigned int name_idx, unsigned int code_offset,
                                   unsigned int arity, unsigned int min_arity, AerVal* defaults,
                                   bool has_receiver, unsigned int receiver_type);
ChunkFunction* chunk_find_function(Chunk* c, const char* name);

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
   reset stack_top=0 and call_depth=0 (repointing registers back at frame 0) — main.c's run()
   does this between REPL statements; other hosts must replicate it. */
bool vm_run(VM* vm);

/* GC suppression is a counter, not a flag (imports can nest: A imports B imports C). aer_module.c
   wraps its nested vm_run() for a file-module's top-level code in suppress/unsuppress: during that
   window the outer chunk/VM isn't in any root set yet, so a collection could sweep what the outer
   file still needs. Imports are small and one-time, so not collecting here is cheap insurance. */
void vm_gc_suppress(void);
void vm_gc_unsuppress(void);

/* Call setup used only by aer_module_call (aer_module.c) for a cross-module call into another
   file's exported function — see ChunkFunction's own comment. Pushes a real CallFrame onto
   target's own call stack (arity/receiver checks, default-filling, mirroring lbl_call_value
   exactly) with dest_reg fixed at 0, so the trampoline convention is: after vm_run(target) drains
   back to call depth 0 (hitting return_ip's OP_HALT), the result is sitting in
   target->call_stack[0].registers[0]. Full contract in setup_call's own comment in vm.c. */
bool setup_call(VM* target, Chunk* fn_chunk, ChunkFunction* fn, int arg_count,
                    AerVal* args, unsigned int return_ip);

/* Returns an uninitialized AerArray header from vm.c's internal slab pool, as if xmalloc'd directly (every in-file vm.c site still uses pool_alloc); exposed only because aer_stdlib.c's string.split() needs one and the pool isn't a raw global outside vm.c. */
AerArray* vm_new_array(void);

/* Same idea, for AerDict — exposed for aer_json.c's json.decode(); caller must set map.is_inline = true and zero the rest of map itself (see lbl_dict_new's call site in vm.c). */
AerDict* vm_new_dict(void);

/* Same idea, for AerFunction — exposed for parser.c's build_function_value. Must come from
   function_pool like every AerFunction: the GC's mark phase expects every TYPE_FUNCTION value in
   Chunk.pool to be a real function_pool cell, and an xmalloc'd/xcalloc'd one makes pool_mark's slab
   lookup fail outright, not just leak. Zero the struct yourself after calling — unlike the xcalloc
   this replaced, pool_alloc returns uninitialized memory. */
AerFunction* vm_new_function(void);

/* Reads back a register's value after a chunk has run to OP_HALT. Test-only: tests/smoke_test.c
   is the only intended caller. */
AerVal register_get(VM* vm, int slot);

#ifdef AER_DEBUG_TOOLS
#include <stdio.h>
/* Prints a byte-accurate memory breakdown (header vs. payload bytes per pool, plus GC run counts)
   to `out` — see source/core/vm.c for what "payload" means per type. Debug-build only. */
void aer_debug_memory_report(FILE* out);
#endif

#endif
