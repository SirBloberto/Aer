# AER Architecture

An internals-facing reference for how the interpreter actually works: value representation,
memory allocation and collection, the register VM, the compiler, and the optimizations layered on
top of all of it. README.md documents the *language*; this document explains the *implementation*
underneath it.

Source map: `source/compiler/{lexer,parser}.c` (front end), `source/core/vm.{c,h}` (bytecode
format + the VM itself, including `vm_run_slice()`, the bounded-instruction-count entry point the
scheduler below drives), `source/value.h` (value representation and its accessors),
`source/utilities/pool.{c,h}` (allocator), `source/stdlib/aer_*.c` (built-in library modules:
math/random/string/time/json/collection/net/regex/actor/scheduler/io), `source/core/aer_module.c`/
`aer_host.c` (import and host-embedding mechanisms), `source/core/aer_actor.c` (independent VM
spawning and a host-side mailbox, reachable from AER scripts via the `actor` module),
`source/core/aer_scheduler.c` (cooperative round-robin scheduler over spawned actors),
`source/core/disasm.c` (debug-only disassembler/profiler).

---

## Design highlights

**Single-pass elegance.** Lexing, parsing, and codegen collapse into one pass. Every `parse_*`
function is simultaneously the grammar rule and the code generator.

**Precedence climbing.** One function and a table (see README's [Operators](README.md#operators))
replace the traditional cascade of `parse_addition`, `parse_multiplication`, `parse_unary`, etc.
Adding an operator is one line — plus, for the one whose right-hand side isn't a general expression
(`in` is; `|>` isn't), one small special case. Casts and shape-checks aren't in this table at all —
they're ordinary calls and comparisons, not a dedicated operator.

**The VM is a computed-goto dispatch loop over integers.** No virtual dispatch, no pointer chasing,
no heap allocation in the hot loop. Direct-threaded dispatch lets the CPU's branch predictor learn
per-instruction patterns instead of funnelling every opcode through one `switch`.

**Every local is a flat, compile-time-resolved slot — no closures.** Assignment inside a function
is always local, which means every name in a function body resolves to a fixed slot at parse time,
not a runtime scope-chain walk. A function's data access is always either its own parameters/locals
or an explicit reference passed in — never an implicit reach into an enclosing function's variables.

**A real module system without a module value type.** `import math` and `import helpers` both work,
resolved entirely at parse time — file-based imports even run the imported file synchronously,
in a fully isolated `Chunk`/`VM`, before the importing file's own parse continues. No `TYPE_MODULE`,
no runtime namespace object, just a name the parser remembers.

**REPL function and struct persistence.** Functions and struct types survive across interactive
calls because the bytecode array and struct registry grow monotonically and are never reset. This
falls out naturally from the design — no special handling required.

**`OP_CALL_VALUE`.** Functions stored in arrays and hashtables can be called directly:
`ops[0](3, 4)` or `dispatch["add"](10, 20)`. The opcode removes the function value from the stack
in-place before jumping, which keeps the call convention identical to a named call.

**Structs without a new value type.** A struct instance is an array with a shape pointer — printing,
reference semantics, and heap layout are all inherited for free. The interesting engineering is
entirely in the guards that keep structs from silently behaving like arrays where that would be
surprising.

---

## 1. Value representation

Every runtime value — a VM register, an array element, a hashtable entry, a struct field, a stack slot
— is an `AerVal` (`value.h`):

```c
typedef struct AerVal {
    ValueType tag;
    union { bool b; int64_t i; double d; void* ptr; } as;
} AerVal;
```

An explicit tagged union, 16 bytes on a 64-bit build (4-byte enum tag, padded, plus an 8-byte
union). `TYPE_NULL == 0` is load-bearing: a zero-initialized `AerVal` (an untouched register, a
freshly-grown array slot) decodes as `null` with no explicit init needed, which is exactly what
`mark_vm_roots` relies on when it unconditionally scans every register of every live frame.

This replaced an earlier **NaN-boxing** scheme (an 8-byte encoding: a genuine IEEE double unless
its bit pattern matched a reserved NaN payload, in which case it was reinterpreted as a 3-bit tag +
packed payload). NaN-boxing was smaller, but direct machine-code disassembly against Lua's own
value representation showed its decode cost — masking and shifting a 64-bit word to test and
extract the tag on *every single touch of every value* — was the dominant remaining gap against
Lua's interpreter. A plain tag field is one aligned load; a NaN-boxed tag is a mask, a compare, and
(on ARM32, where the 64-bit mask constant doesn't fit in a single immediate operand) a literal-pool
load on top. Measured result of the switch: **-11.2% instructions on nbody.aer, -62.5% on an
isolated arithmetic loop, and a 3.3x drop in cache misses** — despite `AerVal` growing 8→16 bytes.
Integers are never boxed under this representation either: the old encoding could only fit a
47-bit inline integer and needed a heap-boxed fallback (`long_pool`) for the rare overflow; a plain
`int64_t` in the tagged union fits at any magnitude, so that whole fallback mechanism is gone.

`value.h` also holds the accessor functions (`aer_int`, `aer_as_string`, `aer_type`, ...) right
alongside `AerVal`'s own definition — every read/write of an `AerVal`'s payload goes through one of
these, never a direct `.as.x` anywhere else, so the representation itself can be swapped again
without touching call sites.

`AerVal` is the one value type, used identically in `AerNativeFn`'s signature (host-registered
functions, `include/aer.h`) as everywhere else internally — a host reads/writes it through the same
`aer_int`/`aer_as_string`/`aer_type`/etc. accessors as the interpreter itself, never a raw field.

---

## 2. Memory allocation

### 2.1 Slab pools, not malloc-per-object

Every heap type — `AerString`, `AerArray`, `AerDict`, `AerFunction`, struct instances,
`AerPackedArray`, and `AerResult` — is allocated from one of seven fixed-size **slab (arena)
pools** (`pool.c`), not individual `malloc` calls. `pool_init(pool, elem_size, elems_per_slab)`
sets one up; `pool_alloc(pool)` hands back memory sized for exactly one cell:

- **Free-list first**: if a previous cell was freed, `pool_alloc` reuses it (an intrusive
  singly-linked list threaded through the first `sizeof(void*)` bytes of each freed cell).
- **Bump allocation otherwise**: hands out the next cell in the current slab; when a slab is
  exhausted, one `xmalloc` grows a whole new slab (`POOL_INITIAL_SLABS = 4`, doubling), not one
  allocation per object.
- Returned memory is **uninitialized**, like `malloc` — the caller fills it in.

Seven pools exist (`vm_pools_init_once`, `vm.c`):

| Pool | Cell size | Elems/slab |
|---|---|---|
| `string_pool` | `sizeof(AerString)` | 256 |
| `array_pool` | `sizeof(AerArray)` | 256 |
| `dict_pool` | `sizeof(AerDict)` | 64 |
| `function_pool` | `sizeof(AerFunction)` | 64 |
| `struct_pool` | `sizeof(AerArray) + MAX_STRUCT_FIELDS * sizeof(AerVal)` | 64 |
| `packed_array_pool` | `sizeof(AerPackedArray)` | 64 |
| `result_pool` | `sizeof(AerResult)` | 64 |

`struct_pool` is the one deliberate exception to "header separate from payload": a struct
instance's field count never changes after construction (unlike a plain array's `items[]`, which
can grow), so there's no reallocation to support — bundling the header and its fields into
**one** pool cell instead of a header-plus-separately-`xmalloc`'d-items layout collapses two
dependent pointer dereferences (header, then its items buffer) into one on every field access.
The cost is real but bounded over-provisioning: every struct — even a 2-field one — reserves
`MAX_STRUCT_FIELDS` (16) slots, since a `Pool` requires a uniform cell size.

### 2.2 Variable-size payloads

A pool cell is fixed-size (the type's header only); anything variable-length is a separate,
individually `xmalloc`/`xrealloc`'d allocation the pool system doesn't track at all:

- `AerString.data` — the actual character bytes.
- `AerArray.items` — the element buffer (grows via `xrealloc`, doubling, in `lbl_array_new`/append
  paths). Not applicable to a struct instance (see above — inline, no separate buffer).
- `AerDict`'s hash table (`HashTable`'s sparse and dense arrays, `hashtable.c`).
- `AerFunction.defaults` — only when the function has default parameters.

Each corresponds to one line in `aer_debug_memory_report`'s "header vs. payload" breakdown
(debug-tools only) — `aer_gc_stats()` alone only gives a live *cell* count, which understates real
memory usage for exactly these variable-payload types.

### 2.3 Per-cell bookkeeping: `cell_state`

Every cell has one state byte, parallel to the slab array (`pool.h`):

```
bit 0  POOL_MARKED     — this collection cycle only
bit 1  POOL_OLD        — 0 = young, 1 = old; set once a cell survives one collection
bit 2  POOL_FREE       — on the free-list (stops the sweep from re-discovering and re-pushing it)
bit 3  POOL_REMEMBERED — already in the GC's remembered set (see write barrier, below)
```

Byte-per-cell, not bit-packed, following this project's readability-over-micro-optimization
precedent. **Aging is per-cell, not slab-position-based** — a slab-boundary aging scheme breaks the
moment a freed *old* cell is reused for a *young* allocation (it would silently inherit the wrong
generation); tagging the cell itself is correct regardless of which physical cell gets reused,
since `pool_alloc` always clears the generation bit on every allocation (fresh or free-listed).

Mapping a live cell pointer back to its state byte (`pool_cell_state_or_null`) is a linear scan
over the pool's slabs, checking which slab's address range contains the pointer. Scanned
**newest-slab-first**, not oldest: `pool_alloc` always bump-allocates fresh cells from the newest
slab once the free list is empty, and the hottest caller (`gc_barrier_array`, invoked on every
array/hashtable/struct-field write) is disproportionately likely to be checking a cell that was itself
just allocated. (Measured to make no difference on `nbody.aer` specifically — its steady-state
allocation pattern keeps `slab_count` small — but sound and free, and could matter for a
longer-accumulating program.)

### 2.4 Garbage collection: generational mark-sweep

A full stop-the-world generational collector (`vm.c`'s "Generational GC" sections), not
reference-counted, not incremental.

**Roots**, walked fresh every cycle (`mark_vm_roots`, `mark_chunk_roots`):
- The value stack (`vm->stack[0..stack_top)` — the module/stdlib-call bridge, see §3.3).
- Every register of every **live** call frame — bounded to `0..call_depth`, not all `VM_CALL_MAX`
  frames regardless of depth. (A blanket scan over every frame slot was a measured cache-miss
  hotspot: even shallow recursion walked every frame's worth of cold, mostly-zeroed memory on every
  GC pass. Frames beyond `call_depth` are dead — already returned, defers already drained by
  `lbl_return` before unwind — so bounding the scan can't under-collect.)
- Each live frame's pending `defer` call arguments (`CallFrame.defers[]`, stored outside
  `registers[]`).
- The chunk's own constant pool (`Chunk.pool[]`) and every registered struct `Shape`'s field
  defaults — permanent roots, since a bytecode constant or a struct's declared default must never
  be collected out from under a later reference to it.
- If file-based `import`s are active, every imported module's *own* VM/Chunk gets its roots walked
  too (`aer_module_get` iterates the whole file-module registry); every spawned actor's VM/Chunk
  likewise (`aer_actor_get`, `vm.c`'s `gc_collect`, mirroring `aer_module_get`'s own enumeration) —
  the seven pools are one shared heap fed by N independent root sets (the main VM plus one per
  imported file plus one per live actor), not N separate collectors.

**Mark phase**: an explicit growable worklist (`MarkWorklist`), not C call-stack recursion — user
data structures (deeply nested arrays/hashtables) have no depth limit, so recursion would risk a native
stack overflow on adversarial input; `pool_mark`'s "already marked" return is what terminates
cycles (a self-referential array marks itself once, then stops).

**Write barrier + remembered set** — the piece that makes the *generational* part sound. A minor
collection only marks/sweeps *young* cells, on the assumption that no *old* object was mutated to
point at a young one since the last cycle... except that assumption is only true if every mutation
that violates it is caught. `gc_barrier_array`/`gc_barrier_dict` (called from every index-assign,
`append`, and struct field-set) check exactly that: if the container being written into is *old*
and the *new value* being stored is *young*, the container is added to `remembered_set` (deduped
via the cell's own `POOL_REMEMBERED` bit — O(1), not a linear scan) and re-traced as an extra root
on every subsequent minor collection. Entries are never proactively removed (matching the
add-only design of the bit itself) — a later *major* collection can legitimately free a remembered
object between when it was added and now, so the minor-collection code checks each entry's
liveness (`pool_is_freed`) before dereferencing it, compacting the array in place.

**Sweep** (`pool_sweep`, per pool): walks every carved-out cell; a free-listed cell is always
skipped (nothing marks a free cell, so it must never be re-freed); if this is a *minor* pass, an
old cell is skipped too (presumed live — the whole point of generational collection is not paying
to re-trace long-lived data every cycle); a marked cell **survives and is promoted** (mark bit
cleared, `POOL_OLD` set — done inline, during sweep, not via any separate "promote" call); an
unmarked cell is finalized (`free_string`/`free_array`/`free_dict`/... — frees the cell's *separate*
payload, e.g. `AerString.data`) and pushed onto the free-list.

**Trigger** (`gc_maybe_collect`, `vm.c`): checked from inside individual allocating opcode
handlers, **not** from `DISPATCH()` on every single instruction (see §5.1 for why this placement
itself was a real, measured win). A minor collection runs once `pool_total_alloc_count` (a single
shared counter across all 7 pools) crosses `minor_gc_threshold` (default 2048, `aer_gc_configure`);
a major collection runs after every `major_gc_every_n_minor` (default 10) minor ones. An optional
live-cell **ceiling** (`aer_gc_set_ceiling`, 0 = unlimited) is checked once per opcode after the
normal rhythm — if exceeded, an extra major collection is forced before the process gives up and
reports "Memory ceiling exceeded" via the normal (non-fatal, longjmp-based — see §5.1) error path.

**When cleanup actually happens, concretely**: right after the specific allocation that could have
crossed the threshold — e.g. `lbl_array_new` calls `pool_alloc(&array_pool)`, fills in the new
array, *then* calls `gc_maybe_collect(vm)` — never before the new value is reachable from a root.
This ordering is itself load-bearing in a few places: e.g. `setup_call`/`vm_call_value` must
increment `call_depth` (making the new frame part of `mark_vm_roots`'s scan) *before* calling
`gc_maybe_collect`, or a collection triggered by filling in default-parameter values could reclaim
a freshly-allocated default array as unreachable.

**Import isolation**: `vm_gc_suppress`/`vm_gc_unsuppress` (a nesting counter, not a flag — imports
can nest, A imports B imports C) wrap a file-module's own nested `vm_run()` during load: that
module's chunk/VM isn't registered in the file-module list yet (that happens only once loading
finishes), so a collection during that window could sweep something the *outer* file still needs.
Imports are small and one-time, so skipping collection during that narrow window is cheap
insurance, not a real leak risk.

---

## 3. The VM: a register-based bytecode interpreter

### 3.1 Bytecode format

`Chunk.code` is a flat array of `uint64_t` words (widened from 32-bit specifically so the hottest
opcode family, `OP_BINARY`, could pack dest + both RK operands into **one** word — see §3.2).
Every instruction is one *descriptor* word (opcode + whichever small operands fit alongside it),
optionally followed by one or more *wide* operand words (jump targets, pool indices, RK-encoded
values too large for the packed scheme). Jump targets are **never** packed alongside anything
else, on purpose: `patch_jump` does a blind word-overwrite at the target offset from dozens of call
sites, and keeping every patchable field in its own dedicated word is what lets that stay a blind
overwrite instead of a read-modify-write.

### 3.2 Instruction packing — several tiers, chosen per opcode's actual heat

All of this exists because of one finding: on a 32-bit ARM target, a `uint64_t` is fetched as two
32-bit register halves, and a packed field that straddles that boundary needs a shift-and-OR
reconstruction across both registers before it's usable. Direct disassembly of `lbl_add` found
roughly 30 of its ~71 instructions were exactly this reconstruction tax. The fix, applied
opcode-by-opcode in order of measured heat, not all at once:

- **`PACK3`/`PACK2`/`PACK1`** — the original, general scheme: `[opcode:8][a:8][b:8][cc:8]`, one full
  byte per field, still used by opcodes that never got individually tuned.
- **`PACK_REG4`** — four plain register indices (7 bits each, exactly `FRAME_REGISTERS`) packed with
  the opcode, for calls-through-a-register-value.
- **`PACK_BINARY`** (`OP_ADD`/`OP_SUB`/.../`OP_IN` — over a third of all dispatches on `nbody.aer`) —
  the tightest, most bespoke encoding: `opcode(7) + dest(7) + rk_b(9) + rk_c(9)` = 32 bits, entirely
  in the **low** word, so the upper 32 bits of the 64-bit code word are never touched at all for
  these opcodes. This is also why `RK9` (a narrower RK sub-encoding, 1 flag + 8 index bits — smaller
  than the general `RK20`) exists: fitting the *whole* instruction in 32 bits required a smaller RK
  budget, guarded at compile time (`emit_binary`'s `rk9_fits`, `parser.c`) so a program that would
  overflow it reports a clean compile error instead of silently corrupting the encoding.
- **`PACK_FIELD_SET`/`PACK_INDEX_GET`** — the next-hottest opcodes (measured 30M and 22.5M hits
  respectively on `nbody.aer`), narrowed the same way once `OP_BINARY` proved the technique worked.
- Every packing macro takes an explicit `(uint32_t)` cast before shifting, not just a mask
  afterward — disassembly showed the reconstruction tax could still occur even after a field was
  logically confined to the low 32 bits, until the compiler was *told* the upper bits didn't matter
  via an explicit narrowing cast.

Two RK operand-encoding widths coexist on purpose: **RK9** (`OP_BINARY`'s family — flag bit 8, 8
index bits, 256 slots) and **RK20** (everything else still using the wider scheme — flag bit 19, 19
index bits, ~524k slots). RK9 is deliberately smaller because it had to fit inside `OP_BINARY`'s
32-bit budget; RK20 is "far beyond any real program" headroom for opcodes with room to spare. Both
report a clean compile error, never silent truncation, if a real program's resolved operand
actually exceeds the budget.

### 3.3 The dispatch loop

Computed-goto dispatch (`vm_run`, a `static void* dt[] = { [OP_ADD] = &&lbl_add, ... }` table +
`goto *dt[cur_op]`), the standard technique for beating a `switch`-based bytecode loop (no bounds
check, no jump-table-then-branch — each opcode handler jumps directly to the next). The opcode
itself is masked out of the low **7** bits of the descriptor word (`0x7F`, not `0x8F`) — 62 real
opcode values fit with 66 to spare, deliberate headroom for e.g. future concurrency primitives
without a second encoding redesign.

`DISPATCH()` itself (the per-opcode macro) does the absolute minimum: read the next word, split
out the opcode, jump — no per-instruction GC check and no per-instruction error-flag check (see
§5.1 for how both stay off this path entirely).

**`CallFrame`** (`vm.h`) is the unit of call isolation: `registers[FRAME_REGISTERS]` (128 slots),
plus (for the primitive pass, §4.3) `raw_ints[32]`/`raw_reals[32]`, plus `return_ip`/`dest_reg` and
a small fixed-size deferred-call list. `VM.call_stack` is a flat array of these
(`VM_CALL_MAX = 64` frames); `vm->registers`/`raw_ints`/`raw_reals` are **pointers repointed at the
current frame** on every call/return (not re-derived from `call_depth` on every access) — a tail
call reuses the current frame in place, so it's the one case that needs *no* repointing.

`VM.stack` (256 slots) is a **separate**, genuinely stack-based array — not used by any
register-native opcode. It exists purely as a bridge for calls that cross out of the register
calling convention: `OP_CALL_MODULE`'s dispatch into stdlib (`aer_math_call` and friends, which
take a popped-argument/pushed-result calling convention) and `aer_module_call`'s cross-file-module
argument hand-off. Stack-neutral from the caller's perspective — `stack_top` always ends exactly
where it started.

### 3.4 Calls, returns, and tail-call reuse

Every call — `OP_CALL` (compile-time-resolved offset), `OP_CALL_VALUE`/`OP_CALL_GLOBAL_VALUE`
(runtime function value in a register or a global), and `setup_call` (the cross-file-module
trampoline, `aer_module.c`) — does the same thing: arity/receiver-type check, bulk-copy argument
registers from the caller's frame into the callee's frame starting at register 0 (always a
forward, non-overlapping-hazard copy — see `lbl_call`'s own comment for why increasing-index
iteration is safe even when caller/callee frames alias registers), fill any omitted trailing
parameters from the function's own compiled-in defaults, save `return_ip`/`dest_reg` in the
*callee's own* frame (not a single shared global — this is exactly what makes nested/recursive
calls safe), bump `call_depth`, repoint `registers`/`raw_ints`/`raw_reals`, jump.

**Tail-call reuse**: when `return f(args)` is the *entire* return expression (checked at compile
time — nothing wraps the call), the already-emitted `OP_CALL`/`OP_CALL_VALUE` opcode word is
patched *in place* to `OP_TAIL_CALL`/`OP_TAIL_CALL_VALUE` (same operand layout — only the opcode
byte changes, a blind mask-and-OR, no re-encoding). At dispatch time, if the *current* frame has no
pending `defer`s (a deferred call must still run before this frame's storage is reused for someone
else's locals), the new arguments simply overwrite the current frame's own registers 0..arg_count
and `ip` jumps straight to the callee — `call_depth`, `dest_reg`, and `return_ip` are untouched, so
however many tail calls chain, the *original* (non-tail) caller still gets its result in the right
place once the chain finally returns via a real `OP_RETURN`. This is what makes deep tail recursion
run in **O(1) stack frames** — verified by a dedicated embedding test that a genuinely deep
*non*-tail recursion still overflows (`VM_CALL_MAX`), confirming the optimization isn't silently
over-applying.

**Struct field access** uses a **per-call-site inline cache** (`Chunk.field_cache_shape[]`/
`field_cache_slot[]`, indexed by the accessing instruction's own bytecode offset): the common case
— the same struct type hitting the same `.field` expression on every iteration of a hot loop — skips
straight to the cached slot index instead of re-scanning `shape->field_names[]` linearly. A
polymorphic call site just keeps missing the cache (one wasted pointer compare) and falls back to
the linear scan; the cache is never *incorrect*, only sometimes not-sped-up, since a `Shape*` is
never reallocated once created.

---

## 4. The compiler (`parser.c`)

### 4.1 Single-pass, no separate AST

The lexer feeds tokens directly into a recursive-descent compiler that emits bytecode *as it
parses* — there is no whole-program intermediate representation, no separate optimization pass
over a tree. (An earlier `Node`/`compile_node` tree type existed purely as test scaffolding —
`tests/smoke_test.c` used it to hand-build small expression trees below the level of real source,
predating any real `.aer` file integration — but it was never part of the real front end; removed,
and those tests rewritten to emit bytecode directly via `chunk_emit`/`PACK_BINARY`, the same style
the rest of that file already used for control flow and calls.) This single-pass constraint is why
some optimization ideas are architecturally ruled out (e.g., a hypothetical "every struct field
named `x` is always type `T`" inference would need whole-file lookahead) while others are
specifically designed to fit within it (the primitive pass below classifies a variable's type from
information already visible at its point of use, never needing to look ahead).

### 4.2 Register allocation

A Lua-style watermark allocator (`reg_alloc`/`reg_free`/`reg_reserve`, modeled directly on Lua's
`FuncState.freereg`): `next_temp_register` grows when a sub-expression needs a scratch register and
shrinks back once its result is consumed, LIFO — which falls out naturally from recursive-descent
compilation's own call structure. `reserved_floor` marks the boundary below which a register is
*permanent* (a real local variable or parameter, or a loop's own promoted iteration state) and must
never be handed out as a temp or freed by ordinary expression compilation. Every hand-out is bounds
checked against `FRAME_REGISTERS` (128) — an unchecked overflow here would be an out-of-bounds
write into `CallFrame`'s neighboring fields, surfacing much later as a GC-time segfault, so it's a
clean compile error instead.

Every operand a compiled sub-expression produces is returned as a single `int`: either a plain
register index, or (with `RK_CONST_FLAG` set) a constant-pool index — this is the RK encoding
described in §3.2, decided at *compile* time and baked into the emitted instruction's operand bits.

### 4.3 The "primitive pass" — raw unboxed locals

A local variable the compiler can *prove*, from information already visible at each assignment, is
always the same primitive type (`integer` or `float`, never string/array/hashtable/struct/function) gets
a raw, unboxed `int64_t`/`double` slot in `CallFrame.raw_ints`/`raw_reals` instead of a tagged
`AerVal` register — and dedicated opcodes (`OP_RAW_ADD_INT`, `OP_RAW_LT_REAL`, ...) that skip both
the RK register-vs-constant check *and* the value's tag check entirely, since the compiler already
knows statically which form applies. Tracked per-name via `parser.c`'s `var_kind` table
(`VAR_BOXED`/`VAR_RAW_INT`/`VAR_RAW_REAL`), a strictly one-way state machine
(`UNTRACKED -> RAW_* -> BOXED` is allowed as a "shadow"; `BOXED -> RAW_*` never is).

Real, deliberately narrow scope rules (each one closes a specific correctness gap, not just style):
- **Never a function parameter** — a parameter's actual runtime type isn't knowable at compile time
  without call-site analysis.
- **Never top-level/global code** — only inside a function body (`function_depth > 0`); raw storage
  lives in `CallFrame`, and globals are mirrored separately (`global_names`/`global_regs`).
- **Only composes through arithmetic on already-raw operands or literals** — never through a
  function call, a container read, or anything needing a runtime type check to resolve (this is
  also what keeps a field-read-derived value from ever being misclassified as raw: `x = y.field + 1`
  must compile to an ordinary boxed `OP_ADD`, verified by disassembly, not assumed).
- **Disqualified permanently if ever assigned inside an `if`/`else` branch** — the classic phi/merge
  problem (a name assigned different types down mutually exclusive branches, read after the join,
  can't be resolved by a single-pass compiler without real dataflow analysis). Loops do **not**
  disqualify — a loop body runs unconditionally each time it runs, no divergent-paths-reconverging
  ambiguity.

Overflowing the raw-slot budget (32 int + 32 float per call) is never a compile error — it's a
graceful fallback to ordinary boxed storage for the overflow names, mirrored by the raw allocator's
own `-1`-on-overflow return convention (unlike the *boxed* register allocator, which does hard-error
on overflow, since that ceiling is a real architectural limit with no fallback).

Measured effect: no win on `nbody.aer` (deliberately out of scope — that benchmark is
struct-field-arithmetic dominated, not local-variable dominated) but **~29% instruction reduction**
on a pure local-variable tight loop, exactly the case this targets.

### 4.4 Fusion: field access folded into arithmetic

`parse_binary_ops` recognizes, at emit time, the specific shape `x OP y.field` (or `y.field OP x`)
— found via a real per-opcode dispatch audit on `nbody.aer`, where this exact pattern (`dx = bix -
bj.x`) compiled as a separate `OP_FIELD_GET` immediately followed by an `OP_BINARY` reading that
temp: two dispatches for something that's structurally one operation. The already-emitted
`OP_FIELD_GET` is truncated and re-encoded as `OP_BINARY_FIELD`/`OP_FIELD_BINARY` (operand order
determines which), each opcode fully packed the same way `OP_BINARY` is (§3.2).

### 4.5 Forward references and cross-module calls

A call to a name not yet known at the point it's compiled (a function defined later in the same
file, or later turning out to be a global variable rather than a declared function) is compiled
optimistically as a placeholder `OP_CALL`, tracked in a pending-call list, and patched once the
real target is known — by the end of the same `parse()` call for an ordinary forward reference, or
retargeted to `OP_CALL_GLOBAL_VALUE` if the name turns out to resolve to a plain global variable
holding a function value instead of a declared function. File-based `import`s compile the imported
file into its *own* `Chunk`/`VM` (parser state is saved/restored around the nested compile so it
can't corrutpt the importing file's own still-in-progress state) and register successfully-compiled
functions in a chunk-level `ChunkFunction` registry that outlives the parser state that created it
— needed because cross-module calls (`aer_module_call`) happen long after the importing file's own
parser tables have been reset for whatever compiles next.

---

## 5. Cross-cutting optimizations: current design and why

Full "what was tried, measured, and reverted" narrative lives in a local, unshipped file
(`OPTIMIZATION_HISTORY.md`) — this section states the current design and the headline number only.

### 5.1 Dispatch-loop overhead

`DISPATCH()` carries no per-instruction GC check and no per-instruction error-flag check. GC checks
are hand-placed at the handful of opcodes that can actually allocate (`gc_maybe_collect`, right
after `OP_ARRAY_NEW`/`OP_STRUCT_NEW`/`append`/string concatenation/...) — an opcode that can never
reach `pool_alloc` has zero GC-related cost, not a skipped check. Errors propagate via
`setjmp`/`longjmp`: `vm_run` sets a catch point once at the top of its own call, `error()` jumps
straight back to it the instant a fault fires, with no flag to poll anywhere. A blanket per-opcode
check of either kind was tried and measured worse; not revisited. Combined measured effect: -2.2%
instructions.

### 5.2 Value representation: tagged union

Covered in full in §1 — the single largest win measured this whole project (-11.2% instructions on
`nbody.aer`, -62.5% on isolated arithmetic).

### 5.3 Instruction encoding narrowing (Tier 1)

Covered in §3.2. Narrowing `OP_BINARY` specifically: instructions went up slightly (+0.1–0.13%)
while wall-clock/IPC were consistently better (~5–8%) — the wall-clock win is trusted as the real
signal; the discrepancy's exact mechanism was never fully pinned down.

### 5.4 The direct-destination-write pattern

Binary-op handlers write their result through a destination pointer directly (`*result = ...`)
rather than building a local `AerVal` and copying it into `vm->registers[dest]` afterward — a
16-byte stack round-trip that profiling found was the single hottest instruction in `nbody.aer`'s
whole profile (~3.6% of cycles). Array-index writes use the same pattern. ~5–6% wall-clock win
(IPC 1.73–1.76 → 1.82–1.86) plus ~0.75% further instruction reduction from the index-write case.

### 5.5 Module/builtin call dispatch: numeric ID, not `strcmp`

A module name in `module.fn(...)` is always a literal identifier (never ambiguous), so it's
resolved once at parse time to a small integer ID (`CALL_MODULE_MATH` etc.), and the VM switches on
that int instead of running a `strcmp` chain per call. `CALL_MODULE_DYNAMIC` (a host-registered
module or a file import — genuinely only resolvable by name at runtime) is the sole remaining
`strcmp` path. `OP_CALL_BUILTIN`'s dispatch (`length`/`print`/`type`/...) works identically via
`builtin_call_id` (parser.c). Measured ~2.8–3% of `nbody.aer`'s cycles recovered.

### 5.6 `vm_binary` fast/cold split

The shared arithmetic helper is split into `vm_binary_fast` (small, `always_inline`) and
`vm_binary_cold` (everything else, out-of-line) — inlining the whole thing bloats `vm_run`'s icache
footprint (161KB vs 108KB, a -75% cache-miss difference with the split); never inlining it costs
~5% on the callers hot enough to need it. Net: -1.6–1.7% instructions.

### 5.7 Build-level wins: LTO and PGO

`-flto` is load-bearing, not optional — `pool.c`'s hot, tiny functions (`pool_cell_state`,
`pool_is_young`, `gc_barrier_array`) are called from `vm.c`, a different translation unit, and pay
full cross-TU call/return overhead without it (~4.4% fewer instructions, ~5–7% faster wall clock
with it on). PGO (`-fprofile-generate`/`-fprofile-use`, see the README's Building section) measures
a further 7–9% wall-clock win with zero source changes, compounding with LTO — not wired into the
default build, to keep the makefile small.

---

## 6. Known architectural limitations (current, unresolved)

- **RK9-decode cost is real and not eliminated.** The register-vs-constant flag test inside
  `vm_rk_ptr9` remains a measurable cost even after PGO (shrunk, not eliminated — same code shape
  under profiling). An RR-opcode-split (separate register-register and register-constant opcode
  variants, eliminating the runtime branch entirely) and a "unified addressing" alternative were
  both scoped and explicitly set aside — real complexity/opcode-surface cost for an unproven payoff.
- **No SIMD/vectorization, no JIT.** Confirmed concretely, not just assumed: GCC's own
  `-fopt-info-vec` diagnostics produce zero output — not even a "missed" message — for `vm_run_slice`
  at any optimization level, because the dispatch loop isn't a shape the vectorizer analyzes at all
  (a computed-goto loop over heterogeneous opcode handlers, not a fixed-body countable-trip loop).
  This is fundamental to being an interpreter, not something a flag fixes. Separately, on the
  Raspberry Pi 4 build specifically: this GCC target's default (`-mfpu=auto` on `armv7-a+fp`) doesn't
  even enable NEON, so *no* loop anywhere in the codebase auto-vectorizes right now, dispatch loop or
  otherwise (confirmed with a trivial textbook-vectorizable loop: `no vectype for stmt` without
  `-mfpu=neon`). Adding `-mfpu=neon -mfloat-abi=hard` gets past that, but float vectorization then
  additionally requires `-ffast-math` (NEON's FP unit isn't fully IEEE-754 compliant — GCC won't
  auto-vectorize float loops without permission to relax that) — a flag with real behavioral risk for
  a language runtime (relaxed NaN/Inf/signed-zero semantics) that should never be applied globally,
  only scoped to a specific, deliberately-audited function if ever used. Even then: NEON (32- or
  64-bit) has no gather/scatter — confirmed for this exact chip (Cortex-A72/BCM2711) — so a
  hypothetical bulk-vectorized opcode over the CURRENT AoS-packed struct-array layout would need
  manual scalar de-interleaving before vectorizing, likely eating most of the gain; the current AoS
  choice (over SoA, made earlier for dynamic-typing/aliasing reasons — see the flat-typed-arrays
  design discussion) is also the one layout choice that makes gather-free SIMD hard on this hardware.
  Net: real vectorization would need (a) a new bulk-operation opcode family with its own tight,
  auditable C loop — the interpreter's normal per-element dispatch can't get this for free — and (b)
  probably reopening AoS vs. SoA for at least that opcode family's storage. Not attempted; the
  cost/risk (relaxed float semantics, a settled layout decision, real implementation size) hasn't yet
  been weighed against a confirmed payoff on the actual target hardware.
- **No OS-thread parallelism at the language level, by design.** A `VM`'s registers/call-stack are
  per-instance (proven by file-based `import`, which already runs each imported file in its own),
  but the GC-managed heap (`string_pool`/`array_pool`/etc., `vm.c`) is one set of pools shared by
  the whole process — two VMs executing simultaneously on separate OS threads would race on the
  allocator and collector. Cooperative, single-threaded concurrency exists instead
  (`source/core/aer_actor.h/c` + `aer_scheduler.h/c`, reachable from AER scripts via the `actor`/
  `scheduler` modules): actors are independent VMs (spawned via `aer_vm_instantiate_from_file`, the
  same primitive `aer_module_load` uses) driven by `vm_run_slice()` — the `vm_run()` dispatch loop,
  refactored to take a bounded instruction count and return `VM_SLICE_YIELDED` instead of running
  to completion, with the budget checked only at the three sites a script can spend unbounded time
  (`lbl_jump`, `lbl_call`, `lbl_iter_range_loop`'s back-edge — never a blanket per-`DISPATCH()`
  check). This works with no fiber/`ucontext`/stack-copying machinery because AER calls never
  recurse in C — every call pushes a `CallFrame` onto a plain array and jumps, so a "suspended"
  script's entire state already lives on the `VM` struct, not the C stack; `vm_run()` already
  resumed from wherever `vm->ip` pointed as its normal contract before this existed (the REPL's own
  statement-by-statement execution relies on the same fact). The scheduler round-robins queued
  tasks through this in small slices until each finishes or errors. True OS-thread parallelism
  would additionally require moving the pools from process-global statics into per-`VM` fields, a
  real but separable, larger refactor — not attempted.
- **TODO: O(n²) minor-GC rescan of a large, repeatedly-grown array OR dict (found, not yet fixed).**
  A remembered array or dict (promoted old, holding a young reference — `gc_barrier_array`/
  `gc_barrier_dict`, vm.c) is scanned in full on *every subsequent minor GC*, for as long as it stays
  remembered (entries are never proactively removed): `REMEMBERED_ARRAY` does
  `for (j = 0; j < a->count; j++) worklist_push(...)`, `REMEMBERED_DICT` does the identical
  `for (j = 0; j < map->count; j++) worklist_push(map->dense[j].payload)` (vm.c:355-390) — this is a
  property of the general remembered-set mechanism, not specific to arrays. (`REMEMBERED_STRUCT` is
  naturally immune: field count is fixed at struct definition, small, bounded by `MAX_STRUCT_FIELDS`.)
  For a container built via many incremental insertions (`append()`, or repeated `dict[key] = value`
  with new keys), each of the ~(N / minor_gc_threshold) minor GCs across construction rescans the
  *entire current* contents, not just what's new since the last scan — total cost degrades to
  roughly O(n²/threshold). Confirmed via `perf stat` on real hardware (a Raspberry Pi 4):
  `bench/struct_array_scan.aer`'s own `make_particles(2_000_000)` setup phase — which runs *before*
  the benchmark's own internal timer starts, so this was invisible to every prior session's own
  "Took: Xs" comparisons — takes ~307 seconds alone, with IPC pinned at 0.45 (heavy, repetitive,
  memory-bound rescanning), matching the predicted mechanism closely. (The dict case is unconfirmed
  by a benchmark — every dict in the current bench suite is small/bounded — but the code path is
  identical, so it's presumably exposed to the same risk at large-N.) The standard fix (delta-scan:
  track a `last_scanned_count` per remembered container, push only the entries added since the last
  scan, update after each scan) turns the pure-growth case from O(n²) to O(n) for both container
  kinds, but needs care: `gc_barrier_array`/`gc_barrier_dict` are also the write barrier for
  overwriting an *existing* entry with a new young value (`arr[i] = x`, or updating an existing dict
  key) — a naive high-water-mark delta-scan would miss that case, so the real fix needs to distinguish
  "grew via append/new-key" from "mutated an existing slot" (or track dirty ranges rather than just a
  count), for both `AerArray` and `AerDict`. Deliberately not fixed yet — deferred pending a careful
  design pass, given generational-GC correctness here has bitten this project more than once before.
- **TODO: no small-string optimization — every `AerString` is a separate heap allocation.** Real
  `perf stat` hardware counters on a Raspberry Pi 4 (`bench/log_processing.aer`, string-interpolation-
  and `string.split`-heavy) showed data-cache misses outnumbering instruction-cache misses **140:1**
  (17.8 vs 0.127 per 1000 instructions); the same ratio on a numeric-array-heavy workload
  (`bench/nbody.aer`) was only 16.5:1. `aer_make_string` (vm.c) always allocates a fresh owned buffer
  via `heap_alloc(heap, &heap->string_pool)` for the cell *and* a separate `xmalloc` for `data` (see
  §2.2) — every interpolated string, every `split()` result, is two separate allocations plus a
  pointer-chase to reach the bytes. Confirmed precisely by reading `FN_STRING_SPLIT`
  (aer_string.c): splitting one `log_processing.aer` line ("`{i} GET {path} {status} {n}`", 5 fields)
  costs 5 `xmalloc`s (one per field's byte buffer) + 5 `AerString` pool cells + 1 array-pool cell + 1
  more `xmalloc` for the result array's `items` buffer — **11 separate heap events per line**, almost
  all of them tiny (3-15 byte) strings that die within a few instructions of being read. `HashTable`
  keys (`hashtable.c`) have the identical shape — `HashTableEntry.key` is a pointer into its own
  size-classed pool allocation, not inline bytes — so `status_counts`/`path_counts`'s dict lookups pay
  the same extra indirection on top. This is the likely dominant cause of the 140:1 ratio above, and
  it isn't a case of AER missing something already fixed elsewhere: §2.2 documents the variable-payload
  split as a deliberate design choice for genuinely variable-length data, and this is the natural
  complementary case — the overwhelming majority of these strings are short enough that inlining
  would apply. Small-string optimization (inlining short strings' bytes directly into the value
  representation instead of a separate heap cell, avoiding both the allocation and the pointer
  dereference for anything under some small length threshold) is the
  standard, well-proven fix for exactly this access pattern — used by V8, LuaJIT, and Swift, among
  others, for the same reason. Not scoped yet: this would touch `AerString`'s representation
  (value.h) and every site that reads string data, a larger change than anything else on this list.
- **TODO: no array-reserve builtin — `collection` module has no `hashtable_reserve`-equivalent.**
  `hashtable_reserve` (hashtable.h) already exists specifically to pre-size a dict's sparse/dense
  arrays once, up front, skipping the incremental one-entry-at-a-time growth `hashtable_put` would
  otherwise do. `AerArray` has no equivalent — every `append()`-built array (e.g. `struct_array_scan.
  aer`'s `make_particles`) pays the same incremental-growth cost `hashtable_reserve` was built to
  avoid for dicts, with no way to opt out. A `collection.reserve(arr, n)` mirroring the existing dict
  mechanism (pre-size `items`/`capacity` once, `xrealloc` immediately to `n` rather than doubling on
  every overflow) would be small, additive, and low-risk — it doesn't fix the O(n²) minor-GC rescan
  bug above (that's about *how often* the array gets rescanned once old, not the reallocation cost),
  but is a real, complementary, and much cheaper win for the same "build a huge array via many
  appends" pattern. Not built yet.
- **TODO: opt-in 32-bit (`int32`/`float32`) fields for structs and packed arrays.** Distinct from —
  and a much better-grounded idea than — shrinking the general `AerVal` (see below): a struct/
  packed-array field's type is already known statically at compile time via `Shape.field_types[]`,
  and every access already goes through a fixed byte offset with zero runtime type-tag recovery (e.g.
  `OP_FIELD_GET_RAW_REAL`, `off=24`) — so narrowing a field from 8 to 4 bytes doesn't reintroduce the
  per-access unpacking cost that made NaN-boxing a measured regression (see below); it's a direct
  continuation of the already-validated principle behind the existing typed-struct-fields work
  (raw 8-byte fields over full 16-byte boxed `AerVal`, chosen for exactly this reason). Confirmed
  feasible with no infrastructure redesign: `Shape.field_offsets[]` (vm.c, `OP_DEFINE_STRUCT`'s
  handler) is already a genuine per-field cumulative sum (`offset += field_types[i] == TYPE_ANY ?
  sizeof(AerVal) : 8`), not a fixed stride — a third, 4-byte case slots directly into that existing
  loop. Real costs: (1) needs new opt-in syntax (`int32`/`float32` as distinct declared field types,
  not silently narrowing the existing `integer`/`float`, which must stay 64-bit everywhere else in
  the language); (2) doubles the raw-field-access opcode surface for structs/packed arrays
  specifically (32-bit counterparts of `OP_FIELD_GET_RAW_REAL`/`OP_FIELD_SET`/
  `OP_FIELD_COMPOUND_RAW_REAL`/etc.); (3) real precision-loss risk needing validation against a
  correctness oracle, not just assumed fine — `nbody.aer`'s own energy-conservation check (already
  used elsewhere this session to catch other regressions) is the natural test, since accumulated
  float32 rounding error over many iterations could plausibly drift further than float64 does today.
  Expected payoff shape: for an array already far larger than any cache (`struct_array_scan.aer`'s
  2,000,000 × 56 bytes = 112MB, already streaming from main memory every pass regardless of field
  width), halving field width mainly cuts memory *bandwidth* consumed per pass, not miss *rate* —
  consistent with that benchmark's own existing design comment, which already reasons about "total
  byte footprint... determines how much survives in cache between passes." Not scoped in detail yet.

---

## Appendix: every allocation site, and exactly when it fires

Two entirely separate allocation lifetimes exist in this codebase. Don't conflate them:

- **Compile-time / permanent** — grows a `Chunk`'s own bookkeeping arrays. Lives for the process's
  life (or the importing module's life), never GC-tracked, freed only by `chunk_free` (or, for
  `shapes`/`functions`, not even then — see below).
- **Runtime / GC-tracked** — a pool cell (one of the 7 pools, §2.1) plus, for variable-length types,
  a separate `xmalloc`'d payload. Reclaimed only by the collector (§2.4), never by an explicit
  `free()` call from ordinary VM code.

### A. Compile-time, permanent, never GC-tracked

| Site | What grows | Trigger | Freed by |
|---|---|---|---|
| `chunk_emit` | `Chunk.code[]` (doubling) | every bytecode word emitted during compilation | `chunk_free` |
| `chunk_pool_append`/`chunk_add_pool` | `Chunk.pool[]` (doubling) | every new constant/name literal | `chunk_free` (also frees each `TYPE_STRING` entry's owned `data`) |
| `chunk_add_pool` (string path) | `name_index` hashmap entries (`xmalloc`'d key + index box) | first occurrence of a given string literal (dedup — later occurrences just look up the existing pool index) | `hashmap_free` in `chunk_free` |
| `chunk_mark_line` | `line_mark_offsets[]`/`line_mark_lines[]` (doubling) | once per compiled *statement* (not instruction) | `chunk_free` |
| `chunk_ensure_field_cache` | `field_cache_shape[]`/`field_cache_slot[]` | lazily, once per `vm_run` call, grown to cover the whole chunk | `chunk_free` |
| `chunk_ensure_debug_hits` (debug-tools build only) | `debug_hits[]` | same as above, debug builds only | `chunk_free` |
| `func_register`/`chunk_add_function` | `Chunk.functions[]` (doubling) + that function's own `defaults` array | once per `function` declaration compiled | **Not freed** — same append-only, outlive-the-parser design as `shapes` (see README: struct/function registrations grow monotonically for the process's life, deliberately, so REPL persistence works) |
| `parse_struct`/`OP_DEFINE_STRUCT` handler | `Chunk.shapes[]` (doubling) + one `xmalloc(sizeof(Shape))` per declaration | once per `struct` declaration | **Not freed**, same reasoning |
| `chunk_add_import` | `Chunk.imported_modules[]` (doubling), each entry an owned `xstrdup` | once per `import` statement | `chunk_free` |
| `aer_module_load` (`aer_module.c`) | an entire second `Chunk`+`VM` pair (`xmalloc(sizeof(Chunk))`/`xmalloc(sizeof(VM))`) per distinct file-based import | first `import "path"` of a given file (cached — a second import of the same file is a no-op) | `aer_module_free_all()` only — an explicit, host-invoked teardown; not called during normal execution |
| `CallFrame.defers` (`vm.c`'s `OP_DEFER_PUSH` handler) | one `xmalloc(sizeof(DeferredCall) * MAX_DEFERS_PER_CALL)` **per call-stack depth slot**, lazily on that slot's first-ever `defer` statement | first `defer` executed at a given recursion depth | `vm_free` (frees all `VM_CALL_MAX` slots unconditionally, since a slot's buffer persists across reuse at that depth — not tied to any one call) |

### B. Runtime, GC-tracked — pool cell allocations (one per construction, always followed by `gc_maybe_collect` once the new value is reachable from a root)

| Opcode / call | Pool | Notes |
|---|---|---|
| `OP_ARRAY_NEW` (`[1, 2, 3]` literal) | `array_pool` | + `xmalloc`'d `items[]` sized to the literal (or 4 if empty) |
| `OP_DICT_NEW` (`{...}` literal) | `dict_pool` | + one `xmalloc`'d owned key copy per entry (`dictmap_put`) |
| `OP_STRUCT_NEW` / bare `Point(1,2)` call (`vm_call_builtin`'s struct-construction fallback) | `struct_pool` | header + fields in **one** cell (§2.1) — no separate items allocation |
| `OP_SLICE_GET`, array branch (`arr[a:b]`) | `array_pool` | + fresh `xmalloc`'d `items[]` for the sub-range copy |
| `vm_default_value` (an omitted array/hashtable-defaulted parameter, or an omitted struct field) | `array_pool` or `dict_pool` | a **fresh empty** container every time — deliberately never the stored default itself (Python's mutable-default-argument bug, avoided on purpose) |
| `build_function_value` (`parser.c`, **compile time**, not per-call) | `function_pool` | a function *value* is constructed once, when its declaration/literal is compiled — calling the function later never allocates one |

### C. Runtime, GC-tracked — string allocations (`aer_make_string`, always a fresh owned buffer; never pool-interned — see below)

Every one of these calls `aer_make_string`, which itself calls `pool_alloc(&string_pool)`:

- String concatenation (`OP_ADD`/`vm_binary_*` on two `TYPE_STRING` operands).
- `string(x)` / string interpolation (`vm_to_str`, via `OP_UNARY`'s `OP_TO_STR` case).
- `type(x)` (a fresh copy of the type-name string, never a pointer into static/pool data).
- `OP_SLICE_GET`, string branch (`s[a:b]`).
- Single-character indexing/iteration (`s[i]`, `for ch in some_string:`) — each character is its own fresh one-byte string.
- Hashtable key iteration (`for k in hashtable:` / `for k, v in hashtable:`) — each yielded key is a fresh owned copy, never an alias into the hashtable's own bucket storage.

**Deliberately never pool-interned** (no `chunk_add_pool` call) — measured 7x slower for 100,000
unique runtime-cast strings vs. 10 distinct ones, since interning would grow the pool and its
`name_index` forever, once per unique *value*, for strings that are used exactly once right where
they're created and never looked up by pool index again.

### D. When `gc_maybe_collect` runs, and when it deliberately doesn't

Called from the handful of opcode handlers listed in B/C above, immediately after the new value is
already reachable from a root (a register or the value stack) — never before. Every non-allocating
opcode (`OP_MOVE`, `OP_JUMP`, field get/set, global load/store, `OP_CALL`/`OP_TAIL_CALL`, all the
"primitive pass" raw-arithmetic opcodes, ...) calls it **zero** times — not a skipped check, a
genuinely absent one (§5.1). `collection.append()`'s items-buffer growth and `vm_call_builtin`'s few
allocating branches (`type()`, struct construction) follow the same rule, called from
`vm_call_builtin`'s own return path.

Suppressed entirely (`vm_gc_suppress`/`vm_gc_unsuppress`, a nesting counter) during a file-module's
own nested `vm_run()` while it's being loaded (`aer_module_load`) — that module isn't in any root
set yet, so a collection during that narrow window could sweep something the *importing* file
still needs.

---

## Contributing conventions

### Comments

Default to no comment. When one is genuinely needed, default to a single line. Reserve multi-line
comment blocks for cases with real, non-obvious complexity that truly can't be said in one line —
a hidden constraint, a subtle invariant, a workaround for a specific bug, or behavior that would
surprise a reader.

- Never restate what the code already says. Well-named identifiers make `i++; // increment i`
  always wrong.
- Never write a multi-line block just because a nearby function has one. Judge every comment on
  its own merits, not by matching the file's existing style.
- Never reference the change that introduced a piece of code ("added for the X feature", "changed
  during the Y refactor"). That belongs in the commit message, not the source — it rots the moment
  the codebase moves past that context.

This isn't a stylistic preference layered on top of the project — AER's own stated values are
light, fast, and easy to understand (see README), and a dense comment block works against "easy to
understand" exactly as much as an unnecessary abstraction does.

### Dead code

If a function, opcode, branch, or file is confirmed unused, delete it outright — don't comment it
out, don't leave a `// no longer used` marker, don't keep a re-export for compatibility. Verify
with a real search (callers, computed-goto dispatch tables, macro-generated references) before
deleting, since this codebase dispatches through function-pointer tables and packed opcodes that a
plain text search can miss.
