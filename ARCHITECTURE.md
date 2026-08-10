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
`AerPackedArray`, `AerTypedArray`, and `AerResult` — is allocated from one of eight fixed-size
**slab (arena) pools** (`pool.c`), not individual `malloc` calls. `pool_init(pool, elem_size,
elems_per_slab)` sets one up; `pool_alloc(pool)` hands back memory sized for exactly one cell:

- **Free-list first**: if a previous cell was freed, `pool_alloc` reuses it — per-slab, not
  pool-wide (each slab threads its own freed cells; see §5.8), so a reused cell's slab is always
  known without a search.
- **Bump allocation otherwise**: hands out the next cell in the current slab; when a slab is
  exhausted, one `xmalloc` grows a whole new slab (`POOL_INITIAL_SLABS = 4`, doubling), not one
  allocation per object.
- Returned memory is **uninitialized**, like `malloc` — the caller fills it in.

Eight pools exist (`vm_heap_init`, `vm.c`):

| Pool | Cell size | Elems/slab |
|---|---|---|
| `string_pool` | `sizeof(AerString)` | 256 |
| `array_pool` | `sizeof(AerArray)` | 256 |
| `dict_pool` | `sizeof(AerDict)` | 64 |
| `function_pool` | `sizeof(AerFunction)` | 64 |
| `struct_pool` | `sizeof(AerStruct) + MAX_STRUCT_FIELDS * sizeof(AerVal)` | 64 |
| `packed_array_pool` | `sizeof(AerPackedArray)` | 64 |
| `typed_array_pool` | `sizeof(AerTypedArray)` | 64 |
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
  GC pass. Frames beyond `call_depth` are dead — already returned, unwound by `lbl_return` — so
  bounding the scan can't under-collect.)
- The chunk's own constant pool (`Chunk.pool[]`) and every registered struct `Shape`'s field
  defaults — permanent roots, since a bytecode constant or a struct's declared default must never
  be collected out from under a later reference to it.

Each VM now collects only its own independent heap, against only its own roots — an imported
file-module or a spawned actor gets its own separate VM/Chunk/heap entirely, collected whenever
*its own* allocation count crosses *its own* threshold, not fanned out to from some other VM's
cycle. `gc_collect` (`vm.c`) takes exactly one `VM*` and never reaches into another one's state.

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
shared counter across all 8 pools) crosses `minor_gc_threshold` (default 2048, `aer_gc_configure`);
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

`Chunk.code` is a flat array of `uint32_t` words — **word-granular, not bit-packed to a wider
word**: every instruction is one or more 32-bit words, fixed at compile time per opcode (1-word,
2-word, ...), never a variable byte count. This replaced an earlier `uint64_t`-based bit-packed
scheme (dest + both RK operands crammed into one 64-bit word for the hottest opcodes) after
measurement on the Pi (32-bit ARM) found a real regression: a `uint64_t` is fetched as two 32-bit
register halves there, and any packed field straddling that boundary needs a shift-and-OR
reconstruction across both halves before it's usable — direct disassembly of `lbl_add` found
roughly 30 of its ~71 instructions were exactly this reconstruction tax. Word granularity fixes
this by construction: the prefetcher only ever sees one of a small, fixed set of strides (4, 8, or
12 bytes per instruction), the same class of mixed-width stream ARM's own Thumb2 and RISC-V's
compressed extension use without this problem.

Jump targets are **never** packed alongside anything else, uniformly, whether or not a particular
site is ever patched later: `patch_jump` does a blind word-overwrite at the target offset from
dozens of call sites, and keeping every patchable field in its own dedicated word is what lets that
stay a blind overwrite instead of a read-modify-write.

### 3.2 Instruction packing — a small, fixed field vocabulary

- **`PACK3`/`PACK2`/`PACK1`** (`op(8) | A(8) | B(8) | C(8)`, low byte first) — the general scheme:
  up to 3 plain 8-bit fields (register index, small count/tag) pack into one word alongside the
  8-bit opcode. A 4th small field, when unavoidable, spills into its own word.
- **`RK8`** (1 flag bit + 7 index bits, 128 registers/constants direct) — used only where two RK
  operands must share one word alongside a dest register (the `OP_ADD`..`OP_RSHIFT`/`OP_IN` family,
  over a third of all dispatches on `nbody.aer`, plus `OP_INDEX_GET`/`SET`, `OP_UNARY`, `OP_CAST`).
  `FRAME_REGISTERS = 128` means a register index fits RK8's 7 bits with zero headroom, by
  construction, not luck; a constant-pool index past 127 is spilled to a scratch register at
  compile time (`materialize()`/`OP_LOADK`, `parser.c`) rather than overflowing the encoding.
- **`RK16`** (1 flag + 15 index bits, 32767 direct) — used wherever an RK operand gets a whole word
  to itself or shares one with just one other 16-bit field; generous enough that no overflow/hoist
  path is needed in practice.
- **`PACK_2X16`** — two independent 16-bit fields in one word (e.g. a field's byte offset + an RK16
  operand sharing a word).
- **NAME/pool-index fields** — 16 bits when paired with one other 16-bit field, otherwise a full
  dedicated 32-bit word.
- **Every packing macro takes an explicit `(uint32_t)` cast before shifting**, not just a mask
  afterward — this was a real, separate bug found while narrowing an earlier scheme: a shift on a
  wider integer type is a genuine wide operation regardless of whether the useful bits fit
  narrower, and the compiler has no way to know the upper bits are always zero without an explicit
  narrowing cast telling it so.

### 3.3 The dispatch loop

Computed-goto dispatch (`vm_run_slice`, a `static void* dt[] = { [OP_ADD] = &&lbl_add, ... }` table
+ `goto *dt[cur_op]`), the standard technique for beating a `switch`-based bytecode loop (no bounds
check, no jump-table-then-branch — each opcode handler jumps directly to the next). The opcode
itself is masked out of the low 8 bits of the word (`& 0xFF`) — matching `PACK3`'s own 8-bit opcode
field.

`DISPATCH()` itself (the per-opcode macro) does the absolute minimum: read the next word, split
out the opcode, jump — no per-instruction GC check and no per-instruction error-flag check (see
§5.1 for how both stay off this path entirely).

**`CallFrame`** (`vm.h`) is the unit of call isolation: `registers`/`raw_ints`/`raw_reals` are
**bump-pointer bases into a shared, VM-level register stack** (not fixed inline per-frame arrays —
that was tried and reverted: it cost every single frame `RAW_REGISTERS_INT`+`REAL` slots regardless
of whether that function used any raw locals at all, `CallFrame` growing past a
`_Static_assert(sizeof(CallFrame) <= 96, ...)` regression guard left specifically to catch this
coming back), plus `frame_size`/`raw_int_frame_size`/`raw_real_frame_size` (this callee's own
compile-time peak), `return_ip`/`dest_reg`, `code_offset` (for stack traces), and
`tail_calls_collapsed`. `VM.call_stack` is a flat array of these (`VM_CALL_MAX = 64` frames);
`vm->registers`/`raw_ints`/`raw_reals` are **pointers repointed at the current frame** on every
call/return (not re-derived from `call_depth` on every access) — a tail call reuses the current
frame in place, so it's the one case that needs *no* repointing.

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
byte changes, a blind mask-and-OR, no re-encoding). At dispatch time the new arguments simply
overwrite the current frame's own registers 0..arg_count and `ip` jumps straight to the callee —
`call_depth`, `dest_reg`, and `return_ip` are untouched, so
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

### 5.8 Deleting a GC phase that did nothing

`pool_clear_marks` walked every live young cell in every pool at the top of every collection cycle,
clearing `POOL_MARKED`. It was 19.5% of cycles in a `log_processing.aer` profile — and provably a
no-op: `pool_sweep` already clears the mark bit on every surviving cell as it promotes it, and
`pool_alloc` zeroes a cell's whole state byte the moment it hands one out (fresh or reused). Between
those two, no live cell can enter a cycle already marked. Confirmed empirically before deleting it:
instrumented the clearing loop to count bits it actually flipped, ran all 21 tests, 6 examples, and
8 GC-heavy benchmarks (`binary_trees`, `log_processing`, `dict_bench`, `test_pool_churn`, ...) —
**0 stale marks across all 35 programs**. Deleted outright rather than kept as a defensive no-op.

While in the same code, also deleted a second, smaller inefficiency it was hiding: every `pool_*`
predicate (`pool_mark`, `pool_is_young`, `pool_is_freed`, `pool_is_remembered`,
`pool_mark_remembered`) took a `Pool*` it never used — the state byte lives in the cell itself, not
a side table. Every caller still paid to compute that unused pool, though: `value_is_young` ran an
8-case type switch and `remembered_pool_for` a 3-case one, on the hottest paths in the GC (all three
write barriers, `worklist_push`). Both switches are gone; the predicates take just `void* cell` now.

Found and fixed in the same pass: `gc_count_live_cells` listed 7 pools where `gc_collect` and
`gc_finalize_all_pools` listed 8 — `result_pool` was silently excluded from the `--memory-size`
ceiling check. All three call sites (plus `gc_collect`'s own sweep loop) now iterate one
`pool_table[]` of `{VmHeap field offset, finalizer}` pairs instead of each hand-listing all 8, so a
ninth pooled type can't again land in three places but not the fourth.

Verified: full four-suite regression + ASAN clean on Windows and the Pi (both untouched by
`pool_clear_marks`'s absence, as expected — it never did anything to break). Real wall-clock,
3+ runs each on the Pi, same benchmarks as the per-slab free-list work above:

| benchmark | before | after | |
|---|---|---|---|
| `log_processing.aer` | 3.27s | ~2.77s | ~15% faster |
| `binary_trees.aer` | 1.79s | ~1.48s | ~17% faster |
| `struct_array_scan.aer` | ~49.85s | ~40.7s | ~18% faster |
| `nbody.aer` | ~3.1s (unchanged) | 3.06s | no change (not GC-bound) |

`struct_array_scan.aer` is notable: it's a large, grow-only, low-churn pool (no `binary_trees`-style
churn at all), and it still won ~18% — confirming the cost was purely the pointless clearing scan
itself, not anything churn-related.

### 5.9 Struct field reordering

Every pool-managed value type carries `gc_state` (a 1-byte GC mark/generation byte, §2's per-cell
design) as its first field, `_Static_assert`'d to stay at offset 0. Left in whatever order the rest
of a struct's fields were declared, that lone leading byte opens up to 7 bytes of padding before the
next 8-byte-aligned pointer field — on every single cell, in every pool. Reordering each struct so
its small (bool/`uint16_t`/`unsigned int`) fields fill that gap instead, with `gc_state` still
first, recovers the waste at zero behavior cost: every field access still goes through `->name`, and
nothing in the codebase depends on field order beyond the offset-0 assert already enforcing itself.

Measured directly (`sizeof`, both targets) rather than assumed:

| type | x86-64 before → after | ARM (Pi) before → after |
|---|---|---|
| `AerString` | 24 → **16** | 12 → 12 (already optimal — no 8-byte pointer alignment gap on a 4-byte-pointer target) |
| `AerArray` | 56 → **48** | 40 → **32** |
| `AerPackedArray` | 32 → **24** | 16 → 16 (already optimal) |
| `AerFunction` | 40 → **32** | 28 → 28 (already optimal) |
| `AerDict` | 64 → **56** | 40 → **36** |
| `AerTypedArray` | 24 → 24 (not touched) | 16 → 16 (not touched) |

`AerTypedArray` isn't reorderable: its payload (1+4+4+8 = 17 bytes) already forces the theoretical
minimum (24, the next multiple of the pointer's 8-byte alignment) regardless of field order, so it's
left as-is rather than churned for no reason.

Verified: full four-suite regression + ASAN clean on Windows and the Pi. Wall-clock on
`log_processing.aer`/`binary_trees.aer`/`struct_array_scan.aer` measured neutral (within noise of
§5.8's post-`pool_clear_marks`-deletion numbers) on all three — expected, since this trades memory
footprint for cache locality on structures already small enough that the difference rarely crosses
a cache-line boundary in practice; the byte savings matter most for GC scan cost at scale (more live
cells fit in cache per pass) rather than any single access.

### 5.10 De-duplicating the specialized packed-array field opcodes

The 12 packed-array handlers among the shape-specialization opcode family (`OP_INDEX_FIELD_GET_RAW_
INT`/`REAL`/`INT32`/`FLOAT32`, their `SET` and `COMPOUND` counterparts) each independently
re-derived the same element address: type-check the container, type-check the index, resolve a
negative index, bounds-check it, then `pa->data + i * pa->shape->instance_bytes + foffset`. Measured
at ~90% pairwise-identical between the `{int,real}` and `{int32,float32}` variants of each -- the
same shape §5's `RAW_ARITH_*` macro family (`vm.c`) already deduplicates for the arithmetic opcodes,
just never extended to this specific family.

Factored into one `static inline __attribute__((always_inline))` helper, `vm_packed_raw_elem`,
returning the resolved element pointer (or `NULL`, having already reported the error, matching every
call site's existing `error(...); DISPATCH();` contract). `always_inline` rather than a macro because
the 12 call sites decode their opcode words differently from each other (GET/SET/COMPOUND each pack
their fields into different word layouts) -- only the address-resolution tail is identical, so a
shared inline function reads more clearly here than threading that variation through macro
parameters. Confirmed this doesn't reintroduce the real per-dispatch call cost §5.6's `vm_binary`
split explicitly measured against: `objdump -d` shows zero `call`/`bl` instructions to
`vm_packed_raw_elem` in either the x86-64 or ARM binary -- fully inlined at every site, as
`always_inline` requires.

~108 lines removed from `vm.c` (3948 → 3854), the single largest concentration of duplicated code in
the file. Verified: full four-suite regression + ASAN clean on both targets (`test_packed_arrays.aer`
specifically). Wall-clock on `nbody_large_packed.aer` (the heaviest user of this opcode family)
measured statistically indistinguishable before/after (~4.2-4.3s either way, 3 runs each) — expected
for a change verified codegen-identical rather than assumed to be.

### 5.11 Folding the parser's 31+ globals into one struct

`parser.c` had 31 separate file-scope mutable statics (the register allocator, variable tables,
shape-specialization tracking, forward-reference bookkeeping, loop-context stack, ...) plus a
hand-mirrored `ParserState` struct and ~75 lines of manual field-by-field save/restore, used around
a nested compile (a module import, or a lazy shape-specialization recompile triggered mid-execution
by `lbl_call_spec`, vm.c). Adding new parser state meant declaring it in three places kept in sync
by hand — the exact pattern already responsible for real bugs in this codebase (`var_kind` init,
`branch_depth`, a `var_slot` bypass, all in the project history).

Folded every one into one `Parser` struct (`static Parser P;`), with `parser_save_state`/
`parser_restore_state` now a plain struct copy rather than a field-by-field list. This required
auditing something non-obvious first: several fields (`shape_sensitive_param`, `current_param_count`,
`alias_source_param`, `reg_known_shape`, `reg_known_element_shape`, the `last_plain_index_*` trio,
`any_compile_error`) were *not* in the original hand-written `ParserState` — deliberately excluded,
not an oversight. Verified each is safe to fold into a blanket save/restore anyway: the whole
shape-specialization group is unconditionally reset at every `parse_function_body` entry
(confirmed at its own reset block), `any_compile_error` is unconditionally reset at every `parse()`
entry, and the nested-compile path this snapshot exists for calls neither of those functions in a
way that would ever observe a stale carry-over value either way. So a full struct copy is
behaviorally identical to the original selective one, not a silent behavior change.

Mechanical transform, not hand-editing ~35 identifiers across ~600 call sites: scripted the rename
(python, word-boundary regex per identifier) into a scratch copy, let the compiler catch every
missed reference (each surfaces as a plain "undeclared identifier" error — a strong safety net for
this specific class of mistake), fixed the real bugs it found (`Parser`'s own definition needed to
move before the register-allocator functions that use it, which meant relocating the small,
self-contained `PendingCall`/`LoopContext` typedefs earlier in the file too; `parser.h`'s existing
`typedef struct ParserState ParserState;` forward-declaration meant `ParserState` needed to stay a
real struct tag, not become a bare alias for `Parser`), then manually swept for comments left
orphaned above their now-removed declarations (several paragraphs that used to precede a
`static int foo;` line and now duplicated the same explanation already in `Parser`'s own field
comments).

Verified: full four-suite regression + ASAN clean on Windows and the Pi, `test_shape_specialization.aer`
and `test_functions.aer` specifically (the two most exercising nested-compile save/restore and
forward-reference bookkeeping). Wall-clock on `nbody.aer` (heaviest user of specialization
recompiles) measured unchanged (~3.0-3.3s, matching this benchmark's existing noise band) — expected,
since this is a compile-time correctness/maintainability change, not a performance one.

### 5.12 Keeping three cold, single-call-site functions out of `vm_run_slice`'s own frame

`aer_host_call` (`aer_host.c`), `aer_actor_module_call` and `aer_scheduler_module_call` (both
`stdlib/`) each declare a local `AerVal[VM_STACK_MAX]` (4KB on this build: `VM_STACK_MAX` is 256,
`AerVal` is 16 bytes) to hold a copy of the popped call arguments. Each is called from exactly one
site in `vm.c`'s `lbl_call_module` — the single-call-site shape GCC's inliner favors regardless of
callee size, since inlining a function called from only one place can't increase code size. Under
`-flto`, it took the bait on all three at once: disassembling `vm_run_slice` showed a ~30KB stack
frame (`sub sp, sp, #30336` plus a further `#44`), confirmed via `-fstack-usage` to be ~7KB in a
non-LTO build of the same function alone — the other ~23KB was these three call targets' own
locals, folded in by the LTO backend at link time.

The dispatch loop's own hot state (`ip`, the `registers`/`raw_ints`/`raw_reals` pointers, `op_word`)
already lives in real registers, not this frame (see §3.3, §5.4's hoisting work) — but the frame
still exists as real stack memory the CPU touches on entry, and at ~30KB it's comfortably bigger
than this target's 32KB L1 dcache, all to serve three opcodes (`io`/`actor`/`scheduler` module
calls) that are never on `nbody`'s hot path at all.

Marking all three `__attribute__((noinline))` dropped `vm_run_slice`'s frame back to ~17.5KB (the
three 4KB arrays gone; something smaller still folds in) at the cost of one real `bl` per actual
`io`/`actor.call`/`scheduler.add` call — none of which are hot paths, each already doing a linear
host-function scan or a `memcpy` of comparable cost. Measured on the Pi, `nbody_large_packed_narrow.aer`,
8 runs each side: **identical instruction count** (~12.897B both sides — same work, confirming this
is purely a locality effect, not a codegen change to the hot loop itself) and **~9-10% fewer cycles**
(8482M → 7671M average). Swept `binary_trees`, `log_processing`, `dict_bench`, `struct_array_scan`,
`sieve`, `mandelbrot`, and `fib_bench` for regressions — all within 1-3%, inside this Pi's documented
per-run cycle-counter noise band, including `log_processing.aer` and `test_actor.aer`/
`test_scheduler.aer` which actually exercise the now-noinline'd functions. Full four-suite regression
passed on Windows, including the exact-source-line error assertions in `test-embed`.

The other 9 single-call-site module-dispatch functions (`aer_math_call`, `aer_string_call`,
`aer_collection_call`, ...) have the exact same shape and were tried the same way — but this is
**not** a blanket win, and is deliberately *not* applied further. `aer_math_call` is a genuine
hot-path callee for any numeric script (`math.sqrt` inside `nbody`'s own inner loop), so forcing it
out of line regressed `nbody_large_packed_narrow.aer` by ~2-6% instead of improving it, and — more
notably — partially reverting just that one function did not cleanly restore the prior measurement:
LTO's whole-program inlining budget is a coupled, non-monotonic decision surface, not a per-function
switch that composes the way `-O` flags do. The three functions actually committed above were chosen
specifically because they're cold on *every* current benchmark (nothing here exercises `io`, `actor`,
or `scheduler` in a loop) — extending this lever further requires checking, per function, whether
anything actually calls it hot, not applying it uniformly to everything with the same local-array shape.

### 5.13 Fast integer-to-string, replacing `snprintf("%lld", ...)`

`vm_to_str` (string interpolation and `+`-with-a-string) and `vm_format_value` (`print()`, JSON
encoding) both rendered every `TYPE_INTEGER` via `snprintf(buf, n, "%lld", ...)`. Profiling
`dict_bench.aer` (200k inserts + 200k lookups, each doing `"key_{i}"`) with `perf report` found
`__vfprintf_internal`, `_itoa`, `__vsnprintf_internal`, and `_IO_default_xsputn` — glibc's
format-string parser and locale-aware digit conversion — at a combined **~14% of total cycles**, for
what is, underneath all that generality, just "write these decimal digits."

Replaced with `aer_format_int` (`value_format.c`): a direct digit-extraction loop, no format-string
parsing, no locale lookup, `LLONG_MIN` handled via the standard "negate through unsigned" idiom to
avoid signed overflow. Used at all three call sites (`value_format.c`, `vm.c`'s `vm_to_str`,
`aer_json.c`'s encoder). Verified against `0`, `-1`, and both `INT64_MIN`/`INT64_MAX` before
measuring anything.

Measured on the Pi, 5 runs each side: `dict_bench.aer` — **~17% fewer instructions** (1299.9M →
1078.5M), **~13% fewer cycles** (974.5M → 846.0M); `log_processing.aer` — ~6.5% fewer instructions,
~6.2% fewer cycles. Every non-string-formatting benchmark (`nbody*`, `binary_trees`, `sieve`,
`mandelbrot`, `fib_bench`, `struct_array_scan`) measured flat, as expected — this path is never on
their hot loop. Full four-suite regression passed on both Windows and the Pi.

### 5.14 Bounding the remembered-set card scan to the actually-dirtied range

`gc_collect`'s minor-cycle replay of `REMEMBERED_ARRAY`/`REMEMBERED_DICT` entries (§ card marking,
`gc_barrier_array`/`gc_barrier_dict`, `gc.c`) is documented as turning "a pure-growth 'build a huge
array via many appends' pattern into true O(n) total instead of O(n^2)" — true for which *values*
get pushed to the mark worklist, but not for the scan that finds them: the loop walked
`[0, dirty_cards_bytes)` every single cycle, and the post-scan clear `memset`'d the same full range,
both regardless of how few bits had actually been set since the last clear. That's still O(current
container size) of pure bit-checking per minor cycle even when exactly one new element was appended
since the last one — the same shape of bug the card scheme was built to eliminate, just with a
cheaper per-byte constant than the original whole-array rescan.

Fixed by tracking `[dirty_min_byte, dirty_max_byte)` alongside the existing bitmap (`AerArray`/
`AerDict`, both structs) — `mark_card_dirty` (`gc.c`) widens the bound on every write, the scan and
the clear both use it instead of `[0, dirty_cards_bytes)`, and it resets to empty
(`dirty_min_byte = (unsigned int)-1, dirty_max_byte = 0`) after every replay. Verified against
`test_card_marking.aer` and the full four-suite regression on Windows and the Pi.

Measured impact on current benchmarks is **negligible** — `dict_bench.aer` (200k inserts) and
`struct_array_scan.aer` (2M appends) both measured flat to within noise. Root-caused why:
`struct_array_scan.aer`'s construction phase (`perf report`, ~79% of cycles in `gc_collect`) turns
out to be dominated by a *different*, unfixed O(n)-per-cycle cost one layer down — see §6's new
entry. `dict_bench.aer`'s single dict tops out at 200k entries, too small for the now-fixed
per-cycle card-scan cost to have been a visible fraction of its total even before this fix. Kept
anyway: the code's own comment claimed this was already O(n) total, and it wasn't -- a container
with sustained post-promotion append/update churn at larger scale, or with a smaller minor-GC
threshold triggering more frequent cycles, would have hit the same quadratic wall this closes.

### 5.15 Threading pool_sweep's minor pass through only the still-young slabs

`slab_young_count[i] == 0` already let `pool_sweep` skip a fully-old-or-free slab's expensive
per-cell scan in O(1) (§5.13's Pass 3) -- but the *outer* `for (i = 0; i < p->slab_count; i++)` loop
still visited every slab index, every minor cycle, just to read that one flag. With slab count
growing alongside a large pure-append workload (`struct_array_scan.aer`'s 2M-particle `struct_pool`,
~2000 slabs by the end of construction), that's O(slab_count) of pure flag-checking per cycle even
once the large majority of those slabs are fully promoted and have nothing left for a minor pass to
do -- the exact shape of bug §5.14 fixed one layer up, for the remembered-set card scan.

Fixed the same way §5.14's problem area is structured, but for slabs instead of cards: a doubly-
linked "slabs with `young_count > 0`" thread (`young_slab_prev`/`young_slab_next`/`young_slab_head`,
pool.c/pool.h), so a minor `pool_sweep` walks only the slabs that still have >=1 young cell instead
of every slab index. Doubly-linked (unlike `slab_free_list`/`free_slab_head`, which only ever pops
its own head) because removal can happen to any slab in the thread, not just the head. `pool_alloc`
links a slab back in the moment its count crosses 0 -> 1 (a cell `pool_sweep` freed getting reused,
always "born young"); `pool_sweep` unlinks it the moment a sweep drains it back to 0 -- the same
bidirectional add/remove care §6 had already flagged this would need, not a simpler one-directional
watermark. A major (non-`young_only`) sweep still walks every slab, since old cells matter there too
and the young thread only ever tracks young ones.

Verified: full test suite (Windows + Pi, including `test_pool_churn.aer` and `test_card_marking.aer`,
which specifically exercise free/reuse churn through this exact machinery), ASAN clean on the Pi (no
leaks or errors attributable to this change -- the one pre-existing `main.c` `real_argv` leak
LeakSanitizer flags on every run, touched or not, is unrelated argv-parsing memory freed by process
exit), and a 300-iteration fuzz run with zero crashes (2 hangs, both confirmed benign fuzzer-mutated
loop-termination breaks unrelated to this change, one of them ironically inside
`test_pool_churn.aer` itself: `i += 1` mutated to `i += -100000` inside `for i < 5:`, so the
condition never goes false).

Measured on the Pi, `struct_array_scan.aer` (2M particles, 4 repeated runs): **branch-misses fell
11-33x** (176.5M -> 5.3-29.4M per run, consistent with turning an unpredictable "which slabs are
old" branch into a tight, always-taken linked-list walk) and instructions fell a real but small
~0.5% (73.78B -> ~73.37B, less outer-loop bookkeeping). Cycles and wall-clock stayed flat within this
Pi's own documented run-to-run noise band (74.3-77.2B cycles / 41.0-43.1s, before and after both
inside that range) -- §5.13's per-cell skip had already captured the large majority of the available
win here, so the remaining outer-loop branch cost, while real and now measurably gone, wasn't a big
enough fraction of the total to move wall-clock on this specific benchmark. Kept anyway: it's a
genuine algorithmic-complexity fix (O(slab_count) -> O(live young slabs) per minor cycle) that will
matter more as slab counts grow further, and the branch-miss reduction is real, large, and
consistent across every run.

### 5.16 Interpreter code size is not an instruction-cache problem (measured, not fixed)

`vm_run_slice` compiles to ~55KB on x86-64 and ~45KB on the Pi, against a 32KB L1i — roughly 4x
Lua's `luaV_execute`, and the obvious next lever after §5.12 is to push cold opcode bodies out of
line the way that section pushed cold *callees* out of the frame, shrinking the function toward
something that fits.

Measured first, and the premise does not hold. L1 instruction-cache miss rates across the suite:

| benchmark | instructions | L1i misses | miss rate |
|---|---|---|---|
| `nbody` | 8.10B | 1.50M | 0.05% |
| `sieve` | 6.96B | 0.29M | 0.02% |
| `fib_bench` | 0.78B | 0.06M | 0.03% |
| `dict_bench` | 0.79B | 0.27M | 0.10% |
| `struct_array_scan` | 47.10B | 2.46M | 0.02% |

The function's *size* is not its *working set*. With 157 opcodes, any one benchmark dispatches a
few dozen, so the bytes actually fetched are the hot labels plus the jump table — the cold labels
occupy address space that is never touched, and the hardware never pays for them. Outlining them
would add a real `bl`/return per cold dispatch to relieve a stall that is already down in the
noise, so it is deliberately not done. Note this is the opposite conclusion from §5.12, and for a
concrete reason: that section's win was a **data**-side frame the prologue touches unconditionally
on every call, not an instruction-side fetch that only happens if the code actually runs.

The same sweep pinned this Pi's cycle-counter noise band precisely, which §5.12 estimated at 1-3%:
running `tools/bench.py` with base and head set to the *same* ref reports per-benchmark cycle
deltas from -0.92% to +1.26%, while instruction counts over the same runs agree to within 0.03%.
Any cycles-based claim below about 1.5% on this hardware is unfalsifiable — use instruction counts
as the gate, and treat `--event cycles` as a coarse sanity check only.

### 5.16a Building an interpolated string once, and fusing a comparison that ends a condition

Two changes that came out of comparing against Luau and LuaJIT's interpreter on equal-length
benchmarks, both targeting dispatch count rather than the cost of a dispatch.

**`OP_INTERP`.** `"key_{n}"` compiled to `OP_LOADK` + `OP_TO_STR` + `OP_ADD`: three dispatches and
two `AerString`s, the second garbage the moment the lookup consuming it finished. An N-part
interpolation was worse than linear -- a left-fold of `OP_ADD` means N-1 allocations and O(N²)
copying. The opcode takes its parts as trailing RK16 words, so a constant segment stays a pool
constant needing no load, and a non-string part is formatted straight into the result rather than
through a throwaway string. `lookup_table_bench`'s key went from four dispatches to two.
`dict_bench` -30.7%, `log_processing` -18.4%, `lookup_table_bench` -17.9%, `small_dict_bench`
-7.4%, everything else within 0.02%. The builder is `noinline` -- its scratch would otherwise land
in `vm_run_slice`'s frame, which §5.16b explains the cost of.

**Fusing a comparison that merely *ends* a condition.** `emit_cond_jump_if_false` only fused when
the comparison *was* the condition -- one word, or two with a constant load before it. Anything
computed first missed, which is most real conditions: `p * p < n`, `x * x + y * y > 4.0`. It cannot
just inspect the last word, because instruction lengths vary and reading backwards can land
mid-instruction, so the two emitters that produce a comparison now record where they put it. Worth
**-5.48% on sieve for no new opcodes**. Adding the four real compare-and-branch opcodes (only the
int forms existed, so a float loop paid three dispatches where an int loop paid two) is a further
-8.75% on mandelbrot.

Also worth knowing: `aer_format_int` divided by 10 in 64-bit arithmetic once per digit, and 32-bit
ARM has no 64-bit divide, so each digit was a libgcc call. Narrowing to `unsigned int` when the
value fits -- as `aer_mod_int64` already did -- is another 4-4.6% on all four string benchmarks.

Left unclosed: `small_dict_bench` is level with Luau (2.58s against 2.60s, medians of 7) but still
1.20x behind LuaJIT's interpreter, and what is left is dict churn
-- 3.3M short-lived four-key dicts, with `pool_alloc`, `hashtable_put_hashed`, `hashtable_free` and
`hashtable_key_dup` together about 30%. The obvious fix, borrowing constant-pool bytes for literal
keys instead of copying them, has been attempted and reverted twice (see the `borrow`/`Retry dict
borrowed keys` commits); the second attempt fixed both flaws the first was reverted for and still
did not land. A third attempt needs a new idea, not a retry.

### 5.16b Why `lbl_call` cannot be micro-optimized (measured, three ways)

`fib_bench` runs 134.4M dispatches for 8.26B instructions -- 61.5 instructions per dispatch, against
`mandelbrot`'s 22.0 on the same interpreter -- so the call path looks like obvious low-hanging
fruit. Three attempts, all reverted, and the reasons are worth keeping.

**Reading back what the call just wrote.** `lbl_call` computed the callee's base pointers, stored
them into the CallFrame, then loaded them straight back out through `vm->call_stack[vm->call_depth]`
to refresh `vm->registers` and the hoisted locals. It also loaded `caller->registers`, which is by
definition the hoisted `registers` local. Removing both round trips regressed **ten** benchmarks --
`mandelbrot` +6.3%, `nbody` +4.7% -- while helping only `fib_bench`.

`mandelbrot` spends 0.22% of its dispatches on calls (1,036,800 of 468,213,512), so a change
confined to `lbl_call` cannot cost it 6% through its own work. A control confirmed it is not generic
codegen churn either: swapping two independent decodes in a cold label moves every benchmark 0.00%.

The mechanism is live ranges. `registers`/`raw_ints`/`raw_reals` are hoisted for the whole function,
and `lbl_call` previously did not touch them -- it went through `caller->...` instead, leaving them
**dead** across the entire call sequence. Referencing them there extends their live ranges over all
of it, and in a function under this much register pressure that reshapes allocation for every other
label. The "redundant" load is what keeps the hot arithmetic paths' allocation good. Reloading from
memory is cheaper globally than reusing a value already in a register.

**Hoisting the frame sizes too** (so `caller->registers + caller->frame_size` needs no loads at all)
is the same trap one step further: four more live locals, and it cost `mandelbrot` 6.3% and `nbody`
4.6% to buy `fib_bench` 2.9%.

**Biasing the RK8 register-vs-constant branch.** `tst.w fp, #0x800000` inside `vm_rk_ptr8` is ~9.4%
of `fib_bench`'s cycles across three sites, so `__builtin_expect` toward the register case looked
free. It did exactly what it was meant to -- branch misses fell 23%, 38.8M to 29.9M -- and cycles
went **up** 2.44%. The misses were not the binding constraint, which also disposes of the theory
that fib is misprediction-bound: at IPC 1.62 with 8.26B instructions, it is instruction-bound.

The consequence for anyone picking this up: the remaining lever on call-heavy code is **fewer
dispatches**, not a cheaper `lbl_call`. Seven dispatches per `fib` call (compare-and-branch, two
subtracts, two calls, add, return) is the number to attack, and only through fusion.

### 5.16c Constants as RK operands: where it pays, and where fewer instructions ran slower

`OP_LOADK` exists only to copy a pool constant into a register because some consuming opcode insists
on one. It was the 11th hottest opcode in the suite, 60M dispatches, and essentially all of it was
that kind of staging. Two attempts to remove it, with opposite outcomes.

**Comparison bounds (landed).** The boxed operand of `OP_RAW_*_{INT,REAL}_BOXED` and its eight fused
`_JUMP_IF_FALSE` forms was a plain register, so `x*x + y*y > 4.0` reloaded `4.0` every iteration --
25.3M times in `mandelbrot` alone. Widening that operand to RK8 lets the compare address the pool
entry directly: `mandelbrot`'s `OP_LOADK` count falls from 25.3M to 1442, **-1.61% instructions and
-4.25% cycles**, `dict_bench` -0.52%, `small_dict_bench` -0.42%, nothing regressed past 0.11%. Note
`pack_rk8` masks the index to 7 bits, so a pool index past 127 has to decline and fall back to the
`OP_LOADK` spill or it would silently alias another constant. Registers are safe without a check
because `FRAME_REGISTERS` is 128.

**Dict-literal operands (reverted).** The same reasoning applied to `OP_DICT_NEW`, whose contiguous
register run cost one `OP_LOADK` per constant key or value -- five per iteration in
`small_dict_bench`, 16.5M dispatches, 25% of that benchmark's total. Making the operands trailing
RK16 words (the shape `OP_INTERP` uses) removed all of them and cut the literal's build sequence
from 12 dispatches to 7.

It measured **-0.42% instructions and +1.87% cycles**, and 2.58s -> 2.64s wall-clock, on the one
benchmark it was built for. Reverted. The trade was five predictable dispatches reading a contiguous
register block for eight scattered 16-byte pool loads inside a single opcode, plus a serial `READ()`
chain through the instruction stream; the pool entries for a dict's keys are not adjacent the way
staged registers are.

The methodological point is the more important one. Instruction count is this project's primary gate
because it repeats to 0.03% while cycles spread ±0.4% and wall-clock is worse -- but here it moved
the wrong way relative to real time, because the change traded instruction *count* for instruction
*locality*. A change that relocates memory accesses rather than removing work needs a cycles and
wall-clock confirmation before it is believed, with a ref-against-itself control run to establish the
noise floor first.

### 5.16d What a new opcode actually costs, measured twice on one change

`OP_INDEX_GET_INTERP` formats `dict["key_{n}"]` into a stack buffer and probes with the bytes
instead of allocating an `AerString` to hash once and drop -- `hashtable_get_hashed` already takes
raw bytes. **`dict_bench` -27.8% instructions, `lookup_table_bench` -16.7%**; in wall-clock 1.61s ->
1.31s and 2.37s -> 2.01s.

Getting there cost two measurements that matter more than the feature.

**A local array in a label is paid for by every opcode.** The first version held
`AerVal parts[INTERP_MAX_PARTS]` in the label -- 256 bytes onto `vm_run_slice`'s frame. `mandelbrot`
went **+4.52%** and `nbody` +2.65% *in instructions*, on an opcode neither can reach, through
register pressure in a 45KB function. Reading the RK16 words straight from the code array and giving
both helpers their own scratch removed every one of those eight regressions and left the wins intact.
This is the same reason `vm_interp_build` is `noinline`, and it is worth restating: **any scratch
declared in a label is a tax on the whole interpreter.**

**Where a label sits is worth percent.** Even with the frame fixed, `mandelbrot` costs **+3.1%
wall-clock (2.54s -> 2.62s, medians of 7) at an identical instruction count** -- pure code placement.
Moving the new label past the raw-compare block it had displaced recovered about a third of that. The
rest is the price of the opcode existing at all.

So the per-opcode cost is real, but it is *placement*, not size, and not the two things it is usually
blamed on: 5.16 measures the icache miss rate at 0.02-0.10%, and 5.16b measures a deliberate 23% cut
in branch misses making cycles *worse*. That matters for the standing question of whether to delete
the ~76 dispatchable opcodes no benchmark reaches (the int32 and bounds-checked corners of the
field-access matrix, and the `>`/`>=` halves of the raw compare families). Deleting them is a
maintainability argument, which is a real one -- but it is a layout lottery, not a directed
optimization, and anyone doing it should expect to measure a shuffle, not a speedup.

### 5.16e The call path, a fourth time; and how many opcodes are actually redundant

**`fib_bench` costs ~277 instructions per call** (8.26B instructions, 29.86M calls, 4.5 dispatches
each). `perf annotate` puts it in frame maintenance rather than dispatch: `ldr r0, [sp, #44]`
reloading `vm` at 4.73%, 16-byte `ldmia` copies at 4.43% and 2.27%, spills at `[sp, #148/296/328]`,
and four `str.w [r3, #732..744]` writing the `vm->` mirror. `CallFrame` is 11 fields / 44 bytes, and
**six of the eleven exist only to maintain the raw int/real side-stacks** -- which `fib` never uses.

**Removing the mirror writes did not work (attempt 4).** `lbl_call` and `lbl_return` each wrote
`vm->registers/raw_ints/raw_reals` and immediately read them back: 6 stores and 12 loads per
call/return pair. Nothing needs them mid-slice -- the GC walks `call_stack[0..call_depth]`
(`mark_vm_roots`), not the mirror. Deleting them bought `fib_bench` **0.36%** and cost eight other
benchmarks 0.7-4.7% (`mandelbrot` +4.58%, `nbody` +4.69%, `struct_array_scan` +3.04%). The stores
were never the expense; the compiler already handled them, and removing them perturbed register
allocation across the whole function. Reverted.

Two things survived it. `vm_resolve_field` read `vm->registers` on **every field access** and now
takes the register base as an argument -- the indirection `vm_rk_ptr16` was already fixed for.
And a portability note: gcc 16.1 **ICEs** (`tree-if-conv.cc`, `factor_out_operators`) if
`vm_run_slice`'s hoisted locals are live across a call at a return point, so `vm_publish_frame` reads
the live `CallFrame` instead of taking them as arguments.

**What did work was the address arithmetic, not the instruction count.** Attributing `fib`'s profile
per handler (segment `perf annotate` on the dispatch `bx`) put **36.3% of cycles in `lbl_call` alone,
at 129 instructions**, and 19.3% in `lbl_return` at 43. Reading those 129 instructions showed what
they were actually spending it on, and it was not the frame writes:

- `CallFrame` was **44 bytes, not a power of two**, so `call_stack[depth]` compiled to a multiply --
  two `mla`s per call -- instead of a shift. Rounding it up to 64: **fib_bench -3.26% instructions,
  binary_trees -1.23%**, nothing else moved.

  **Correction, found later by UBSan.** This was first done with `_Alignas(64)` on the first member,
  which gets the size but also raises the type's *alignment* -- and that requirement propagates to
  `struct VM`. `aer_module.c` allocates module VMs with plain `xmalloc` (8-byte aligned on 32-bit
  ARM), so **every module VM was undefined behaviour**, and `VM` is a complete type in the public
  header, so any embedder calling `malloc(sizeof(VM))` hit the same trap silently. It now pads via an
  anonymous union with a `char[64]` member: same 64-byte size, alignment left at the members' natural
  8. That costs **fib_bench +0.87%, binary_trees +0.29%** of the win back, because the compiler can no
  longer assume the frame array is 64-byte aligned. Paid deliberately -- a public struct that is UB to
  heap-allocate is not a trade worth 0.87%.
- A Thumb-2 `ldr` reaches a **12-bit displacement**, and `registers`/`raw_ints`/`raw_reals`/
  `call_depth` sat past 10000 bytes into `VM`, behind `heap`, `stack[256]` and `call_stack`. Moving
  just those four (16 bytes) to the front: another **-2.99% and -1.31%**.

- `stack[VM_STACK_MAX]`, the 4096-byte stdlib scratch channel **nothing on the call path touches**,
  still sat in front of `call_stack` and pushed every `CallFrame` field back out of the window.
  Moving it behind: another **-4.62% and -1.46%**, and `lbl_call`'s materialized-constant count goes
  to zero.

Together: **fib_bench -10.5% instructions, `binary_trees` -4.2%**, with `lbl_call` down from 129
instructions to 107 and `lbl_return` from 43 to 36. Nothing else in the suite moved past 0.12%.

Note the shape of the displacement budget, because it bites both ways: moving `call_stack` *itself*
to the front bought `fib_bench` a further 5.79% and cost seven other benchmarks 0.4-3.8%. At 4096
bytes it fills the entire 12-bit window on its own and pushes `heap` -- which every allocating opcode
touches -- back out. Reverted. The rule that worked: **small hot scalars first, then the structures
the dispatch loop indexes, and every big cold array last.**

What remains in `lbl_call` is structural: the 16-byte `AerVal` argument copy (`ldmia`, 2.4%) and `vm`
reloading from a stack spill in `lbl_return` (4.0%). Both are consequences of a 16-byte value in a
C-compiled dispatch loop, which is the deferred value-representation work. Beyond those, the lever is
fewer dispatches per call -- fusion, a new opcode, priced at about 3% by 5.16d.

**How much of the opcode table is actually redundant: four opcodes, not 76.** A census across
`bench/` + `tests/` (per-opcode dispatch counts from the debug build) shows 76 of 153 dispatchable
opcodes never fire in any benchmark. That number is misleading. Most are feature-bearing:
`OP_TAIL_CALL`, `OP_DESTRUCTURE`, `OP_SLICE_GET` are language features the benchmarks happen not to
use, and the int32 and bounds-checked corners of the field-access matrix are the symmetric halves of
features that *are* hot (`OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED` alone runs 49.4M times in
`nbody_large_packed_narrow`). Deleting the int32 half would not fall back to a narrow path -- there
isn't one; `OP_FIELD_GET_RAW_INT` would read 8 bytes out of a 4-byte field -- so the parser would
have to emit the generic boxed path, making `field: int32` a performance trap. That is a capability
regression dressed up as a cleanup.

What was genuinely redundant: the **raw-vs-raw `>` and `>=`** compares. Both operands are slots of
the same kind, so `a > b` is `b < a` with the slots swapped, exactly the flip
`try_emit_cmp_raw_boxed` already does for `boxed OP raw`. Removing `OP_RAW_GT_INT`,
`OP_RAW_GTE_INT`, `OP_RAW_GT_REAL` and `OP_RAW_GTE_REAL` costs nothing and loses nothing: **153
opcodes, down from 157**, instruction counts flat to 0.03% everywhere. (The `_BOXED` forms keep
theirs -- a raw slot and a boxed value are not interchangeable.)

And the performance answer, measured: instructions moved 0.00-0.03% on every benchmark, while
**cycles moved `mandelbrot` -3.88% and `nbody` +2.21%**. Removing opcodes is a layout shuffle of
roughly the same amplitude as adding one, in either direction. Trim for size and comprehension, which
are real goals; do not trim expecting speed, and do not trim anything that carries a capability.

### 5.16f Why the raw slots cannot share register_stack (a GC invariant, then a measurement)

`lbl_call` maintains three bump pointers -- `registers`, `raw_ints`, `raw_reals` -- with three
frame sizes, six of `CallFrame`'s eleven fields. Measuring the compiled handler put **4.53% of
`fib_bench`'s cycles** in exactly those four raw-side stores and loads, so folding the raw slots into
`register_stack` and deriving the two bases looked like a clean win: one bump pointer, four fewer
fields, and better locality for `nbody`-shaped code that touches registers and raw slots in the same
loop.

It segfaults, and the reason is worth writing down because nothing else records it.
**`mark_vm_roots` (gc.c) scans `[registers, registers + frame_size)` unconditionally**, including
slots the callee has not written yet -- its own comment says "zero-init decodes as harmless
TYPE_NULL". That is only true because `register_stack` has *only ever held valid AerVals*. Interleave
raw `int64`/`double` words into the same bank and a later frame's register range can overlap a dead
frame's raw area, so the collector reads a `double` as a tagged pointer. `tests/test_gc_raw_frames.aer`
was written first, for exactly this, and caught it on the first run.

The invariant can be restored by clearing a frame's raw area when it pops -- confirmed, it fixes the
crash -- but that leaves the same hole on every error unwind and yield, since those skip `lbl_return`
entirely. And it does not pay regardless: **`fib_bench` +2.02%, `binary_trees` +3.58%**, nothing
improved. Deriving the two bases costs more arithmetic on every call *and* return than the four loads
it removes; a load from an already-hot cache line is cheaper than recomputing an address.

So the three separate stacks are load-bearing, not an oversight. The four raw-side fields stay.

### 5.16g Non-PIE is worth 0.3-4.3%, and is deliberately not taken

A PIE build reaches the computed-goto label table PC-relatively, so **every dispatch** pays
`ldr rN,[pc,#imm]; add rN,pc` on top of the table load itself -- 4 ARM32 instructions where a
fixed-address build needs 2. Building with `-fno-pie -no-pie` improves **every benchmark in the
suite**: `binary_trees` -4.26%, `sieve` -4.01%, `fib_bench` -3.63%, `lookup_table_bench` -2.65%,
`struct_array_scan` -2.09%, `log_processing` -1.75%, `dict_bench` -1.65%, `small_dict_bench` -1.22%,
`mandelbrot` -0.60%, `nbody` -0.33%. Nothing gets worse. It is the only change measured here that
helps all ten.

It is still not the default, and there is no build switch for it. Two reasons, and the second is the
binding one. It gives up ASLR on the executable image, which is a real hardening loss for a runtime
that executes other people's code. And **both interpreters this project benchmarks against ship
PIE** -- `luajit` and `luau` are each `pie executable` on the test machine -- so taking it would
report a build-flag difference as an interpreter win. Same rules, or the number means nothing.

Recorded because the underlying cost is real and worth knowing: on a computed-goto interpreter,
position independence is a per-dispatch tax, not a one-off.

### 5.16h Where the stack traffic actually is, and what did not reduce it

Cycle-weighted inside `vm_run_slice`, loads are 64-86% and stores 7-17%, of which **stack spills are
17-24%**: `fib_bench` 24.3%, `lookup_table_bench` 23.0%, `nbody` 17.5%. The load share itself is not
a defect -- an interpreter is a load-dispatch-store machine and LuaJIT's is too -- but the spill
share is the reducible part, and it is the largest single structural item left.

One bite of it worked. `lbl_call` built four out-params for `vm_call_resolve_specialization` and
**took their addresses**, which forces all four into memory for the whole handler no matter which
path runs -- an escaping address cannot live in a register. Scoping those to the branch that uses
them: `fib_bench` **-4.53%**, `binary_trees` -0.93%, and `lbl_call` went from 103 instructions with
22 spill references to 92 with 17. Worth checking any hot handler for the same shape.

What did **not** work, against expectation: hoisting `lbl_interp`'s `AerVal parts[16]` (256 bytes)
out into a noinline helper, the same fix that was right for `OP_INDEX_GET_INTERP`. It moved nothing
on the benchmarks that do not interpolate (`mandelbrot`, `fib_bench`, `binary_trees` all 0.00%) and
cost the ones that do 1.2% (`dict_bench` +1.16%, `small_dict_bench` +1.23%) for the extra call and
its five arguments. **gcc already overlaps locals whose live ranges do not overlap**, so a per-label
array is not additive frame cost the way it looks -- 5.16d's +4.52% came from adding a *second* live
array alongside an existing one, not from the array's size alone. Reverted.

**What the spill slots actually hold.** Reading the prologue against the sampled slot offsets, the
spilling is not spread across dozens of temporaries -- it is two values, each given several slots by
the register allocator:

| value | slots | share of `fib_bench` cycles |
|---|---|---|
| `vm` | sp+44, sp+72 | **10.4%** |
| `c` (Chunk*) | sp+40, sp+52, sp+68 | **9.4%** |
| `const_pool` | sp+48 | 3.1% |

That is essentially all of it, out of a 3268-byte frame. Both are reloaded a few hundred times
across the function because nearly every label needs one or the other, and neither survives in a
register across 153 label bodies on a 14-register ISA.

Three attempts to reduce it, all measured:

- **Hoisting `&vm->heap`** so the 33 `gc_maybe_collect` sites test the threshold without touching
  `vm`: **0.00% on every benchmark.** gcc was already CSE-ing the address through `always_inline`.
  Reverted -- machinery for no gain.
- **Register-allocation flags.** `-fira-region=one` +1.73% on fib, `-fsched-pressure` 0.00%,
  `-fira-algorithm=priority` -0.65% on fib and nothing elsewhere, `--param=max-inline-insns-auto=8`
  far worse. A lottery that would not generalise across compilers, same objection as `ARCH_FLAGS`.
- **Shrinking the frame** (5.16h above): no effect, gcc overlaps non-overlapping label locals.

And note what does *not* follow: **cutting opcodes will not help this.** Register allocation is
driven by live ranges, not code size -- the dozen values live across every dispatch conflict with
everything no matter how many labels exist. Splitting the interpreter into hot and cold halves does
not help either, for the same reason: the hot half would still need the same values live. 5.16
measures the icache miss rate at 0.02-0.10%, so the 45KB is not costing anything directly. Trim
opcodes for comprehension; do not expect registers back.

**So: can the spilling be fixed?** Partly, and the shape of the answer is precise. Removing a hot
*use* of a spilled value helps; adding another hoisted *value* does not, because the live set is
already saturated.

| change | effect |
|---|---|
| hoist `c->code` (READ refetched it every dispatch) | 9 benchmarks improved, `fib_bench` **-2.02%** |
| scope the address-taken specialization out-params | `fib_bench` **-4.53%** |
| hoist `c->functions` (indexed on every call) | `fib_bench` **-0.43%** |
| hoist `&vm->heap` for the 33 GC checks | 0.00% -- gcc already CSE-d it |
| **hoist `vm->call_stack`** | **`fib_bench` +2.17%, `binary_trees` +0.86%** |

That last row is the ceiling made visible. `vm->call_stack` is a fixed inline array whose address
never moves, `lbl_call`/`lbl_return` index it repeatedly, and hoisting it is the identical trick that
worked three rows above -- but it made things *worse*, because it became one more value competing for
the same 14 registers and evicted something hotter. Trading a dereference for a live value only pays
while a register is free, and there isn't one.

That is the floor for a C-compiled computed-goto interpreter on a 14-register ISA. LuaJIT avoids it
by writing the interpreter in assembly and pinning BASE/PC/DISPATCH to fixed registers, which no
amount of C-level restructuring reproduces. Further progress here means removing hot uses one at a
time -- worth 0.4-4.5% each and getting scarcer -- not another structural attempt.

### 5.16i Per-operand-kind opcodes are not worth it (measured with a single probe)

The standing hypothesis for closing the remaining gap to LuaJIT's interpreter was to split binary
operators by operand kind, the way LuaJIT has `ADDVV`/`ADDVN`/`ADDNV`. The reasoning looked strong:
disassembling a boxed subtract showed **44 instructions, of which 16 were RK operand decode** -- the
register-vs-constant test, twice -- against only 6 doing the arithmetic. Removing that from the
`fib_bench` shape (`n - 1`, `n - 2`, 29.9M executions) should have been worth ~7%.

Rather than build the whole family (16-20 opcodes), one probe: `OP_SUB_RC`, a reg-minus-const
subtract reading `registers[b]` and `const_pool[c]` directly. It compiled, gated clean at 154/154
coverage, and fired 29,860,702 times in `fib_bench` -- exactly replacing `OP_SUB`.

**`fib_bench` -1.95%. Nine other benchmarks regressed 0.5-4.7%** (`mandelbrot` +4.72%, `nbody`
+2.72%, `struct_array_scan` +1.98%). Net clearly negative, and reverted.

Two things worth keeping from it:

**The decode is not worth what it looks like.** 1.95% of 6.889B over 29.86M executions is **4.5
instructions saved per subtract, not 16.** gcc had already collapsed most of that decode -- it is
branchless `ite` predication that pipelines well, not 16 independent instructions. Reading an
instruction count off a disassembly listing overestimated the real cost by 3.5x.

**And the per-opcode tax is larger than the per-opcode win.** One addition cost nine unrelated
benchmarks more than it gained on its target, which is the same ~3% layout roll 5.16d measured from
the other direction. Scaling to 16-20 opcodes multiplies the tax while the wins stay confined to
whichever shapes each one covers.

So the last structural idea on the list is closed. LuaJIT's advantage here is not the opcode split by
itself -- it is that a hand-written assembly interpreter pays no layout lottery and no register
pressure, so the split is free for them and costs us more than it returns.

### 5.16j A 21% win sitting in sieve, blocked by one unrecognised expression shape

Profiling every benchmark **by function** rather than by opcode (the same lens that found the
redundant `strlen` and the `memcmp` call) turns up one clear outlier:
`vm_index_set_compute` is **28.67% of `sieve`**.

The cause is a gap in the bounds proof, not a fundamental cost. `sieve` reads and writes the same
array in the same nest:

```
for p in 2..n:                     -- read  is_composite[p]      -> OP_TYPED_INDEX_GET_UNCHECKED
    for multiple in (p*p)..n..p:   -- write is_composite[m] = 1  -> OP_INDEX_SET  (generic!)
```

13.0M reads take the unchecked path; **29.9M writes do not**. `index_safe_unchecked` needs both
`bound_safe` (the range end is the tracked `length(arr)`) and `start_safe`. Isolating it by variant:
`p..n`, `p..n..p`, `2..n..p` and `2..n` **all** get the unchecked write -- steps and computed steps
are fine. The only shape that fails is the computed start `p * p`, because `start_safe` recognises a
non-negative literal, a bare enclosing-safe register, or `safe_reg + non-negative const` (an OP_ADD
decode) -- but not an OP_MUL.

Forcing it through with a throwaway parser hack that accepts `safe_reg * safe_reg`:
**`sieve` -21.38% instructions, and nothing else moved past 0.02%.** The prize is real.

**Why it was not shipped.** `p` is proven `0 <= p < count`, so `p*p >= 0` mathematically -- but
`count` is `unsigned int`, so an index can reach 4.29e9 and `p*p` can overflow int64 to a negative
start. Today that yields a clean "index out of bounds" from the checked path; with the unchecked
opcode it is an out-of-bounds write. A 3-billion-element `int8` array is 3 GB -- remote, but
reachable, and memory corruption is not an acceptable trade for a benchmark number.

The fix that would close it properly: have `OP_ITER_RANGE_PREP` verify `cur >= 0` **once per loop**
when the loop was compiled on this assumption, raising exactly the out-of-bounds error the checked
path would have. One comparison per loop entry, not per iteration, and it needs a flag bit on PREP.
That is a design change worth doing deliberately rather than bolting on.

**Also measured and rejected on the way:** reordering `vm_index_set_compute`'s type dispatch.
`TYPE_TYPED_ARRAY` sits fourth in its if-else chain, so every typed write appeared to pay three
failed tag compares. Converting the chain to a jump-table `switch` moved `sieve` **+0.14%** -- i.e.
nothing -- and regressed nine other benchmarks 0.5-4.7%. The dispatch was never the cost; the bounds
check, the element-type validation and the write are. Reverted.

### 5.16k Proving range starts non-negative -- and the segfault that found

`index_safe_unchecked` lets a range-for's body index without bounds checks when the range end IS
`length(arr)` and the start is provably `>= 0`. The upper half was solid; the lower half was three
hand-decoded shapes -- a literal, a bare enclosing-safe register, and `safe_reg + const` recognised
by inspecting the emitted `OP_ADD` word. Anything else fell back to the generic path, which is why
`sieve`'s `(p*p)..n..p` writes ran through `vm_index_set_compute` -- **28.67% of that benchmark**
across 29.9M writes, while its reads on the same array in the same nest took the fast path.

Replaced by a composable predicate rather than a fourth special case. `Parser.reg_nonneg` marks a
register proven `>= 0` -- a non-negative constant, a bounded loop index, `length()`, or those
combined with `+ * // %` -- propagated at the single `emit_binary` funnel, seeded at `reg_alloc` (so
a recycled register carries no stale proof) and cleared by the existing `invalidate_register`. Not
propagated through `-` or `<<`, which can go negative from non-negative operands. `start_safe`
collapses to one call and the three special cases are **deleted**. **`sieve` -21.38%.**

**It also fixed a segfault in shipped code, which the new tests found.** The proof assumed the loop
ascends -- but direction is inferred from the bounds, so `for i in 200..length(a)` on a short array
counts *downwards* from 200, straight off the end, writing unchecked. On the then-current `feature`
that was 199 out-of-bounds writes and a reproducible `exit 139`. It had presumably been reachable
since non-literal starts were first accepted.

Both halves of the precondition are now verified **once per loop entry** in `OP_ITER_RANGE_PREP`,
which already validates the bound types and rejects `step <= 0`. One unsigned compare covers both
(`rng_end` is a length, so a negative `cur` reinterprets as a huge unsigned and fails the same test).
The flag rides in the spare high bits of the `item_dest` operand -- an 8-bit register index in a
32-bit word -- so no encoding growth and no new opcode. Overflow in a computed start is caught by
that same check, which is what makes the predicate safe to extend with more operators later without
a fresh safety argument.

One caveat recorded honestly: `nbody` is **+0.50%**, over the usual 0.30% bar. Isolated by forcing
the guard flag permanently off -- same code shape, guard never taken -- and `nbody` stayed +0.50%
while `sieve` kept its -21.38%. So the cost is codegen shift from touching these files, not the
guard's arithmetic (which would be ~3 instructions against the 10.5 per `PREP` the delta implies).
Landed anyway: it buys a memory-safety fix and 21% on another benchmark.

### 5.16l Taking lbl_call apart: what its 91 instructions are, and what moved them

Attributing the handler instruction-by-instruction against source (`objdump -dS` over the range
`perf` identifies as `lbl_call`) rather than guessing. What it is actually made of:

- `add.w r6, r2, ip, lsl #6` -- the `CallFrame` power-of-two shift working as intended (5.16e).
- `mov.w r3, #328` + `mla r5, r3, r5, r2` -- **`sizeof(ChunkFunction)` is 328, not a power of two**,
  so `functions[func_index]` costs a materialized constant plus a multiply where a shift would do.
  The same defect `CallFrame` had, still present one struct over.
- `cmp.w ip, #35` + `beq.w` -- the tail-call test, on the hot path.
- `str r2, [sp, #356]` at entry, reloaded at exit -- `dest_reg` spilled and re-read for nothing.
- Six frame-field stores at offsets `#2024`-`#2044`, all inside the 12-bit window (5.16e's ordering
  fix holding).

**Splitting `OP_TAIL_CALL` into its own label: neutral.** `OP_CALL` and `OP_TAIL_CALL` shared
`lbl_call`, so every ordinary call tested `cur_op == OP_TAIL_CALL` and carried the tail body in its
live range -- and the suite runs **35.5M ordinary calls against zero tail calls**. Separating them
measured `binary_trees` -0.21%, everything else 0.00%. Kept anyway: one label per opcode is simpler,
and it costs nothing.

**Inlining the specialization cache hit: reverted, and the most useful number here.**
`vm_call_resolve_specialization` is `noinline` with nine parameters and ran on *every* call to a
shape-sensitive function even when the per-site cache hit -- 6.4% of `binary_trees`. Inlining just
the hit test for a struct receiver bought **`binary_trees` -4.63%** and cost **`fib_bench` +3.87%**,
a net **+125M instructions** across the two.

`fib_bench` never executes that code -- its `shape_sensitive_mask` is zero, so the branch is never
taken. The 3.87% is the cost of roughly ten instructions merely *existing* inside `lbl_call`. That is
the sharpest measurement yet of what 5.16h describes: this handler is at its register-allocation
limit, and anything added to it is paid for by every call in every program, executed or not. Adding
to `lbl_call` needs a win larger than ~4% on the benchmark it targets before it breaks even.

### 5.16m The module-call path: where nbody's 9.6% goes, and why it stays there

`math.sqrt` is 9.6% of `nbody` across `vm_call_module_dispatch` + `aer_math_call` +
`math_pop_double`, over 11.5M `OP_CALL_MODULE` dispatches -- roughly 80 instructions for a
`double -> double`. Reading the whole path end to end:

1. `lbl_call_module` reads three operand words, then `PUSH`es each argument onto `vm->stack` with a
   bounds check;
2. a `noinline` **seven-argument** call into `vm_call_module_dispatch`, which switches on `module_id`;
3. `aer_math_call` matches `fn_id` down an if-chain, then `math_pop_double` pops (bounds-checked);
4. `math_unary` switches on **the same `fn_id` a second time**;
5. the result is pushed (bounds-checked), then `lbl_call_module` pops it back into a register.

So the value travels **register -> vm->stack -> local double -> vm->stack -> register**: four
bounds-checked 16-byte transfers where one move would do. Note the if-chain is *not* nbody's problem
-- `FN_MATH_SQRT` is 0 and first in the chain, so it matches immediately.

Costed before building, per the "measure before building" rule. The three changes available without
touching the module ABI or adding an opcode: passing `c`/`module_idx`/`fn_idx` lazily (they are used
only by the dynamic-host fallback and the not-found error, but 32-bit ARM puts the 5th argument
onward on the stack) ~0.36%; hoisting the per-argument `PUSH` bounds check ~0.24%; collapsing the
duplicate `fn_id` dispatch ~0.36%. **Combined ceiling ~0.95%**, and that assumes every saved
instruction is real.

That is below the 1% bar this step was given, so it was not built. What is left is the four stack
round-trips, and removing those means either changing the module ABI so arguments pass in registers
(every module function signature) or a fused opcode for 1-argument numeric math -- and 5.16i prices
a new opcode at more than it returns. `nbody` already beats both comparison interpreters, so this is
recorded rather than pursued.

### 5.16n Five benchmarks measured a result nothing checked

`nbody.aer`, the three `nbody_large_*` variants and `typed_array_bench` all computed a result into a
variable and then printed only their timing. A benchmark whose output cannot change when its answer
changes measures speed at the cost of being unable to notice a wrong answer -- and two ports had
already been caught doing exactly that (`nbody.lua`'s 500x scale bug, and `nbody.py` raising
`IndexError` on `sys.argv[1]` while scoring a bogus 0.28s in a five-way comparison).

All five now print their result after the timing is captured, so validation costs the measurement
nothing. `nbody.aer` matches `nbody.lua` to every digit Lua prints (`-0.169075164`/`-0.169096160`).

The layout variants are the reason this mattered most: `nbody_large_boxed` and `nbody_large_packed`
agree exactly (`-4.0182e-06 -> 30931.1`), which is the check that the packed representation computes
what the boxed one does. `nbody_large_packed_narrow` agrees only on the *initial* energy
(`-4.01819e-06`) and ends at `1.84e+07` against the float64 pair's `30931.1`. That is not a bug:
1024 bodies over 20 steps have close encounters, and a `1/r^2` singularity amplifies float32 rounding
macroscopically. Only the initial energy is comparable across field widths; the final one is a
per-variant determinism check.

`typed_array_bench` initially printed `xs[0]`, which its `xs[k] = k * 0.5` fill makes permanently
`0.0` -- a checksum that could never fail. It prints `total` and `xs[N-1]` instead.

### 5.16o The dict benchmarks re-hash immutable strings (investigated, then built)

`small_dict_bench` runs 8.38B instructions in 2.57s. Its profile is `vm_run_slice` 30.5%,
`pool_alloc` 14.2%, `hashtable_put_hashed` 10.3%, `vm_index_get_compute` 8.4%, `hashtable_key_dup`
6.1%, `hashtable_free` 5.0%. `hashtable_hash_bytes` appears nowhere because LTO inlines it into the
first four.

Every dict access recomputes the FNV hash of its key from bytes. The benchmark's keys are the string
constants `"id"`, `"name"`, `"active"`, `"score"` -- values whose hash cannot change. Per iteration
it hashes 17 bytes building the dict and 7 more reading it back; over 3.3M iterations that is ~79M
byte-steps of a hash whose answer was already known. `perf annotate` confirms the loop body is
`eor` + `umull` + `mla`: FNV's 64-bit multiply, which 32-bit ARM has no single instruction for.

There is a second scan on the same bytes. `lbl_dict_new` computes `hashtable_key_true_len(...)` and
passes the result to `hashtable_key_dup`, which calls `hashtable_key_true_len` **again** on the
already-truncated length -- a walk that by construction can no longer find a NUL.

**Why this is not the borrowed-keys idea that failed twice.** 5.16a records two attempts at pointing
dict keys at constant-pool bytes, both reverted. Those changed *ownership*: a key's lifetime stopped
matching the table that held it. Caching a hash changes no ownership at all. It memoizes a pure
function of bytes that never change -- `aer_string_alloc` (`vm.c:333`) is the single site that
allocates an `AerString`, and nothing anywhere writes `->data` or `->length` afterwards.

`hashtable.h` already anticipates the caller: `hashtable_put_hashed`'s comment names "an `AerString`
reused as a dict key many times" as its reason to exist. But it also requires the supplied hash to
equal `hash_bytes(key, length)` **exactly**, "or this table's probe sequence silently disagrees with
a plain `hashtable_get`/`put`'s, corrupting lookups" -- and `HashTableEntry.hash` is a `uint64_t`.

**The field is not free, and the first version of this note was wrong to say so.**
`sizeof(AerString)` is 28 on 32-bit ARM against a `stride = (elem_size + 7) & ~7` of 32, so there
are exactly **4** spare bytes per string cell. A `uint64_t` needs 8, and its 8-byte alignment on
ARM32 EABI pushes the struct to 40 -- **+25% on every string in the program**, not just dict keys,
which `log_processing` would pay in full for no benefit.

Three ways out, in preference order:

1. **Narrow the hash to 32 bits everywhere.** FNV-1a's 32-bit form is the standard variant, so this
   is not a quality compromise. It makes the cache field fit the existing padding for free, shrinks
   `HashTableEntry` by 4 bytes as well, and independently makes *uncached* hashing cheaper: 32-bit
   ARM has a single-instruction `mul` for it, against the `umull` + `mla` pair a 64-bit multiply
   costs today. Blast radius is `hashtable.c`'s probe/compare paths and every `hashtable_*_hashed`
   signature.
2. Accept the 8 bytes and measure whether `log_processing`'s memory regression is tolerable.
3. Shrink `AER_STRING_INLINE_MAX` to make room -- rejected, since 5.16's SSO work measured 15 as the
   win and this would give it straight back.

Either way the cache is lazy, with 0 meaning "not computed yet"; a key whose true hash is 0 just
recomputes, which is correct and rare. It is consulted only when `true_len == length`, because a
string with an embedded NUL hashes over its truncated prefix and the two lengths would disagree.

The duplicated `hashtable_key_true_len` walk is separate from all of this and carries no trade-off,
so it was fixed on its own: `hashtable_key_dup_known` for callers already holding the truncated
length. **`small_dict_bench` -5.51%, `dict_bench` -2.69%**, every other benchmark inside ±0.04% and
all 15 outputs byte-identical. Larger than the raw byte count suggests -- the scan is a
branch-per-byte loop, and removing it also unblocks the copy that follows it.

**The hash cache was then built, taking option 1.** `HashValue` is a `uint32_t` and
`hashtable_hash_bytes` is FNV-1a's 32-bit form; `AerString.hash` memoizes a key's hash with 0
meaning not-yet-computed, populated through `hashtable_string_hash`.

| benchmark | instructions |
|---|---|
| small_dict_bench | **-10.14%** |
| lookup_table_bench | -3.35% |
| log_processing | -2.46% |
| dict_bench | -1.48% |
| binary_trees | -1.15% |
| struct_array_scan | -0.62% |
| fib_bench | **+0.43%** |

All 15 benchmark outputs byte-identical, UBSan clean, 300 fuzz iterations with no crash or hang.

The "free field" claim was checked rather than assumed: `sizeof(AerString)` goes 28 -> 32 while the
pool's rounded stride stays **32**, and `log_processing`'s peak RSS is identical to the byte
(84048 KB both sides). `HashTableEntry` also loses 4 bytes.

`fib_bench` is kept despite crossing the 0.30% revert line. It contains no dict or string work at
all -- one `print` -- so the extra instructions cannot be the cache doing work; it is the
register-allocation shift 5.16l describes, where changing code anywhere in `vm_run_slice`
re-allocates registers across all 153 label bodies and `fib_bench`'s hot `lbl_call` pays for it.

**A correction.** This section originally justified keeping it with "cycles are lower in the new
build (4.278B against 4.308B)". That evidence does not survive 5.16p: cycle measurements at
`--runs 3` carried a head-favouring ordering bias plus ~4% noise, so a 0.7% cycle difference
measured that way was indistinguishable from nothing. The change is kept on the strength of the
six instruction wins and the mechanism above, not on that cycle reading.

### 5.16p The benchmark harness was biased toward `head`, and cycles need seven runs

Found while trying to adjudicate a change whose instruction and cycle counts disagreed. Running
`tools/bench.py --base HEAD --head HEAD` -- the same commit against itself, where every delta must
be zero -- reported **six improvements and no regressions**, up to `fib_bench` **-5.15%**.

Two separate defects, both now closed:

**1. Ordering bias (fixed).** `measure()` ran every base sample, then every head sample. Any drift
over the measurement window -- frequency ramp, page-cache warming, thermal -- landed entirely on
whichever side ran first, which was always base. Instructions are deterministic and were never
affected; cycles were, systematically and in one direction. `measure_pair()` now interleaves
base/head within each run, and the same control scatters both ways instead of favouring head.

**2. Cycle noise is far larger than assumed.** Even interleaved, a self-control at `--runs 3` still
shows `fib_bench` +3.77% on identical code. At `--runs 7` it falls to ±0.8%. `perf stat -r 5` on one
binary reports instructions at ±0.00% and cycles at ±0.90%, so this is inherent run-to-run variance,
not the harness.

The working rules this establishes:

| metric | runs | resolves |
|---|---|---|
| instructions | 3 | ±0.03% -- the gate |
| cycles | **7+** | ~±0.8%, so only effects above ~1.5% |
| cycles | 3 | **nothing** -- do not quote it |

The older "cycles spread ±0.4% ref-against-itself" note that several sections lean on was measured
before this and is optimistic by roughly an order of magnitude at `--runs 3`. Any conclusion in this
document that rests on a sub-1.5% cycle difference at low run counts should be treated as unproven;
5.16o has been corrected on exactly that basis.

### 5.16q Two more attempts on `vm_run_slice`'s register pressure, both reverted

`fib_bench` runs ~234 instructions per call against LuaJIT `-joff`'s ~117, and `perf annotate` blames
spill traffic rather than dispatch: **25.16% of its samples are stack-slot loads/stores**, with
`vm` (8.79%), `registers` (6.27%) and `const_pool` (4.43%) -- **19.5% between them** -- reloaded from
`[sp,#44]`, `[sp,#40]`, `[sp,#48]`. All three are already hoisted into locals; the allocator spills
them anyway, because ~10 long-lived pointers compete across 153 label bodies on an ISA with ~11
allocatable registers. The hottest single instruction is the RK8 const-flag test, which GCC compiles
into a *conditional reload of a base pointer from the stack* on every operand decode.

**Attempt 1: un-hoist `raw_ints`/`raw_reals` to free two registers.** Only the ~18 `lbl_raw_*`
labels use them, and `fib` uses none at all (its bytecode has zero raw opcodes -- verified with
`--debug-path`). The full ablation, in instructions:

| variant | fib | sieve | nbody | mandelbrot | binary_trees |
|---|---|---|---|---|---|
| both un-hoisted | **-4.28%** | +1.99% | +0.72% | -0.37% | -1.10% |
| `raw_reals` only | -2.57% | +0.01% | +2.15% | **+3.45%** | -0.95% |
| `raw_ints` only | -0.86% | +0.01% | -0.01% | +0.57% | -0.15% |

Freeing *one* register produced a worse allocation than freeing two -- the middle row is the worst of
the three. Reverted: the best variant still trades `sieve` +1.99% and `nbody` +0.72% for `fib`, which
is chasing one synthetic call microbenchmark at the suite's expense.

**Attempt 2: stop re-deriving the frame pointer the call path already holds.** `lbl_call`,
`lbl_return` and `vm_call_value`'s tail-call setup each finished by reading
`vm->call_stack[vm->call_depth]` three times to refill the `vm->` mirror, then read the mirror
straight back into the hoisted locals -- while `callee` already pointed at that exact frame. Removing
the recompute is strictly less work. It measured `fib_bench` **+0.86%** instructions, and at
`--runs 7` cycles agreed: mandelbrot +2.15%, lookup_table +5.01%, fib +0.44%. Reverted.

**The rest of the hoisted set was then ablated too, to settle which locals earn their register:**

| un-hoisted | result |
|---|---|
| `raw_ints` + `raw_reals` | `fib` -4.28% but `sieve` +1.99%, `nbody` +0.72% -- a trade, not a win |
| `functions` | `fib` **+0.43%**, everything else flat |
| `const_pool` | **all ten benchmarks regress** (nbody +2.35%, sieve +2.00%, fib +1.93%, mandelbrot +1.51%) |

So the hoists are not excess baggage -- every one of them is load-bearing, and the two that look
most redundant (`functions`, used only by `lbl_call`; `raw_*`, used by 18 of 153 labels) are the only
ones where un-hoisting is even arguable. **This closes the "reduce what we pin" line of enquiry.**

Together with 5.16e attempt 4 and 5.16h, that is four independent attempts. The pattern is now firm
enough to state as a rule: **in `vm_run_slice`, removing work and freeing registers are not the same
as going faster.** Any edit reshuffles allocation across all 153 labels, and the reshuffle routinely
outweighs the work removed -- in both directions, unpredictably. Micro-editing this function is a
lottery; the productive changes have all been *addressing* changes (5.16e's power-of-two frame,
5.10's struct layout) or work removed *outside* it (5.16o's key scan, 5.16n's string search).

### 5.16r The compute-bound gap is codegen, not the VM

Against LuaJIT `-joff` in instructions: `fib` 1.99x, but `nbody` **1.04x** and `mandelbrot`
**1.05x** -- and on those two AER uses *fewer* cycles (nbody 5.56B vs 6.41B, mandelbrot 4.62B vs
5.14B) at higher IPC. Arithmetic dispatch is not where compute-bound work is losing.

What is losing is what the compiler *emits*. `mandelbrot`'s inner loop, 173,658 iterations at
SIZE=60, is 15 opcodes and at least four are avoidable:

```
17  OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE   rawi0 < reg2
19  OP_RAW_MUL_REAL     rawr2 = rawr0 * rawr0        x2 = x*x
20  OP_RAW_MUL_REAL     rawr3 = rawr1 * rawr1        y2 = y*y
21  OP_RAW_ADD_REAL     rawr4 = rawr2 + rawr3
22  OP_RAW_GT_REAL_BOXED_JUMP_IF_FALSE  rawr4 > 4.0
26  OP_RAW_LOAD_REAL    rawr4 = 2.0                  <-- loop-invariant, reloaded 173k times
28  OP_RAW_MUL_REAL     rawr4 = rawr4 * rawr0
29  OP_RAW_MUL_REAL     rawr4 = rawr4 * rawr1
30  OP_RAW_ADD_REAL_BOXED  rawr4 += reg1             <-- cy is a boxed param: tag check per iteration
31  OP_RAW_SUB_REAL     rawr5 = rawr2 - rawr3
32  OP_RAW_ADD_REAL_BOXED  rawr5 += reg0             <-- cx likewise
33  OP_RAW_MOVE_REAL    rawr1 = rawr4                <-- y = y_new
34  OP_RAW_MOVE_REAL    rawr0 = rawr5                <-- x = x_new
35  OP_RAW_LOAD_INT     rawi1 = 1                    <-- loop-invariant, reloaded 173k times
37  OP_RAW_ADD_INT      rawi0 = rawi0 + rawi1
38  OP_JUMP -> 17
```

Three distinct codegen defects, none of which is a VM problem:

1. **No loop-invariant hoisting of raw constant loads.** `raw_materialize` (`parser.c:877`) allocates
   a fresh slot and emits a load at *every* use of a literal, with no memoization. Fixing it needs
   the load emitted before the loop, so it is loop-structure work, not just a cache. (`sieve` also
   has such a load at 664,579 hits, but that is only **1.2%** of its dispatches -- it is dominated by
   typed-array ops. This defect is concentrated in `mandelbrot`, not general.)
2. **Dead copies survive.** `y_new`/`x_new` are written to temps and then moved into `y`/`x`. Both
   sources are dead at that point, so the arithmetic could target `y`/`x` directly and both moves
   disappear. Needs liveness the single-pass parser does not currently keep.
3. **Parameters stay boxed inside the hot loop.** `cx`/`cy` are read through
   `OP_RAW_ADD_REAL_BOXED`, paying a tag check per iteration. This is the one case where the
   twice-reverted raw-param specialization would genuinely pay -- and note it is *not* `fib`'s shape
   (`fib` unboxes nothing useful), which is why both previous attempts measured it on the wrong
   benchmark.

Counting executed dispatches rather than loop text confirms it. Of `mandelbrot`'s 2.87M dispatches:

| opcode | count | share |
|---|---|---|
| `OP_RAW_MOVE_REAL` | 354,516 | **12.3%** |
| `OP_RAW_ADD_REAL_BOXED` | 347,316 | **12.1%** |
| `OP_RAW_LOAD_REAL` | 195,258 | 6.8% |
| `OP_RAW_LOAD_INT` | 178,050 | 6.2% |

Copies plus constant reloads are **25.3% of everything it executes**, and none of it is the VM's
fault -- these are dispatches the compiler chose to emit.

**What fixing them actually requires, and why it is a design step.** The parser emits as it parses,
in one pass, and both fixes need information a single pass does not have:

- *Dead-copy elimination* needs liveness. `y = y_new` is a copy the programmer wrote; the parser
  already targets expression results directly at their destination slot, so these moves are not
  parser sloppiness. Removing them means knowing `y_new` is dead after the copy, which needs a
  backward scan over the function's emitted code.
- *Loop-invariant hoisting* needs the load emitted before the loop header, but the literal is not
  known until the body is parsed -- so it needs either a pre-pass or relocation of already-emitted
  words with jump patching.

Both are naturally expressed as a **small post-emit pass over one function's bytecode**, which is
tractable precisely because raw slots are function-local and statically allocated: the pass would
work on a dense, closed set of slots rather than general aliasable memory. That is a real addition to
a compiler whose stated virtue is being easy to understand, and should be judged on that basis and
not only on the percentage.

The third defect is cheaper to reach: the raw-param specialization machinery **already exists**
(`vm.c:2327`, `parser_specialize_function`) and is merely gated behind a `SpecEntry`, which only
exists for shape-sensitive functions. `mandelbrot_point(cx, cy, max_iter)` has no struct or
packed-array parameter, so it never gets one. Both prior attempts at unboxing numeric params were
measured on `fib`, which unboxes nothing useful -- 12.1% of `mandelbrot`'s dispatches say that was
the wrong benchmark to judge it on.

### 5.16s Dispatch is eight instructions, and three of them do no work

Disassembling the tail of any label body gives the same eight:

```
ldr.w  fp, [sl, r9, lsl #2]   ; op_word = code[ip]
add.w  r9, r9, #1             ; ip++
ldr.w  r3, [pc, #3008]        ; <-- GOT offset for the dispatch table
uxtb.w ip, fp                 ; cur_op = op_word & 0xFF
add    r3, pc                 ; <-- rebuild the table base from PC
ldr.w  r3, [r3, ip, lsl #2]   ; target = dt[cur_op]
orr.w  r3, r3, #1             ; <-- set the Thumb bit
bx     r3
```

Four instructions are the actual work (fetch, advance, mask, load target). The other four are the
branch itself plus **three that exist only because of how the address is formed**: two rebuilding the
dispatch table's base from `pc` on *every single dispatch*, and one setting the Thumb bit.

This is a large share of everything the interpreter does. `mandelbrot` executes ~414M dispatches
against 8.82B instructions, so **dispatch is roughly 37% of its total instruction count** -- the
single biggest line item anywhere in this document.

**The two PC-relative instructions are the PIE tax, measured directly:**

| benchmark | non-PIE | PIE | delta |
|---|---|---|---|
| mandelbrot | 8.404B | 8.821B | **-4.73%** |
| nbody | 9.342B | 9.672B | **-3.41%** |
| sieve | 6.313B | 6.495B | **-2.80%** |
| fib_bench | 6.814B | 6.979B | **-2.36%** |

That is consistent with 5.16g's older 0.3-4.3% range but now has a mechanism behind it rather than
just a number: position-independent code cannot keep a static table's address as a link-time
constant, and the register allocator (5.16q) has nothing spare to cache it in, so it is rebuilt
per dispatch.

`orr.w r3, r3, #1` **survives in the non-PIE build**, so the Thumb-bit fixup is not a PIC artifact --
it is how GCC materialises `&&label` addresses on Thumb-2, and C offers no way to pre-set it in the
table.

**This is a decision, not a fix.** 5.16g declined non-PIE on fairness, and that argument is
unchanged: LuaJIT and Luau on the test machine are both `pie executable`, so a non-PIE AER would win
comparisons partly on build flags. The two questions are separable, and only the second is
technical:

- *For cross-language benchmarking*: stay PIE, or every published number needs an asterisk.
- *For what ships to users*: 2.4-4.7% is real, and costs ASLR.

**Would removing rarely-used opcodes help?** Reasoned, not measured: no. Computed-goto replicates the
dispatch sequence at every label, so the BTB cost is set by the number of *hot* dispatch sites, which
deleting cold opcodes does not change. It would shrink code size, and 5.16 already measured that
interpreter code size is not an icache problem here. 5.16d's finding that *adding* opcodes hurts is
about the new label bodies competing for prediction resources, which does not run in reverse for
opcodes that never execute.

### 5.16t Pinning the dispatch base: the largest win measured here, and why it is reverted

5.16s identified two of dispatch's eight instructions as rebuilding the table's base from `pc`
because no register was free to cache it. Giving GCC one explicitly --
`register const void* const* aer_dispatch_base asm("r8")`, chosen because r8 is callee-saved under
AAPCS -- removed exactly those two instructions. The disassembly confirms it: dispatch became
`uxtb` + `ldr [r8, ip, lsl #2]` + `orr` + `bx`, with 408 sites using r8 as the base.

**Every benchmark improved, with no regressions -- the only change measured here that has ever done
that:**

| benchmark | instructions |
|---|---|
| sieve | **-6.13%** |
| mandelbrot | **-4.23%** |
| nbody | -2.94% |
| fib_bench | -2.56% |
| struct_array_scan | -2.09% |
| binary_trees | -1.39% |
| small_dict_bench | -1.72% |
| lookup_table_bench | -0.86% |
| log_processing | -0.56% |
| dict_bench | -0.49% |

All 15 outputs byte-identical, 250 fuzz iterations clean, `error_lines.py` clean, actor and
scheduler tests clean.

**It still had to be reverted: it corrupts the heap.** `make test-ubsan` segfaults, and the backtrace
is unambiguous -- `free()` called on `&dt` itself:

```
#8  __GI___libc_free (mem=0x4e7a60 <dt>)
#9  parser_restore_state (s=0x4e7a60 <dt>) at source/compiler/parser.c:4963
    r8  0x6
```

A parser pointer had been given the dispatch base's value. The cause is `-flto`: the declaration
lived in `vm.c`, so only that translation unit reserved r8, while `parser.c` and the rest used it as
an ordinary callee-saved register. LTO then merges and inlines across that boundary, and the two
assumptions collide. GCC's own rule for global register variables -- the declaration must be visible
to *every* translation unit -- is not satisfiable here without putting it in a header that all 29
source files include (three do not even include `error.h`, the closest thing to a universal one).

So the choice is not "is 2-6% worth it" but **"is reserving a register program-wide, in every
translation unit including the parser and stdlib, worth it"** -- which taxes code that never
dispatches, and makes the reservation part of the project's ABI. That is a design decision, not an
optimization, and it is left to be taken deliberately rather than smuggled in behind a measurement.
The measurement above is what it would be worth.

**Two safe versions were then tried, and neither works.** The obvious hope is that the allocator
would keep the base itself if simply asked, or if given room:

| variant | sieve | mandelbrot | nbody | fib |
|---|---|---|---|---|
| `const void* const* dtb = dt;` as a plain local | **-4.50%** | **+5.20%** | +2.03% | -0.86% |
| the same, plus un-hoisting `c` to free a register | -4.50% | +5.19% | +2.10% | -0.86% |

Freeing a register changed **nothing** -- the two rows are the same measurement. GCC does not spend a
freed register on the dispatch base; it had already decided, and naming `dtb` only forces the base
live at the cost of whatever `mandelbrot`'s hot loop wanted more. This is 5.16q's rule again, and it
rules out the polite version of the fix: the win is only available by *taking* a register, not by
asking for one or making space.

**The program-wide reservation was then done properly, and still fails.** The declaration was moved
into `error.h` -- the one header every translation unit that links the VM already includes, the
smoke/embed tests included -- and the two library sources that lacked it (`value_format.c`,
`aer_stdlib.c`) were given it. The pin demonstrably took effect: dispatch compiles to
`ldr.w r3, [r8, ip, lsl #2]` with no `pc` arithmetic. The x86 build, the full test gate and the
comment check all pass.

`make test-ubsan` **still segfaults.** So visibility in every TU was necessary but not sufficient,
and the failure is not the LTO-inlining story alone. Whatever the residual cause -- the sanitizer
runtime, or GCC declining to honour the reservation under instrumentation -- the first version was
provably miscompiling (a parser pointer holding `&dt`), and there is no way to establish that the
same corruption is not happening silently in the uninstrumented build. It passes every test, fuzzes
clean and produces byte-identical output; so did the version that was definitely wrong.

**That conclusion was wrong, and was reached by reverting instead of diagnosing.** Two experiments
found the real cause:

- Building the UBSan binary **without `-flto`** passes every test. LTO is the culprit, not the pin.
- `parser.c`'s assembly still contained `r8` uses despite including the declaration -- so GCC's LTO
  simply **does not honour a source-level `register asm("r8")` reservation** across translation
  units, no matter how visible the declaration is.

The fix is to state the reservation where LTO cannot ignore it: **`-ffixed-r8`**, a codegen-level
flag rather than a source declaration. With `-flto` *and* `-ffixed-r8`, the build that segfaulted
passes. The two are a matched pair and neither is correct alone -- the makefile sets them together,
ARM-only, and says so.

Verified: UBSan clean on the previously-failing test and others, `MALLOC_CHECK_=3` clean on the
normal build, 150 fuzz iterations with no crashes, all 15 benchmark outputs byte-identical.

**Net effect, against a clean baseline:**

| improved | | regressed | |
|---|---|---|---|
| sieve | **-4.55%** | lookup_table_bench | +1.05% |
| mandelbrot | **-3.85%** | small_dict_bench | +0.75% |
| nbody | -1.33% | dict_bench | +0.64% |
| fib_bench | -1.07% | | |
| struct_array_scan | -1.03% | | |
| binary_trees | -0.98% | | |

The split is exactly the mechanism: benchmarks that dispatch heavily win, and the three that spend
their time in hashing and allocation pay for a register now reserved program-wide. `-ffixed-r8`
costs 0.4-2.5% on its own; the dispatch saving more than covers it where dispatch dominates.

The obvious worry -- that a program-wide reservation quietly taxes the *compiler*, which every
program runs and which no benchmark isolated -- was checked and is **not** borne out. AER parses on
the fly, so every benchmark already pays its own parse cost; it is simply swamped by loops running
billions of iterations. `bench/compile_bound.aer` (6600 lines of declarations, almost no runtime
work) makes parsing the dominant term instead, and across the pin it measures **-0.00%**. The
regressions are specific to hashing and allocation, not general to the front end.

**On fairness**, since 5.16g rejected `-no-pie` on exactly that ground: this is a different kind of
change. `-no-pie` alters the shipped binary's security properties, and the interpreters compared
against ship PIE. `-ffixed-r8` is an implementation choice about AER's own source; the binary stays
PIE. Pinning interpreter state in fixed registers is standard practice -- LuaJIT pins four
(`BASE`, `PC`, `DISPATCH`, `KBASE`) by writing its interpreter in assembly. This pins one, from C.

### 5.16u The range-for counter, and why the loop variable stays boxed

`for i in 1..100000000: total += i` compiles `total` into a raw unboxed slot but leaves `i` in an
ordinary tagged register, so the body uses `OP_RAW_ADD_INT_BOXED` — a tag check per iteration. The
obvious fix is to give the loop variable a raw slot too. **It is the wrong fix**, and the reason is
worth recording because the idea keeps looking attractive.

Raw slots are not addressable as index operands. `index_safe_unchecked` (parser.c) rejects a
raw-flagged index outright, and `emit_index_get` packs its index as RK8, which encodes a register or
a constant and has no third case. So a raw loop variable would emit an `OP_BOX_INT` at *every*
`a[i]` and simultaneously forfeit the bounds-proof elision of §5.15 — trading one predictable tag
check for a whole extra dispatch plus a restored runtime bounds check, on exactly the array-scanning
loops (`sieve`, `struct_array_scan`, `nbody`) the change was meant to help. Making it pay would mean
raw-index variants of the whole index-get/set family, which §5.16d already measured as a losing
trade. The tag check stays.

What was actually costing something sat one level up, in `OP_ITER_RANGE_LOOP` itself. It maintained
*two* registers per iteration — a private counter and the loop variable it publishes to — each
written as a full 16-byte tagged store. The second register exists only because a body is allowed to
assign its own loop variable, which (Lua's numeric-for semantics, which AER matches) must not disturb
iteration.

The parser can settle that question for free. `LOOP` is emitted *after* the body, so by the time it
is built the parser already knows whether the body ever wrote that register — `invalidate_register`
is called at every site that changes what a register holds, which is the same choke point the
bounds proof and `length_tracked_valid` already depend on. When nothing wrote it, the loop variable
*is* the counter and the two collapse into one. `PREP` needs no change at all in either case: it
only ever reads its `cur` operand, so it can keep pointing at the start snapshot while `LOOP` points
at the loop variable. No new opcode, no flag bit, no patching — just a different operand.

Alongside it, `cur`/`remaining`/`step` are now written value-only (`.as.i`) rather than as full
tagged stores. `PREP` validated all three as integers and they are loop-owned snapshots that
`arg_materialize` guarantees nothing else can alias, so no tag can have changed under them.

Instructions, Pi, `--runs 3`:

| benchmark | delta |
|---|---|
| `sieve` | **-6.25%** |
| `struct_array_scan` | -1.44% |
| `lookup_table_bench` | -1.28% |
| `nbody` | -1.00% |
| everything else | within ±0.05% |

On cycles `sieve` confirms at -3.1 to -3.9%. `nbody` first read +2.28% and then +0.69% on a repeat,
which is the §5.16p noise floor talking, not a result — a reminder that a single cycle sample is
still not evidence even at seven runs.

### 5.16v The pinning matrix: r8 is the only one worth having

5.16t pinned the dispatch base in `r8` and closed with the open question of whether LuaJIT's other
three (`BASE`, `KBASE`, `PC`) were worth the same treatment. They are not. Every remaining
candidate was built and measured, alone and in combination, since 5.16q established that freeing
registers is not additive — the allocator's response to two reservations is not the sum of its
response to each.

`registers`, `const_pool` and `ip` differ from the dispatch base in one way that decides the
implementation: the base holds one value for the process's life, so a nested `vm_run_slice`
overwriting it is harmless, while all three of these change per invocation. Each therefore needs an
entry save and an exit restore, exactly like `runtime_error_unwind_target`/`active_vm_for_errors`/
`current_heap` already do. The pin was confirmed honoured, not silently ignored, by disassembly:
with `-ffixed-r4` the prologue's `stmdb sp!, {r5, r6, r7, r9, sl, fp, lr}` no longer saves `r4`.

Instructions vs. the r8-only baseline, Pi:

| variant | nbody | sieve | mandel | fib | dict | log | struct_scan | mean |
|---|---|---|---|---|---|---|---|---|
| `BASE` (r4) | -0.57% | -0.30% | +0.29% | -0.00% | +0.86% | +0.87% | -0.18% | **+0.14%** |
| `KBASE` (r5) | +0.27% | +0.69% | -0.04% | +0.86% | +0.91% | +1.29% | +1.33% | **+0.76%** |
| `BASE`+`KBASE` | -0.44% | +0.35% | -0.38% | -2.39% | +1.75% | +1.35% | +0.62% | **+0.12%** |
| `PC` (r6) | +2.92% | +2.35% | +3.99% | +2.16% | +1.05% | +1.40% | +1.74% | **+2.23%** |
| `BASE`+`PC` | +3.31% | +5.00% | +4.30% | +2.15% | +2.04% | +2.01% | +1.88% | **+2.96%** |
| all three | — | — | — | — | — | — | — | **does not compile** |

`PC` is the clearest loss and the most instructive one. `ip` is incremented on every single
dispatch, which is exactly the profile that makes it *want* a scratch register with good addressing
modes — forcing it into a fixed callee-saved slot while simultaneously denying that register to
every other function in the program is a loss on both ends.

`BASE`+`KBASE` is the only combination with a real win in it, `fib_bench` -2.39%, and it is a
textbook non-additive one: `BASE` alone moves fib -0.00% and `KBASE` alone +0.86%, yet together they
find an allocation neither reaches. It still does not clear the bar. The win is on the one
synthetic, call-overhead-dominated benchmark, paid for by `dict_bench` +1.75% and `log_processing`
+1.35% — the two most realistic workloads in the suite.

Pinning all three on top of `r8` does not merely regress, it **fails to build**: `source/terminal.c:180:
error: unable to find a register to spill`. That is the concrete form of the ARM32 register-budget
argument. With `r8` plus three more reserved, roughly seven allocatable registers remain, and
ordinary C in a file that has nothing to do with the interpreter can no longer be compiled at all.
It is also the direct answer to "LuaJIT pins four, why don't we": LuaJIT can, because it hand-writes
its interpreter in assembly and pays the cost in exactly one function. A C compiler applies
`-ffixed` to the whole program, so the reservation is charged against every function whether it
benefits or not.

`r8` earned its keep because dispatch touches it on every opcode and nothing else in the program
wanted it that badly. Nothing else clears that bar. **Pinning is closed at one register.**

The obvious follow-up — if only one register is worth reserving, is the dispatch base the right
thing to put in it? — was measured the same way, each variant reserving exactly one register with
the dispatch pin removed:

| occupant of r8 | nbody | sieve | mandel | fib | dict | log | struct_scan | mean |
|---|---|---|---|---|---|---|---|---|
| dispatch base (current) | — | — | — | — | — | — | — | **0.00%** |
| `raw_reals` | -0.42% | +5.41% | +0.34% | +0.21% | +0.35% | +0.57% | +0.14% | **+0.94%** |
| nothing pinned at all | +1.36% | +5.09% | +3.99% | +1.08% | -0.65% | +0.13% | +1.03% | **+1.72%** |
| `raw_ints` | +1.98% | +5.33% | +2.75% | +1.08% | -0.13% | +0.31% | +1.47% | **+1.83%** |
| `code` | +2.01% | +4.56% | +4.32% | +1.94% | +0.69% | +0.83% | +2.29% | **+2.38%** |
| `const_pool` | +3.43% | +5.81% | +4.27% | +1.95% | +0.42% | +0.90% | +1.85% | **+2.66%** |
| `registers` | +4.64% | +7.30% | +10.09% | +1.73% | +1.22% | +1.12% | +3.48% | **+4.23%** |

Four of the six alternatives are worse than reserving nothing at all, which is the whole finding in
one line: a program-wide reservation only repays its cost if the value occupying it is touched on
literally every dispatch. Anything touched merely *often* loses, because the reservation is charged
against every function in the program while the benefit accrues to one loop.

`registers` is the sharpest illustration. It is LuaJIT's `BASE`, it is the single most-referenced
value in `vm_run_slice`, and as a *replacement* for the dispatch base it is the worst option
measured. The table above it shows the same value as an *addition* to r8 costing +0.14%. Both are
true: the base pin is worth roughly nothing either way, and the dispatch pin is worth 1.72%, so
trading one for the other loses the difference.

**Re-measured after 5.16w**, since merging `code` and `ip` into one pointer removed a live value
from the hot path and could have made a second reservation affordable:

| variant | nbody | sieve | mandel | fib | dict | log | struct_scan | mean |
|---|---|---|---|---|---|---|---|---|
| dispatch base only | — | — | — | — | — | — | — | **0.00%** |
| + `pc` in r4 | -2.09% | +0.88% | -6.52% | +0.66% | +0.42% | +0.01% | +2.05% | **-0.66%** |
| + `raw_reals` in r4 | -2.06% | +1.26% | -3.80% | -0.44% | +0.99% | +1.15% | -0.38% | **-0.47%** |
| + `registers` in r4 | +0.23% | +2.24% | +0.32% | +0.44% | +0.87% | +0.81% | +0.32% | +0.74% |
| r8 = `pc` instead | +1.87% | +6.60% | +3.13% | +1.12% | +0.32% | -0.01% | +4.86% | **+2.56%** |

The occupant question is settled twice over: `r8` holds the dispatch base, and giving it to `pc`
instead costs +2.56%. The *second* pin question did move -- `pc` in r4 is -0.66% where the best
addition before was +0.14%.

Re-measured again after 5.16x's hoist, which changes the instruction mix these depend on:

| variant | nbody | sieve | mandel | fib | dict | log | struct_scan | mean |
|---|---|---|---|---|---|---|---|---|
| + `raw_reals` in r4 | -2.07% | +1.24% | -3.82% | -0.44% | +0.93% | +1.09% | -0.39% | **-0.49%** |
| + `pc` in r4 | -2.11% | +0.89% | -5.78% | +0.65% | +0.57% | +0.08% | +2.11% | **-0.51%** |
| + `registers` in r4 | +0.20% | +2.24% | +0.35% | +0.44% | +0.88% | +0.81% | +0.35% | +0.75% |

The hoist barely moved them, so the result is stable. The two candidates tie on mean and differ
entirely in shape: `pc` buys a bigger `mandelbrot` but regresses `struct_array_scan` +2.11%, while
`raw_reals` wins four benchmarks including three compute-heavy ones and spends its cost on the
hashing/allocation side. Neither is free -- both reserve a register program-wide, and both regress
`sieve`.

**Both declined.** A -0.5% mean is thin payment for a permanent program-wide reservation, and the
same precedent that reverted PGO, SSO twice and `OP_MOD_POW2_INT` applies to a trade this shaped.
The stronger reason is that pinning treats the symptom: the allocator spills `raw_reals` because the
live set exceeds the register file, and 5.16ya has since shown that *shrinking the live set* helps
broadly where reserving a register helps narrowly and costs everywhere. Reserving a register now
would also make the next live-set reduction harder to evaluate, since it would be measured against
an artificially constrained allocator. Revisit only if a live-set reduction stops being available.

**On x86-64 the whole question is moot**, and for two independent reasons. `r8` there is a *volatile*
argument register under both Win64 and System V -- clobbered by every libc call -- so the ARM
register would be the wrong one anyway; a pin would have to use `rbx` or `r12`-`r15`. More to the
point, nothing needs pinning: x86-64 has a memory-indirect jump, so dispatch compiles to
`jmp *(%rbx,%rax,8)` with the table base already resident and no Thumb-bit fixup. The 5.16s problem
-- rebuilding a PIC table base every dispatch, on a machine with no spare register to cache it in --
is an ARM32 problem, which is why the pin is `#if defined(__arm__)` and why it needs no counterpart.

### 5.16w The program counter is a pointer: dispatch is five instructions

5.16s measured dispatch at eight instructions, three of which did no work, and 5.16t's `r8` pin
removed two of them. Re-disassembling afterwards showed seven, not six -- the pin had also pushed
`code` out of a register and onto the stack, so every dispatch now began by reloading it:

```
ldr    r3, [sp, #44]        <- `code`, reloaded from a stack slot, every dispatch
ldr.w  fp, [r3, sl, lsl #2] <- op_word = code[ip]
add.w  sl, sl, #1           <- ip++
uxtb.w ip, fp
ldr.w  r3, [r8, ip, lsl #2]
orr.w  r3, r3, #1
bx     r3
```

Giving `code` a register of its own does not fix this: 5.16v measured pinning it at **+2.38%**, worse
than reserving nothing. The register budget cannot afford a fourth resident.

The fix is to stop needing two values. `ip` was an offset into `code`, so every fetch had to
reconstruct an address from a base and a scaled index. As a moving pointer -- which is what Lua and
LuaJIT both use -- the fetch is one post-indexed load, and `code` is no longer read on the hot path
at all:

```
ldr.w  fp, [r9], #4         <- op_word = *pc++
uxtb.w ip, fp
ldr.w  r3, [r8, ip, lsl #2]
orr.w  r3, r3, #1
bx     r3
```

Three instructions become one. `vm->ip` stays an offset, because everything outside `vm_run_slice`
-- error line lookup, `CallFrame.return_ip`, yields, `debug_hits[]` -- wants a stable index into a
buffer that can move. The conversions sit at the boundary: `pc = code + target` at a jump,
`(unsigned int)(pc - code)` at a sync.

**The one real hazard is aliasing, and it has exactly one site.** `vm_call_resolve_specialization`
re-enters the parser, which can `realloc` the chunk and move `code` out from under `pc`. That call
already refreshed `code`; it now converts to an offset before and re-derives `pc` after. Nothing
else in the function can grow the chunk, which is what makes a raw pointer safe to hold across
2,700 lines of handler bodies.

Instructions, Pi:

| benchmark | delta | | benchmark | delta |
|---|---|---|---|---|
| `nbody` | **-5.63%** | | `dict_bench` | -1.14% |
| `mandelbrot` | **-5.44%** | | `fib_bench` | -0.87% |
| `struct_array_scan` | **-4.84%** | | `lookup_table_bench` | -0.70% |
| `sieve` | **-2.49%** | | everything else | within ±0.3% |

Cycles agree and in one case exceed it: `struct_array_scan` -8.13%, `sieve` -3.86%, `mandelbrot`
-2.14%, `nbody` -1.81%. The fuzzer is incidental corroboration -- the four runs that previously hit
its 30-second wall-clock backstop now finish inside it.

### 5.16x Hoisting loop-invariant raw constants, and what the preheader costs

A literal used inside a loop is re-materialised into a raw slot every iteration, because the slot it
lands in is clobbered by whatever consumes it. `mandelbrot` reloaded `2.0` and `1` 24.9M times each;
`dict_bench`'s `i = i + 1` reloaded `1` 800,000 times.

The first design considered was a post-emit pass that decodes the loop's emitted range. That was
over-engineered, and the objection raised against it -- that it makes the parser a fourth source of
truth about instruction encoding -- does not apply to what was actually built. **The parser already
knows the hoisted slot at the moment it emits the instruction that reads it**, so no decoding, no
operand table and no liveness analysis is needed. Each loop reserves a fixed preheader gap, and
`raw_materialize` (the single choke point every raw constant load passes through) returns a hoisted
slot instead of emitting a load. The gap is backfilled once the body is parsed.

**Slot allocation is the entire correctness question, and getting it wrong does not fail loudly.**
A temp is freed at the end of its statement, so reserving hoisted slots at the raw floor hands a
constant the slot an *earlier statement in the same loop* uses as scratch -- and that statement
rewrites it every iteration. `total += i * 2` left its product in slot 3, `i = i + 1` then hoisted
its `1` into the freed slot 3, and `i` advanced by `i * 2` forever. The symptom was a hang, not a
wrong answer. Hoisted slots now come from `max_raw_*_used`, the function's temp high-water mark and
the only counter that outlives an individual statement.

| benchmark | delta | | benchmark | delta |
|---|---|---|---|---|
| `mandelbrot` | **-9.12%** | | `struct_array_scan` | -0.29% |
| `small_dict_bench` | -1.30% | | `sieve` | -0.21% |
| `dict_bench` | -1.07% | | `nbody` | **+0.55%** |
| `log_processing` | -0.76% | | rest | ±0.05% |

**Dispatch share badly overestimated this.** The pattern was 13.33% of `dict_bench`'s dispatches and
16.41% of `typed_elementwise`'s, which predicted wins an order of magnitude larger than the -1.07%
and -0.05% measured. A raw constant load is among the cheapest opcodes there is -- five instructions
of dispatch plus two of work -- while these benchmarks' totals are dominated by hashing, allocation
and field access. Removing a tenth of the *dispatches* removes a hundredth of the *instructions*
when the removed dispatches are the cheap ones. Dispatch counts are the right tool for finding
waste and the wrong one for sizing it.

**The preheader is not free.** A loop that hoists nothing still pays one skip-jump per loop ENTRY,
to step over the unused gap. `nbody` hoists nothing and has inner loops entered millions of times,
which is the whole of its +0.55%. Gating the preheader to the while form recovers that exactly
(8.916B, against 8.916B before the hoist existed) but gives back 0.40% of `mandelbrot` and 0.21% of
`sieve`, because range-for bodies do hoist -- just less often, since range-for already subsumes the
single most common loop constant, the increment. The gate was rejected: it buys 0.05% on average by
making an optimisation silently unavailable in one loop form, which is a permanent behavioural wart
standing in for a fixable implementation limit.

**The fix is exact-size insertion, and 5.16w just made it reachable.** The skip-jump exists only
because the gap is reserved before its size is known. Inserting exactly the right number of words
instead would require moving the loop body, which absolute jump targets forbid -- but the program
counter is now a pointer, which is the precondition for PC-relative jumps. Relative jumps make a
loop body position-independent, which makes exact-size preheader insertion a memmove, and shrinks
the jump encoding as a side effect. That is the path forward, not a wider gap.

### 5.16y PC-relative jumps: built, measured, reverted -- and the clearest lottery evidence yet

5.16w removed the `code` rebuild from *dispatch*, but every taken branch still did `pc = code +
target`, and `code` is spilled -- so each branch paid a stack reload. Making control-flow targets
signed deltas from the word after the operand turns that into `pc += delta`, removing the reload
outright. The prediction was a win on anything branch-heavy: `sieve` is 56% loop control, `fib_bench`
22%, `mandelbrot` 14%.

It was built and it worked. `patch_jump` became the single choke point for the new encoding, with
`patch_call_target` split out for the one genuinely absolute case (a call's `callee_offset` is a
function entry, not intra-function control flow). Every jump-like operand in the instruction set is
the last word read before its branch, which is what lets one base serve all sixteen VM sites. Full
gate green, all 16 benchmark outputs identical, disassembly byte-identical once it resolved deltas
back to targets.

It measured worse. Instructions:

| benchmark | delta | | benchmark | delta |
|---|---|---|---|---|
| `fib_bench` | **+0.87%** | | `mandelbrot` | +0.05% |
| `sieve` | **+0.53%** | | `nbody` | -0.25% |
| everything else | within ±0.22% | | | |

**And the reason is not the register lottery, though it looked like it at first.** Counting loads
from one stack slot before and after suggested the reload had been removed -- but the frame layout
changed, so that compared two different variables. Aligning the full histograms shows what actually
happened:

| | baseline | relative |
|---|---|---|
| hot slots (loads) | `44:157  48:291  56:97  68:57  72:62` | `40:157  44:291  48:142  56:98  68:57  72:62` |
| total `ldr [sp]` | **972** | **1064** |

The two hottest spills merely shifted down four bytes, unchanged in frequency. The relative build has
**one more spilled value than the baseline** -- the new slot 48, loaded 142 times -- and 92 more
stack loads overall.

The cause is a liveness change, not an allocation coin-flip. `pc = code + target` is a *pure write*:
`pc`'s previous value is dead, so the branch does not extend its live range. `pc += delta` is a
read-modify-write, which keeps `pc` live across every branch. Meanwhile `code` was not freed at all
-- `SYNC_IP` still needs it for `vm->ip = pc - code`, as do the six absolute sites. So the change
added a liveness constraint without removing a value, and in a function where 153 label bodies share
one register file, that cost more than the load it deleted.

This is worth more than the -0.87% it cost, because it names something 5.16q could only describe as
a lottery: **removing an instruction from a hot path is not the same as removing a value from the
live set, and only the second reliably helps here.**

Reverted. The secondary motivations do not carry it either: relative jumps would make loop bodies
relocatable and so allow the exact-size preheader 5.16x wants, but that is worth `nbody`'s +0.55% on
one benchmark, against +0.87% and +0.53% here. A smaller jump encoding remains a real future prize,
since deltas are small enough to pack into word0 -- but that is a *different* change, and it must be
justified by the packing, not by the addressing.

### 5.16ya The same change, landed: relative jumps paired with a pointer-valued error PC

5.16y's post-mortem named the defect precisely enough to fix it. Relative jumps added liveness --
`pc += delta` is a read-modify-write where `pc = code + target` was a pure write -- without removing
a value, because `SYNC_IP` still needed `code` for `vm->ip = pc - code` at 21 sites. Removing an
instruction from a hot path is not the same as removing a value from the live set.

So remove the value. `SYNC_IP` now stores a pointer, `VM.error_pc`, read only by `lookup_runtime_line`
-- a cold path that can afford `vm->chunk->code` itself. `vm->ip` stays an offset and is written at
only the four yield sites, because an offset is what *survives*: a yield resumes after the caller may
have reparsed (the REPL does exactly this), and a stored pointer would dangle across the chunk growth
that follows. The durable representation and the hot-path representation want to be different things,
which is why one field could not serve both.

With both halves, `code` drops from ~312 references to ~10, all cold:

| build | `vm_run_slice` instructions | `ldr [sp]` sites |
|---|---|---|
| before | 13453 | 972 |
| relative jumps alone (5.16y) | 13483 | 1064 |
| **both together** | **13070** | **922** |

The identical jump change that regressed six benchmarks now improves eight of eleven:

| benchmark | delta | | benchmark | delta |
|---|---|---|---|---|
| `struct_array_scan` | **-1.21%** | | `small_dict_bench` | -0.61% |
| `sieve` | **-1.05%** | | `dict_bench` | -0.10% |
| `binary_trees` | **-0.96%** | | `mandelbrot` | -0.06% |
| `log_processing` | -0.82% | | `fib_bench` | **+0.44%** |
| `nbody` | -0.63% | | rest | ±0.00% |

`fib_bench` is the lone regression and the exception proves the rule: its calls take the absolute
path (`callee_offset`, `return_ip`), so it pays the allocation reshuffle without collecting the
branch benefit.

**A failed follow-up, recorded because the reasoning was wrong rather than the idea.** Five stack
slots hold 70% of the remaining 922 reloads. `gdb`'s `info scope` reports `max_instructions is a
variable in $r1`, which looked like a whole register held for four back-edge checks that never fire;
folding it into the budget with a `UINT_MAX` sentinel measured 922 -> 926 and freed nothing. The
premise was a misreading: that is a DWARF *location* for a parameter live only near entry, and the
other locals read "optimized out" because they have location *lists*, not because they are absent.
Going below 922 needs those lists decoded properly, not inferred from a summary line.

### 5.16yb The call path, and a multiply hiding in every call

`fib_bench` is 44.4% `OP_CALL` + `OP_RETURN` and is the only benchmark that loses to LuaJIT `-joff`
on *both* platforms by the same margin (0.72x on ARM, 0.69x on x86-64) -- every other benchmark
shifts a uniform ~20% between them. A platform-independent loss is a design cost, not a codegen one,
so the call path is where the remaining structural work is.

Working backwards from the totals: `fib_bench` runs 134.4M dispatches for 6.84B instructions, of
which ~29.9M are calls and ~29.9M returns. That puts the call/return pair near **96 instructions**,
about 83% of everything the benchmark executes. Disassembling `lbl_call` shows the first thing it
does:

```
mov.w  r3, #328          ; sizeof(ChunkFunction)
mla    r1, r3, r1, r2    ; &functions[func_index]
```

`ChunkFunction` is 328 bytes, which is not a power of two, so resolving the callee cost a **multiply
on every call** -- and it is entirely avoidable, because the parser knows the index at compile time
and the scale never varies. `emit_call` now emits a byte offset instead, and both `lbl_call` and
`lbl_tail_call` index with an add.

| benchmark | delta | | benchmark | delta |
|---|---|---|---|---|
| `fib_bench` | **-0.43%** | | everything else | within ±0.01% |
| `binary_trees` | -0.11% | | | |

Exactly the shape the change predicts: it touches call-heavy code and nothing else. It also recovers
`fib_bench`'s +0.44% from 5.16ya precisely (6.874B -> 6.844B, its pre-5.16ya figure), so the two
changes together improve eight benchmarks and regress none.

**A cache with no reader.** The `VM` struct mirrored the active frame's `registers`/`raw_ints`/
`raw_reals`, updated on every call *and* every return and then immediately reloaded into the hoisted
locals -- six stores and six loads per call/return pair. Nothing hot read it: `mark_vm_roots` scans
`call_stack[f].registers`, and the only other consumers were a test accessor, a REPL variable dump,
and `vm_call_value`'s tail path, all of which can read the frame directly. The three fields were
deleted outright and every reader now goes to `call_stack[call_depth]`; `lbl_call` and `lbl_return`
already hold the relevant `CallFrame*`, so refreshing the locals costs three loads off a pointer
in hand instead of three loads plus three stores through the VM.

| benchmark | delta | | benchmark | delta |
|---|---|---|---|---|
| `fib_bench` | **-5.25%** | | everything else | within ±0.16% |
| `binary_trees` | **-1.83%** | | | |

The largest single instruction win on `fib_bench` in this section, and again exactly the predicted
shape -- call-heavy benchmarks collect it, nothing else moves, nothing regresses. Worth noting what
made it findable: the field's own comment justified its position in the struct ("touched on every
call and return") as if that were a reason to keep it fast, when it was a reason to ask why it
existed.

**The three cold fields are not worth chasing.** `code_offset`, `tail_calls_collapsed` and
`synthetic_entry` serve only stack traces and tail-call accounting, and two of them are zeroed on
every ordinary push -- so making them an aligned, same-width, adjacent pair should let one `strd`
replace two stores. Tried: GCC emitted no `strd` at all (checked by disassembling `lbl_call`), and
the measurement agreed at -0.01% / +0.00%. Reverted, since it also cost a `bool` its honest type.
The eleven frame writes compile to eleven separate `str.w`; coalescing them is not something the
source can ask for from here.

`vm` itself remains the hottest spill in the interpreter (stack slot 44, 270 reload sites).

The structural item beyond those is the argument copy. AER gives the callee a fresh window
(`callee->registers = caller->registers + caller->frame_size`) and copies arguments into it; Lua
overlaps the windows so the arguments are already in place and the copy disappears. That is the
change most likely to matter for `fib_bench`, and also the one that touches frame layout, GC root
ranges and the parser's register allocation at once.

### 5.16yc Shrinking ChunkFunction 328 -> 60 bytes: reverted, then taken once measurable

`SpecEntry specializations[SPEC_MAX]` is 272 of `ChunkFunction`'s 328 bytes -- **83% of the struct**
-- for a table most functions never use, since only a shape-sensitive parameter triggers
specialization. Behind a lazily-allocated pointer (fixed at `SPEC_MAX`, never grown, so the
`SpecEntry*` `CallSpecCacheEntry` caches stays stable) the struct becomes **60 bytes**, fits one
cache line, and drops peak RSS on `bench/compile_bound.aer` from 3040 KB to 2892 KB.

It was reverted first time round. Instructions said free (every benchmark within ±0.04%), but ARM
cycles said `nbody` +13.18%, then +24.64% on a repeat. The write-up blamed cache locality; the
counters refuted that -- cache-misses *fell*, 80,384 to 71,720, on a benchmark whose working set is
five bodies. The cost was branch mispredictions, 13.5M to 45.7M, and the rule applied was: reject a
change whose instructions show no work removed, whichever way its cycle luck fell.

**The rule was right and the measurement was noise.** 5.16yd then measured the layout band and 5.16yh
found A32. Re-measured against both, the same change reads:

| | `nbody` | `mandelbrot` | `fib_bench` | `sieve` | `binary_trees` |
|---|---|---|---|---|---|
| ARM Thumb-2, first build | +13.2% / +24.6% | -- | -- | -- | -- |
| ARM Thumb-2, later build | **-2.20%** | +3.53% | +8.98% | -0.04% | -0.28% |
| ARM A32 | -0.73% | **-0.44%** | +3.58% | -0.31% | -0.17% |
| x86-64 | -0.21% | -0.45% | -0.42% | -2.55% | -1.15% |

The same comparison read +24.64% and -2.20% on two Thumb-2 builds. That is the lottery, not the
change. Under A32 the `mandelbrot` regression disappears and `fib_bench`'s +3.58% sits inside its own
5.84% band; on x86-64, where bands are 0.7-4.4%, six of seven benchmarks improve slightly.
Instructions remain neutral everywhere (+0.00% to +0.04%).

**Taken.** Not as a speed win -- it is instruction-neutral and honestly reported as such -- but
because 83% of a struct devoted to a table most instances never allocate is worse code, and the only
evidence against it turned out to be an artifact of the noisiest measurement in this document.

### 5.16yd How big the layout lottery actually is: measured, and it is enormous

That whole argument rested on an unmeasured "several percent". `tools/layout_sweep.py` measures it:
it builds the *same commit* at several code offsets (`-DAER_LAYOUT_PAD=N`, which shifts every
following address) and reports how far each benchmark moves with **no source difference at all**.

Five layouts, `HEAD` against itself:

| benchmark | spread across layouts, identical source |
|---|---|
| `mandelbrot` | **25.58%** |
| `fib_bench` | **5.84%** |
| `nbody` | 3.08% |
| `binary_trees` | 0.62% |
| `sieve` | 0.60% |

`mandelbrot`'s cycle count varies by a quarter on identical code. Any single-build cycle comparison
on it is meaningless, and some numbers quoted earlier in this document sit inside their benchmark's
band: 5.16yb's `fib_bench` -5.7% is smaller than fib's own 5.84% lottery, so it is not evidence that
change helped on cycles -- its -0.43% on *instructions* is the real result. `sieve` -3.86% and
`binary_trees` movements above ~1% are well outside their bands and can be trusted.

**Sensitivity is a property of the benchmark, not the machine.** `sieve` and `binary_trees` barely
move; `mandelbrot` moves 25%. The plausible reason is how concentrated the hot loop's opcode mix is:
a loop cycling a handful of dispatch sites lives or dies on whether those few collide in the
predictor, while a loop spread across many sites averages its own luck out. That also explains why
5.16d's "adding an opcode taxes everything" and 5.16q's "register allocation lottery" were both real
observations of one underlying effect that neither could pin down from a single build.

Practical consequences, superseding the cycle guidance in 5.16p:

- **Instructions remain the gate.** Near-immunity to layout is why they are trustworthy, not a
  limitation.
- **A single-build cycle comparison is worth nothing on a layout-sensitive benchmark.** Run
  `layout_sweep.py` and compare the median against the measured band.
- **Quote the band alongside the median.** A median inside it says which build got lucky.

`OP_RAW_ADD_REAL_BOXED` is 13.6% of `mandelbrot`'s dispatches -- `cx` and `cy` staying boxed across
518,400 calls -- so binding a shape-less function's numeric parameters as raw locals keeps looking
like the obvious win. It has been built twice and reverted twice. This time it was costed before
being built, and the arithmetic explains both earlier failures without needing a third.

The working hypothesis was that 5.16x's hoist would rescue it: attempt 2's post-mortem blamed
compared constants being raw-materialised once a parameter went raw, which defeats compare-and-branch
fusion, and a hoisted constant costs one load per loop entry instead of one per iteration. **The
hypothesis is wrong on both halves.** The parser has kept compared literals boxed since 2026-08-02
(`lhs_is_slot && rhs_is_const`, parse_binary_ops), which is a day *before* attempt 2 was measured --
so that fix was already in place and did not save it. And the real blocker is not constants at all.

`mandelbrot`'s hot loop compiles its condition to a single fused dispatch:

```
OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE   rawi=0  rk=reg2  -> 17     [25.4M hits]
```

`iter` is *already* raw; `max_iter` is the boxed operand, and the fusion exists only for the
raw-vs-**boxed** shape. Specialization would make `max_iter` raw too, turning that one dispatch into
`OP_RAW_LT_INT` plus `OP_JUMP_IF_FALSE_REG` -- **+25.4M dispatches**, at roughly 10-15 instructions
each. Against that, the two `OP_RAW_ADD_REAL_BOXED` become `OP_RAW_ADD_REAL`, which changes no
dispatch count at all and saves only a tag check, about two instructions on 49.8M executions. The
trade is ~250-380M instructions spent to save ~100M, on a 7.28B baseline -- a few percent worse,
which is what attempt 2 measured (+7.4%).

So specialization does not fail because of an implementation detail. It fails because **making an
operand raw removes it from the one comparison shape that fuses**, and it buys back only a tag check.

The unlock is a package, not a patch. Raw-vs-raw fused compares would close the gap, and `vm.h`
records them as measured earning nothing -- but that measurement was taken in a codebase where
nothing *produces* raw-vs-raw comparisons, because parameters stay boxed. Each change is worthless
alone and they have only ever been evaluated alone. A fourth attempt should build both or neither.

### 5.16aa Loop-invariant raw constants are worth hoisting; the dead copies are not

`mandelbrot`'s inner loop spends 4 of its 14 dispatches on work that does nothing:

```
26  RAW_LOAD_REAL  r4 = 2.0        <- reloaded every iteration, 24.9M times
28  RAW_MUL_REAL   r4 = r4 * r0
33  RAW_MOVE_REAL  r1 = r4         <- y = y_new
34  RAW_MOVE_REAL  r0 = r5         <- x = x_new
35  RAW_LOAD_INT   r1 = 1          <- reloaded every iteration, 24.9M times
37  RAW_ADD_INT    r0 = r0 + r1
```

These are two separable problems and the evidence separates them cleanly. Dispatch shares:

| benchmark | constant loads | dead copies |
|---|---|---|
| `typed_elementwise` | 16.41% | 0.00% |
| `dict_bench` | 13.33% | 0.00% |
| `mandelbrot` | 12.85% | 12.32% |
| `small_dict_bench` | 10.00% | 0.00% |
| `log_processing` | 7.84% | 0.00% |
| `struct_array_scan`, `sieve` | ~1% | 0.00% |
| `lookup_table`, `fib`, `nbody`, `binary_trees` | 0.00% | 0.00% |

**The dead copies are mandelbrot-only** — 12.32% there and exactly zero everywhere else. They come
from a source-level `y = y_new; x = x_new` swap the programmer wrote, and eliminating them needs
real liveness analysis to prove the producing instruction can target the destination directly.
Narrow benefit, the harder half of the work, and the half where a wrong answer miscompiles
silently. Not worth it.

**The constant loads are general** — five benchmarks between 7.8% and 16.4%, including the three
most realistic workloads in the suite. The pattern is one shape, and it is the same shape in every
case: a literal used inside a loop is re-materialised into a scratch raw slot on every iteration,
because the slot is clobbered by the instruction that consumes it. `dict_bench` is `i = i + 1`:

```
16  RAW_LOAD_INT  rawi1 = 1              [800,000 hits]
18  RAW_ADD_INT   rawi0 = rawi0 + rawi1  [800,000 hits]
19  JUMP -> 8                            [800,000 hits]
```

It survives in exactly the loops range-for does not cover. Every benchmark at 0.00% uses range-for,
where the increment lives inside `OP_ITER_RANGE_LOOP` and no separate load is emitted at all; every
benchmark above 7% has a manual `for cond:` loop. So the pattern looks rare only if the sample
happens to be range-for-shaped — a static count over all 83 `.aer` files in the repo finds it in 47
of them.

Two routes were weighed. Immediate-operand opcodes (`OP_RAW_ADD_INT_K` and friends) fold the
constant into the arithmetic instruction and need no analysis at all, but spend opcode surface,
which 5.16d measured as a real branch-misprediction tax on every program whether it uses them or
not — the same trade `OP_MOD_POW2_INT` was reverted for. A post-emit hoist costs no dispatch
surface but makes the parser decode instructions it did not just emit, which means a fourth source
of truth about instruction encoding alongside `emit_*`, the VM labels, and `disasm.c`. The first
three disagree loudly; a parser that disagrees miscompiles quietly. That risk is containable by
restricting the pass to the raw-slot opcode family and bailing out of any loop containing an opcode
outside it, with a generated coverage check so a newly added opcode cannot silently fall outside
the table.

### 5.16ye One dispatch site or 153: measured, and the answer is per-benchmark

5.16yd showed cycles are dominated by mispredictions on the computed-goto dispatch, and that
`mandelbrot` swings 25% on layout alone. The obvious lever is the *number* of dispatch sites:
`DISPATCH()` expands at the end of every handler, so ~153 indirect branches compete for a
Cortex-A72's finite indirect-predictor entries. `-DAER_SHARED_DISPATCH` builds the opposite endpoint
-- one shared site, verified in the disassembly as exactly 1 `bx` against 153.

| benchmark | instructions | cycles | branch-misses |
|---|---|---|---|
| `mandelbrot` | +6.78% | **-13.50%** | **-90.51%** |
| `binary_trees` | +1.91% | -1.08% | -7.67% |
| `fib_bench` | +2.98% | -3.39% | +47.08% |
| `dict_bench` | +0.98% | +0.21% | +2.97% |
| `nbody` | +5.28% | +16.00% | +152% |
| `struct_array_scan` | +4.27% | +13.73% | **+2299%** |
| `sieve` | +3.82% | +21.31% | **+810%** |

**Replication stays**, and the classic reason holds: a per-opcode branch lets the predictor learn
which opcode tends to follow which, and collapsing that costs `sieve` and `struct_array_scan`
enormously. Instructions rise everywhere too, since a shared site adds a jump per dispatch.

**But `mandelbrot` loses 90% of its mispredictions by having fewer sites**, and that is the same
fact as its 25.58% layout band: its hot loop cycles a handful of dispatch sites that collide with
*each other*, and any layout shift re-rolls whether they do. Replication helps a loop with a varied
opcode mix and hurts one with a tight mix, which is why no single answer wins.

That reframes what "partial replication" would have to do. The tempting version -- share the cold
tail, keep hot opcodes replicated -- targets the wrong end: cold opcodes are not what alias on
`mandelbrot`, its hot ones are. Freeing predictor entries by sharing the tail is still worth a
measurement, but this data says not to expect it to fix the benchmark that needs it most.

The knob stays as opt-in measurement tooling, never part of a normal build, on the same footing as
`make pgo` and `debug-tools`.

### 5.16yf A use-after-free found by reading the call path, not by a test

`vm_run_slice` hoists `c->pool` into `const_pool`, and a shape-specializing recompile runs the
parser again while that local is live. `lbl_call` already refreshed `code` and `functions` across
that call, with a comment saying a recompile "can realloc both" -- so the question is why `pool` was
not in that list.

The reasoning that makes it look safe is real but incomplete. `chunk_add_pool` dedups by value, so
recompiling the same source interns nothing new and cannot grow the pool. **Except for
`TYPE_FUNCTION`, which dedups on `code_offset`** -- and a specialized body's function expressions
are emitted at *new* offsets. A shape-sensitive function containing a lambda therefore appends on
its first specialized call, and `chunk_pool_append` reallocs, leaving `const_pool` pointing at freed
memory for the rest of the slice.

Reachable in ordinary code -- a function that reads a struct field (making it shape-sensitive) and
assigns a `function(...)` expression is nothing unusual:

```
function shape_fn(p, k):
    g = function(a):
        return a * 2
    return p.x + p.y + g(k)
```

**Demonstrated, not just argued.** `tests/test_spec_pool_realloc.aer` exercises the path but passes
either way on a normal build, because the pool usually has spare capacity at the moment of
specialization. Forcing `chunk_pool_append` to realloc on *every* append makes the unfixed build
fail immediately:

```
Error: Cannot apply '*' to integer and Ã@
```

-- a constant read out of freed memory. With the one-line fix, all assertions pass under the same
stress. The test stays as a canary: it covers a combination (specialization plus a lambda in the
same body) that nothing else did, and it will catch a regression whenever the pool does happen to
grow at that moment.

Worth noting how this surfaced: not from a failing test, but from reading `lbl_call` while looking
for something else and asking why one hoisted pointer was refreshed and another was not. The
comment next to the refresh said "can realloc both", and *both* was the bug.

### 5.16yg Outlining cold handlers, and what code alignment actually buys

If mispredictions come from dispatch sites aliasing, the tempting fix is to shrink the code between
them -- outline the ~130 cold opcode bodies so the hot ones pack together. Two cheaper experiments
answer that before committing to a 130-handler refactor.

**GCC's own hot/cold splitting does nothing here.** `-freorder-blocks-and-partition` left
`vm_run_slice` at exactly 13048 instructions, byte for byte. Under `-flto` with a computed-goto
loop there is no profile and no basic-block frequency to act on, so the compiler cannot separate hot
from cold. Manual outlining would be the only route, and this says nothing else will do it for free.

**Alignment does produce a real, reproducible effect -- and it is a trade, not a win.**
`-falign-jumps=64`, swept across five code offsets so the layout lottery cannot fake it:

| benchmark | deltas across layouts | verdict |
|---|---|---|
| `fib_bench` | -8.76, -5.44, -5.51, -5.36, -5.21% | **consistent -5.4%** |
| `nbody` | +0.90, +1.42, +3.03% | consistent +1.8% |
| `binary_trees` | +1.71, +0.67, +0.45% | consistent +0.9% |
| `mandelbrot` | +0.37, +0.23, +2.19% | +0.9% |
| `struct_array_scan` | +1.06, +0.41, -1.43% | neutral |
| `sieve` | +0.28, +0.22, -0.33% | neutral |

`fib_bench` improves on every single layout, and the aligned builds are also markedly more stable
(3.67-3.68B against a 3.885-3.914B spread). That is a genuine effect on call-heavy code, and it cost
402 instructions of padding to get. Everything loop-heavy pays for it.

**Declined as a default**, on the same footing as PGO and the register pins in 5.16v: a concentrated
win against broad small losses, on a knob any user with a call-heavy workload can set themselves.
Recorded rather than deleted so it is not re-derived.

**Which answers the outlining question.** Alignment is the cheap proxy for "does moving code around
reduce mispredictions in a directed way", and the answer is: it produces consistent, workload-
specific effects, helping tight call sequences and hurting tight loops -- not a uniform improvement.
Outlining would be another intervention of exactly that shape, at far higher cost, with no reason to
expect a different distribution. The only lever measured so far that reduces mispredictions
unconditionally is reducing the NUMBER of dispatches, which is what fusion, the constant hoist and
the range-loop work already do.

### 5.16yh The Thumb-bit fixup was not irreducible -- it was Thumb

5.16s catalogued dispatch as eight instructions, three doing no work, and called the Thumb-bit
`orr r3, r3, #1` unfixable: it survives a non-PIE build, so it is not a PIC artifact, and C offers no
way to pre-set bit 0 in a table of `&&label` values. Every later entry repeated that. The word doing
the work in "irreducible" was doing too much: it is irreducible *within Thumb-2*.

`-marm` builds the interpreter as A32, where a branch target needs no mode bit at all. Instructions,
against the Thumb-2 default:

| benchmark | instructions | cycles | branch-misses |
|---|---|---|---|
| `mandelbrot` | **-7.24%** | **-27.46%** | **-97.47%** |
| `fib_bench` | **-6.94%** | -10.23% | -55.26% |
| `nbody` | -5.00% | -2.84% | -6.69% |
| `struct_array_scan` | -3.96% | -5.51% | -8.02% |
| `binary_trees` | -1.96% | +1.05% | -0.32% |
| `sieve` | -1.88% | -1.54% | -1.23% |
| `dict_bench` | -1.25% | +0.51% | +1.79% |
| `log_processing` | -0.75% | +3.79% | +10.95% |

**Instructions improve on every benchmark**, which is the part that needs no defending -- instruction
counts are layout-immune. The cycle wins were then swept across code offsets, because `mandelbrot`'s
own layout band is 25.58% and a single -27% reading would sit inside it: `mandelbrot` measures
-27.34%, -25.46%, -27.09% and `fib_bench` -4.82%, -5.10%, -5.76%, with `log_processing` the only
consistent loser at +0.12%, +3.95%, +0.79%. The A32 builds are also far more stable
(`mandelbrot` 3.60-3.74B against Thumb-2's 4.94-5.03B).

`mandelbrot` losing **97% of its branch mispredictions** is the same fact 5.16ye and 5.16yd kept
circling. Its hot loop cycles a handful of dispatch sites that alias destructively in Thumb-2, where
instruction lengths vary and site addresses land unevenly; A32's uniform 4-byte encoding spreads them
predictably and the collisions stop. That also explains why the benchmark with the widest layout band
is the one A32 helps most -- the band and the misprediction rate were always the same phenomenon.

Cost: binary size 1209 KB -> 1269 KB, under 5%, far below A32's usual 20-30% density penalty --
because most of this binary is one enormous function whose Thumb-2 encoding was already dominated by
32-bit `.w` instructions.

**Not made default here**, because `ARCH_FLAGS` is deliberately empty so a build runs on whatever
machine it is copied to, and A32 does not exist on Cortex-M. It is safe on every `arm-linux-gnueabihf`
target, where it is the single largest win in this section. Build with `make ARCH_FLAGS=-marm`.

### 5.16yi Is -marm fair, and is there an x86-64 equivalent? Both answered by rebuilding

5.16g rejected `-no-pie` on fairness, so `-marm` deserved the same scrutiny rather than an
assertion. It could not be settled by inspecting the distro binaries -- they are stripped, and three
successive tells each measured something else: the ELF entry point reports the C runtime (Thumb on
armhf even in an A32 build), conditional-suffix counts match Thumb IT-blocks too, and `ldr pc, [...]`
matches PLT stubs, which are A32 in every ARM binary regardless.

So Lua 5.4.6 was rebuilt from source **twice, same source, only the ISA differing**:

| benchmark | Lua Thumb-2 | Lua A32 | delta |
|---|---|---|---|
| `nbody` | 5.21s | 4.88s | -6.33% |
| `mandelbrot` | 2.98s | 3.02s | +1.34% |
| `fib_bench` | 2.34s | 2.34s | 0.00% |
| `sieve` | 5.62s | 5.61s | -0.18% |
| `dict_bench` | 3.92s | 4.00s | +2.04% |
| `binary_trees` | 1.76s | 1.75s | -0.57% |

**Lua barely moves.** The flag is equally available to it and buys it almost nothing, while AER gains
up to 27% -- because AER's dispatch is 153 replicated computed-goto sites, each paying the Thumb
mode-bit fixup once. The asymmetry is AER's architecture meeting the ISA, not a rigged comparison,
and any interpreter built the same way would collect the same win. It is also unlike `-no-pie` in the
way that mattered there: no security property changes and the binary stays PIE.

With the ISA stated for both sides (`lua-best` = the better of Lua's two builds):

| benchmark | aer T2 | **aer A32** | lua best | luajit -joff | A32 vs LJ |
|---|---|---|---|---|---|
| `mandelbrot` | 2.71s | **2.03s** | 2.99s | 2.84s | **1.40x** |
| `struct_array_scan` | 3.17s | 3.02s | 8.05s | 6.81s | 2.25x |
| `dict_bench` | 1.23s | 1.23s | 3.93s | 2.57s | 2.09x |
| `sieve` | 2.06s | 2.06s | 5.62s | 3.79s | 1.84x |
| `nbody` | 2.70s | 2.56s | 4.83s | 3.40s | 1.33x |
| `binary_trees` | 1.04s | 1.06s | 1.69s | 1.16s | 1.09x |
| `lookup_table_bench` | 1.62s | 1.67s | 4.31s | 1.70s | 1.02x |
| `fib_bench` | 2.11s | 1.92s | 2.26s | 1.51s | 0.79x |

**There is no x86-64 equivalent, and the reason is that there is nothing to fix.** x86-64 dispatch is
already `jmp *(%rbx,%rax,8)` -- one instruction, table base resident, no mode bit -- against ARM's
five. The measurable levers there are ordinary compiler flags: `-O3` is worth a consistent ~2.7%
(-1.79/-2.79/-3.25% on `mandelbrot` and -2.66/-3.76/-1.89% on `fib_bench` across three layouts), and
`-march=native` another 1-5% at the cost of a binary that no longer runs anywhere. `-O3` is NOT a
universal default: on ARM it is mixed (`nbody` -2.44% but `fib_bench` +0.47%, `sieve` +0.29%).

One incidental finding worth more than either flag: **the layout lottery is an ARM problem.** The same
sweep that moves `mandelbrot` 25.58% on ARM moves it 1.1% on x86-64 (0.609-0.616s across three
offsets). A larger BTB and better indirect prediction absorb what ARM's cannot -- which is why
5.16yd's warning about single-build cycle comparisons applies to the Pi and not to the desktop.

### 5.16yj Every struct construction was doing a linear scan with strcmp

`binary_trees` sits at 0.86x against LuaJIT `-joff`, and a whole session of dispatch and call-path
work never touched it -- its dispatch share is 6.1%, so all of that aimed at the wrong 6%. Profiling
it instead of reasoning about it:

| symbol | share |
|---|---|
| `vm_run_slice` | 57.64% |
| `gc_collect` | 8.75% |
| `vm_struct_field_write` | 8.19% |
| `vm_call_resolve_specialization` | 6.63% |
| `pool_alloc` | 3.90% |
| **`strcmp`** | **1.63%** |

`strcmp` has no business appearing at all. `lbl_struct_new` resolved its type by calling
`chunk_find_shape(c, name)` -- a newest-first linear scan over every shape, with a `strcmp` per
shape -- **on every single struct construction**. `binary_trees` allocates millions of nodes, so
that is millions of string comparisons to answer a question that is constant at each call site.

The name arrives as a pool index, and `chunk_add_pool` dedups strings, so one cache keyed by pool
index serves every site building the same struct -- smaller than a per-site cache and shared across
them. A shape registration clears it wholesale, since a redeclare must stop resolving to the Shape
it shadows; definitions are rare and constructions are not, which is the entire trade.

| benchmark | x86-64 wall | ARM instructions |
|---|---|---|
| `binary_trees` | **-10.27%** | **-4.22%** |
| `dict_bench` | -3.86% | +0.01% |
| `nbody` | -3.18% | +0.11% |
| `struct_array_scan` | -0.56% | -1.18% |
| `mandelbrot` | -0.21% | -0.01% |
| `fib_bench` | +0.44% | -0.00% |

`binary_trees` -10.27% is well clear of its 4.40% layout band, and `nbody`/`dict_bench` clear theirs
too. This is the largest single win of the session and it came from the part of the system nothing
had profiled -- the work *inside* opcodes rather than the dispatch between them. Dispatch is 12.3%
of instructions suite-wide and as little as 2.5% on the allocation-heavy benchmarks; the remaining
money is in the handlers.

`vm_call_resolve_specialization` at 6.63% is the next thread to pull: it is documented as `lbl_call`'s
*cold* path and split out with `noinline` for exactly that reason, so 6.63% means it is not cold at
all for struct-passing code.

### 5.16yk Struct construction wrote its fields the slow way

Next entry down the same profile: `vm_struct_field_write` at 8.19% of `bench/binary_trees.aer`.
`lbl_struct_new` called it once per field of every construction, and it is an *externally linked*
function that re-derives `s->shape` twice per call:

```c
ValueType ftype = s->shape->field_types[slot];
unsigned int offset = s->shape->field_offsets[slot];
```

An inline counterpart already existed -- `vm_struct_field_write_at`, taking offset/type/narrow
precomputed -- used by every field-SET opcode but not by construction, which is the one place that
writes *every* field. And `lbl_struct_new` already holds `shape`, so the two dependent loads per
field were re-deriving something it had in a register.

| benchmark | instructions |
|---|---|
| `binary_trees` | **-5.15%** |
| `struct_array_scan` | **-2.84%** |
| everything else | within ±0.08% |

### 5.16yl A major collection was walking the whole heap twice

`gc_count_live_cells` walks every cell of every slab of every pool to size the next minor threshold.
`gc_run_collection_cycle` called it immediately after a major — and a major sweep *already* visits
every cell and decides each one's fate, so the answer was recomputed from scratch by a second full
pass over data the first pass had just touched.

`pool_sweep` now accumulates survivors into an optional out-param on a major pass. Equivalence was
checked under a temporary assertion comparing the swept count against the separate walk on every
benchmark that majors, with a **negative control** (forcing the assertion true) confirming it
actually executed rather than silently passing — `binary_trees`, `struct_array_scan` and
`log_processing` fire it; `nbody` never majors at all.

The size of the win is worth recording carefully, because instructions and cycles disagree:

| measure | `binary_trees` |
|---|---|
| instructions | **-0.15%** |
| cell visits removed | **893,387** over 32 majors |
| profile share before | 2.32% |

Both are right. The walk reads one byte per cell striding by `stride`, so nearly every visit is a
cache miss for 1/stride of the line fetched — a large share of *cycles* off a small share of
*instructions*. The deterministic cell-visit count reconciles them: ~893K visits at ~4.5
instructions each is ~4M against a 2.68B program, exactly the -0.15% measured.

`cache-misses` could not size the cycle side. A **base-vs-base self-control** at `--runs 5` moved
-5.45% to +12.12% across benchmarks, which swallows the -9.39% the change appeared to show. That
counter needs far more runs than instructions to resolve anything; do not quote it at low run counts.

`gc_count_live_cells` stays for the two callers with no collection to piggyback on: `aer_gc_stats`,
and the memory-ceiling check after a *minor* (a minor deliberately never inspects old cells, so it
cannot produce a whole-heap count).

### 5.16ym Prefetching the sweep walk did nothing, and the reason generalizes

With the redundant walk gone, `gc_collect` is 9.65% of `binary_trees` and **63% of that sits on two
instructions** — the `*state & POOL_FREE` load in `pool_sweep`'s major branch (46.5%) and in its
minor branch (16.5%). A fixed-stride walk missing cache on every cell looked like the textbook case
for a software prefetch, especially since ARM's prefetcher does not reliably latch a ~40-byte stride.

Issuing the miss eight cells early measured, across three code layouts:

| benchmark | median cycles | lottery band |
|---|---|---|
| `binary_trees` | -0.29% | 0.91% |
| `struct_array_scan` | +0.35% | 0.57% |
| `log_processing` | -0.06% | 1.63% |

Every result inside the layout lottery. Reverted.

The reason is worth keeping: **the sweep's iterations are independent**, so the out-of-order engine
already runs many of those misses concurrently. Prefetching cannot add memory-level parallelism that
the loop already has; it only helps when the miss chain is *serial*. That is a general test to apply
before reaching for `__builtin_prefetch` again — and it points at `pool_alloc`, whose free-list pop
(`head -> next`, 61% of that function on one dependent load) is exactly the serial shape prefetching
does help.

Removing the state byte from the cell into a dense side bitmap would cut what the sweep touches by a
factor of `stride`, but it is not obviously a win: `pool_is_young(ptr)` on the write-barrier path
needs O(1) state from a bare pointer, which a side table only gives back via a
`(ptr - slab_base) / stride` division on a non-power-of-2 stride. The in-cell byte is what makes the
*barrier* cheap, and the barrier runs far more often than the sweep.

### 5.16yn The same prefetch, on a serial chain, does work

5.16ym's test predicted where prefetching *would* pay: a chain the hardware cannot overlap with
itself. `pool_alloc` is that shape. It pops a cell and immediately reads that cell's next-pointer to
re-head the free list — and a free cell has been untouched since the sweep that freed it, so the
walk is `head -> next -> next`, one exposed miss per allocation. perf puts **61% of `pool_alloc` on
that single load**.

Issuing the next pop's load before returning gives it the whole of the caller's initialization to
complete in. The first version re-read `free_slab_head` behind a branch; prefetching the value
already in hand one line above is the same effect for a fraction of the cost:

| | instructions, `binary_trees` |
|---|---|
| re-read `free_slab_head` | +0.57% |
| prefetch the value in hand | **+0.25%** |

This trades instructions for cycles, so cycles decide it. Across **5 layouts x 5 runs**:

| benchmark | median cycles | lottery | |
|---|---|---|---|
| `binary_trees` | **-1.43%** | 0.55% | all five layouts negative |
| `log_processing` | +0.32% | 1.88% | within the lottery |

`log_processing` is the cautionary half. At 3 layouts it read **-1.78%** on one variant and
**+2.19%** on the other — a sign flip on nearly identical code. Widening to 5 layouts x 5 runs
collapsed it to noise. A cycle result that changes sign between variants is measuring the layout,
not the change; widen the sweep before believing either number.

So the win is narrow and real: the allocation-heavy benchmark, and nothing else resolved in either
direction. Kept because +0.25% instructions buys -1.4% cycles where allocation dominates, and costs
no measured cycles anywhere else.

### 5.16yo What a call actually costs, measured rather than inferred

Dividing a benchmark's instructions by its dispatch count needs an assumed cost for every *other*
opcode, and that assumption silently carries the whole answer — it produced a "150-180 instructions
per call" figure here that was wrong and that then mis-explained two separate failures. Derive
per-construct costs from a ladder of microbenchmarks differing by exactly one construct instead.

Each of these is a 20M-iteration loop; the delta is the construct (Pi, `perf stat instructions`):

| construct | instructions |
|---|---|
| base loop (`total += 1`) | 57.1 / iter |
| **one CALL+RETURN, 1 arg** (`id(id(1))` minus `id(1)`) | **116.0** |
| each additional argument passed | ~13 |
| shape-sensitive call minus plain call | +108 |
| **boxed tag-checked operand minus raw operand** | **+3.0, and 0 cycles** |

The last row's instruction figure is solid; **its cycle figure does not generalize, and the
microbenchmark that produced it is a trap worth recording.** `OP_RAW_ADD_INT` versus
`OP_RAW_ADD_INT_BOXED` over structurally identical loops measured 1,142,618,051 vs 1,202,481,029
instructions but **identical cycles** (464.9M vs 464.7M, min of 3) — inviting the conclusion that a
boxed operand is free and a raw constant table would be pointless.

That conclusion is unsupported. The counters say why:

| workload | IPC | branch-misses |
|---|---|---|
| mandelbrot | 1.46 | 51,870,508 |
| nbody | 1.89 | 15,082,929 |
| fib_bench | 1.78 | 12,329,346 |
| **the microbenchmark** | **2.59** | **14,070** |

A two-opcode loop is perfectly predictable, so its dispatch never mispredicts and it runs at 2.59
IPC with idle issue slots for the extra instructions to fill. Every real benchmark sits at 1.46-1.89
IPC with millions of misses, where the bubbles come from misprediction *flushes* rather than
dependency stalls — and a flush discards work rather than leaving a slot for it. The regimes are not
comparable in either direction.

**A microbenchmark built to isolate one construct also isolates it from the branch behaviour that
dominates the real workload.** Instruction deltas from such a ladder are trustworthy; cycle deltas
are only trustworthy at a misprediction rate resembling the target. Whether unboxing numeric
constants pays is therefore still open, not closed.

Where a call's 116 instructions go, from `perf annotate` on a call-dominated loop:

| | share | |
|---|---|---|
| `callee->registers = caller->registers + caller->frame_size` | 9.71% | now reads the hoisted base |
| `AerVal result = callee->registers[src_reg]` (return) | 9.99% | 16-byte load |
| argument-copy **loop header** | 7.43% | more than the copy it performs |
| argument copy body | 4.79% | |
| remaining `CallFrame` field writes | ~7.4% | |

`CallFrame` setup is ~17% of a call and the argument-copy loop scaffolding costs more than the
single 16-byte move it makes at arity 1 — both still open.

`vm_struct_field_write` had no callers left afterwards and was deleted.

Worth recording the measurement trap: on x86-64 wall-clock this read `dict_bench` **+9.09%**, well
outside its 2.97% layout band, which looked like a real regression. Instructions -- immune to both
layout and machine drift -- say -0.00%. The x86 bands from 5.16yd bound *layout* variance, not
background load, and a 9-run minimum did not filter it. Instructions remain the gate on both
platforms; the x86 laboratory is better than ARM's, not perfect.

Both wins in this section came from the same place: a hot loop calling a general helper that
re-derives what the caller already knows. Neither is exotic, and neither is in the dispatch path
that most of section 5.16 is about.

### 5.17 String interning would not fix the dict benchmarks (measured, not built)

Lua interns short strings, so a table lookup's key comparison is a pointer compare rather than a
hash plus `memcmp`. AER does not intern, which makes interning the obvious explanation for
`dict_bench`/`lookup_table_bench` lagging, and the obvious thing to build next.

Profiling says otherwise. `lookup_table_bench` (400k lookups over a 500-key table) on the Pi:

| symbol | share | what it is |
|---|---|---|
| `vm_run_slice` | 30.9% | dispatch |
| `aer_format_int` | 20.5% | the `{n % 500}` interpolation's int-to-string step |
| `vm_index_get_compute` | 9.7% | the index path itself |
| `aer_make_string_copy` | 8.8% | allocating the freshly built key |
| `pool_alloc` | 5.0% | that allocation's cell |
| `hashtable_get_hashed` | 4.5% | hashing and probing |
| `memcmp` | 3.2% | the key comparison interning would remove |

Building the key costs roughly 38% (`aer_format_int` + `aer_make_string_copy` + `pool_alloc` +
`memcpy`); comparing it costs 3.2%. Worse, interning is not free on the construction side: every
key here is a fresh string, so each one would still have to be hashed and probed against the intern
table before it could be compared by pointer. The plausible ceiling is a few percent, against a
change that touches `AerString` (whose `inline_buf` short-string optimization pulls the opposite
way), the hashtable's key ownership, and the GC's treatment of the intern table all at once.

`dict_bench` shows the same shape with `gc_collect` at 13.5% on top -- pressure from the same
short-lived key strings.

So the dict benchmarks are dominated by string *construction*, not string *comparison*, and the
lever worth pulling is the interpolation/allocation path (§5.13 already took one pass at
`aer_format_int` and it is still a fifth of this benchmark), not interning.

---

## 6. Known architectural limitations (current, unresolved)

- **RK9-decode cost is real and not eliminated.** The register-vs-constant flag test inside
  `vm_rk_ptr9` remains a measurable cost even after PGO (shrunk, not eliminated — same code shape
  under profiling). An RR-opcode-split (separate register-register and register-constant opcode
  variants, eliminating the runtime branch entirely) and a "unified addressing" alternative were
  both scoped and explicitly set aside — real complexity/opcode-surface cost for an unproven payoff.
- **DONE: SIMD/vectorization for typed arrays; no JIT.** The dispatch loop itself remains
  unvectorizable and always will be — confirmed concretely: GCC's own `-fopt-info-vec` diagnostics
  produce zero output, not even a "missed" message, for `vm_run_slice` at any optimization level,
  because a computed-goto loop over heterogeneous opcode handlers isn't a shape the vectorizer
  analyzes at all. That's fundamental to being an interpreter, not something a flag fixes. But the
  earlier assumption that gather/scatter-free SIMD was blocked by the AoS packed-struct-array layout
  turned out to be the wrong target entirely: `TYPE_TYPED_ARRAY` (added earlier for the repeat-literal
  numeric case) is *already* a flat, contiguous, uniformly-typed buffer — exactly the shape auto-
  vectorization wants, no de-interleaving needed, sidestepping the AoS/gather problem completely
  rather than needing to resolve it. Landed: elementwise `+`/`-`/`*` on two same-kind, same-length
  typed arrays (`vm_typed_array_binary_op`, vm.c), each `(kind, op)` pair its own tight, branch-free C
  loop with zero hand-written intrinsics.
  - **The float/fast-math problem was real, exactly as this entry originally predicted**, and solved
    the way it recommended: fast-math is required to vectorize the float32 pair specifically (NEON's
    float unit isn't strictly IEEE-754-compliant), applied via a *per-function* GCC `optimize`
    attribute — confirmed to work standalone, with no global `-ffast-math` build flag — so every
    other float operation in the language keeps strict IEEE 754 semantics untouched.
  - **A subtlety this entry missed going in**: a per-function `optimize` attribute requesting only
    `"fast-math"` is not enough by itself on a build using this project's own `-O2` flag — plain `-O2`
    vectorizes nothing at all here, integer or float; the tree vectorizer itself only engages under
    `-O3`. Each of the six functions (not just the float32 pair) carries its own
    `optimize("O3","tree-vectorize")` (plus `"fast-math"` for float32), confirmed to override the
    file's global `-O2` standalone, so the six loops vectorize even though the rest of vm.c stays at
    `-O2`.
  - **A second target-portability subtlety, found only by testing the actual shipped binary, not an
    isolated snippet**: NEON is not part of the guaranteed baseline for the generic
    `arm-linux-gnueabihf` target this project's default (flagless) build compiles for — so even with
    the attributes above, the exact same source does **not** vectorize in the real `binary/aer`
    on the Pi without an explicit `-mcpu`. Confirmed by disassembling the actual compiled function:
    plain scalar `ldr`/`str` by default, real `vld1`/`vadd.f32 q8, q8, q9`/`vst1` NEON instructions
    with `make ARCH_FLAGS=-mcpu=native`. x86-64 has no such gap — SSE2 (128-bit SIMD) is part of the
    baseline x86-64 ABI itself, so the identical source vectorizes on the Windows/MinGW build with no
    flags at all (confirmed via disassembly: `movdqu`/`paddd %xmm0`). Rather than force a
    hardware-specific flag into the default build (which must run on whatever ARM/x86 machine it's
    copied to — a NEON-less older ARM board would `SIGILL` on a binary built assuming NEON), added an
    empty-by-default `ARCH_FLAGS` makefile variable so a builder targeting one known machine can opt
    in (`make ARCH_FLAGS=-mcpu=native`) without changing what the portable default build guarantees.
  - **Honest end-to-end numbers, not just the isolated loop's own speedup.** The raw elementwise C
    loop itself measures ~2.2-2.4x faster with vectorization on cache-resident arrays (confirmed via a
    standalone microbenchmark with dead-code-elimination carefully ruled out — an early version of
    that benchmark showed a bogus ~20x from the compiler hoisting repeated identical calls entirely
    out of the loop, caught by checking the disassembly for the function's actual presence). But
    `bench/typed_elementwise.aer`'s *interpreted, end-to-end* run (16,384 elements × 5,000 passes,
    each pass allocating two fresh result arrays) showed only a ~2% difference between the portable
    default build and `ARCH_FLAGS=-mcpu=native` on the Pi — VM dispatch and per-call array
    allocation/GC pressure dominate the total cost at this scale, diluting the vectorized loop's own
    contribution to a small fraction of each call. The win is real but workload-shaped: it matters
    most for larger, compute-bound (not memory-bandwidth-bound — the benefit also vanishes for arrays
    much bigger than cache, confirmed separately) elementwise work relative to the fixed per-call
    interpreter overhead, not for many small operations. No JIT: unchanged, out of scope for this
    session's work.
- **DONE: `vm->registers`/`raw_ints`/`raw_reals` hoisted to locals in `vm_run_slice`, same treatment
  `vm->ip`/`c->pool` already got.** Found via instruction-level profiling (`perf annotate` on
  `bench/nbody.aer`, the canonical 5-body/500,000-timestep shootout benchmark — asked for
  specifically to answer "how many CPU instructions does each opcode actually take, not how many
  opcodes get dispatched"): `vm->raw_reals`'s own pointer reload (a field read fresh from the `VM`
  struct on every dispatch that touches a raw register, rather than cached once per call the way
  `ip` already is) was among the single hottest instructions in the whole hot loop. `nbody.aer`
  itself is a useful contrast to `struct_array_scan.aer`'s GC-bound profile — confirmed via
  `perf stat` (99.15% in `vm_run_slice`, near-zero L1-icache miss rate, no GC/allocation at all) that
  this benchmark's cost is pure scalar dispatch/arithmetic, not memory or GC, so none of this
  session's other GC work could have moved it. `register_stack`/`raw_int_stack`/`raw_real_stack`
  (`vm.h`) are fixed-size inline `VM` arrays, never reallocated, so caching these three pointers in
  registers across the whole dispatch loop is safe as long as the locals get refreshed at the exact
  3 sites the fields themselves get reassigned (`lbl_call`, `lbl_call_value` via `vm_call_value`,
  `lbl_return`) — confirmed by checking each site individually rather than assuming. Caught one real
  ordering bug of its own while wiring this up: a mechanical find-replace initially left `lbl_return`
  writing the callee's result into the *stale* (still-callee-pointing) local instead of the
  just-reassigned caller's, which would have corrupted whichever register of the caller's frame
  happened to share the destination index. Fixed by refreshing the locals before that write, not
  after. Measured on the Pi, 3 runs each way: baseline 3.33-3.42s → 2.92-3.10s, a consistent
  **~11% wall-clock win** from a single, targeted, zero-new-opcode fix — deliberately the shape of
  change favored here over adding opcode-fusion surface (a raw `sqrt` opcode and a fused
  get-field-and-subtract opcode were also identified as real, evidence-backed opportunities in this
  same hot loop, worth ~4-5x and ~2x their own dispatch cost respectively, but set aside for now in
  favor of tuning what already exists rather than growing the opcode surface).
- **Checked: icache is not a real bottleneck anywhere in this project's own benchmark suite.**
  Explicitly re-examined (not just assumed from the `nbody.aer` finding above) via a `perf stat`
  sweep of every benchmark in `bench/` with `L1-icache-load-misses`/`L1-icache-loads`: every single
  one is under 0.15% miss rate (`dict_bench`/`lookup_table_bench` highest at ~0.11-0.12%, most others
  at 0.01-0.06%). This is consistent with, and helps explain, the small-string-optimization TODO's
  own 140:1 *dcache*:icache ratio finding above — the imbalance there is data-cache pressure from
  scattered small heap allocations, not instruction-cache pressure; icache itself was never the
  problem in either case. Nothing to fix here.
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
- **DONE: O(n²) minor-GC rescan of a large, repeatedly-grown array OR dict.** Fixed in two
  separate, additive passes, since real profiling on the Pi found two distinct causes stacked on top
  of each other, not one.
  - **Pass 1 — card marking (`gc_barrier_array`/`gc_barrier_dict`, gc.c).** The remembered-set
    replay for a promoted-old array/dict holding a young reference used to rescan its *entire*
    current contents on every subsequent minor GC, for as long as it stayed remembered (entries are
    never proactively removed) — `for (j = 0; j < a->count; j++) worklist_push(...)` regardless of
    which indices actually changed. For a container built via many incremental insertions
    (`append()`, or repeated `dict[key] = value` with new keys), each of the ~(N / minor_gc_threshold)
    minor GCs across construction rescanned everything built so far, not just what's new — total cost
    degraded to roughly O(n²/threshold). Fixed with a JVM/.NET-style write barrier: `AerArray`/
    `AerDict` gained a lazily-allocated per-index dirty-bit array (`dirty_cards`), set only for the
    exact index written; the remembered-set replay walks only dirty bits, then clears them.
    `collection.insert`/`delete`/`sort` (which shift element-to-index correspondence) fall back to a
    coarser `dirty_all` flag instead of tracking per-index state through a reorder. `REMEMBERED_STRUCT`
    was never affected — field count is fixed at struct definition, small, bounded by
    `MAX_STRUCT_FIELDS`. Verified via `tests/test_card_marking.aer` (scattered writes across two
    separate rounds of further GC pressure, plus the `dirty_all` fallback paths).
  - **Pass 2 — freeze the old generation during a minor mark phase (gc.c).** Card marking alone
    didn't close the gap: a `perf record` flat profile of `bench/struct_array_scan.aer`'s
    `make_particles(2_000_000)` setup phase, taken *after* card marking landed, still showed
    `gc_collect` (38.24%) + `pool_clear_marks` (35.14%) + `pool_sweep` (15.94%) ≈ 90% of all cycles,
    with the actual interpreter (`vm_run_slice`) at only 6.36%. Root cause: `mark_vm_roots`/
    `mark_value` re-descended into every already-old object's full contents on *every* minor cycle,
    regardless of card marking — card marking only sped up the *remembered-set replay*, not this
    separate, unconditional root-tracing path, so a live old array reachable from a register was
    still walked element-by-element every cycle. Fixed by threading a `minor` flag through
    `worklist_push`: when tracing a minor cycle, an already-old cell is never pushed for recursion at
    all — correct because the *only* legitimate old→young edge is a write that went through the
    barrier, and that's exactly what the remembered-set replay (pass 1, above) already covers
    separately. This in turn made it safe for `pool_clear_marks` to also skip old cells on a minor
    cycle (their mark bit is never set or read there anymore), closing the loop. Re-profiling the
    identical workload after this fix: total cycles for the same 2,000,000-particle/50-pass run
    dropped from ~506 billion to ~225 billion (`perf record`'s own event count, same hardware, same
    command) — a ~55% reduction — and total wall-clock (construction + benchmark) fell from the
    original ~307-second construction-phase baseline to ~105 seconds of construction (~124s total
    including the benchmark's own ~19s internal timer). GC-related symbols (`pool_clear_marks` +
    `pool_sweep` + `gc_collect`) still account for the large majority of cycles on this specific
    *worst-case* benchmark (a huge array of long-lived structs, repeatedly promoted and never
    reclaimed) — `vm_run_slice`'s own share only grew because the *denominator* shrank, not because
    GC work vanished. `bench/nbody.aer`, by contrast, shows 98%+ in `vm_run_slice` with no GC
    symbols in the profile at all — the remaining GC cost here is real but workload-specific, not a
    universal tax. Verified via a differential test (`tests/test_memory_gc.aer`, section 37): a value
    written into an old container reachable *only* through another old container, where the inner one
    gets its own independent write-barrier trigger after both are promoted — the case this specific
    change makes newly relevant, since an old object's own children are no longer re-traced by the
    ordinary mark phase at all.
  - **Pass 3 — per-slab skip in `pool_clear_marks`/`pool_sweep` (pool.c/pool.h).** Pass 2 stopped the
    mark phase from *recursing into* an old object's contents, but `pool_clear_marks`/`pool_sweep`
    still touched every individual cell's state byte every minor cycle just to confirm it was old —
    for `struct_array_scan.aer` specifically, `make_particles(2_000_000)` means 2,000,000 separate
    `AerStruct` cells in `struct_pool` (one per `Particle()` call, all held live in one array), and
    that flat per-cycle scan cost was the residual ~90%-GC profile pass 2 left behind. Fixed with a
    per-slab live-young-cell count (`Pool.slab_young_count`, parallel to `slabs[]`): once every cell
    in a slab is old or free, `pool_clear_marks`/`pool_sweep` skip that slab's cells entirely in O(1)
    instead of visiting each one. Deliberately does not try to track which slab a free-list-*reused*
    cell lands in (would need either per-cell metadata — a real memory-density cost on every object in
    every pool — or an address-range search on the hot `pool_alloc` path, for exactly the churny,
    reuse-heavy pools this session's earlier segregated-bitmap experiment already found not worth
    it, per this same header's comment): the first reuse in a pool just permanently disables the skip
    for that pool (`Pool.reused`), falling back to today's full scan — safe (never wrong, only leaves
    the optimization on the table for mixed grow/free/reuse workloads), and free of cost for churny
    pools since they never engage the skip logic beyond the one-time flag check. Re-profiling the same
    workload after this fix: total cycles dropped again, ~225 billion → ~92 billion (another ~59%),
    and total wall-clock (construction + benchmark) fell from ~124s to ~52s. `vm_run_slice` — the
    actual interpreter — is now the single largest symbol in the profile (35.6%), with
    `gc_collect`+`pool_clear_marks`+`pool_sweep` together at ~53.6% (still the majority on this
    specific worst-case benchmark, but no longer ~90%). Combined effect of all three passes on the
    original ~506-billion-cycle / ~307-second-construction baseline: **~82% fewer cycles, ~5.9x faster
    wall-clock**. Verified with the full four-suite regression on both Windows and the Pi after each
    pass, including `tests/test_dict_pool_stress.aer` specifically (the churny, free-list-reuse-heavy
    case this change is designed to leave untouched).
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
  would apply.

  **Tried and reverted.** Small-string optimization (inlining short strings' bytes directly into
  `AerString` instead of a separate heap cell) is the standard, well-proven fix for this access
  pattern elsewhere (V8, LuaJIT, Swift) — but a real implementation and honest `perf stat`
  measurement on the Pi found it a **~30% regression** on `log_processing.aer` (10.8B → 14.0B
  cycles), not a win, so it was reverted rather than kept as a paper improvement. Root cause: it
  isn't the string logic itself (the allocation-avoiding path was ~0.6% of cycles) — it's an
  interaction with the per-slab GC skip above. `string_pool` is a *high-churn* pool for this
  workload (every line's `split()` results die and get reused every iteration), and the per-slab
  skip explicitly, permanently disables itself the moment any cell in a pool is reused from the
  free list (see pass 3's own `Pool.reused` comment) — so `string_pool` never benefits from it,
  full-scans every cycle regardless. Adding inline storage to every `AerString` (even at a
  deliberately small 15-byte threshold, and even after fixing a real design flaw where the first
  attempt didn't avoid an allocation at all — every `aer_make_string` call site already pre-allocates
  a buffer before calling it, so a second constructor, `aer_make_string_copy`, was needed to actually
  copy from a stack/borrowed source instead) made every cell in that always-fully-scanned pool
  bigger, for no offsetting benefit. `struct_array_scan.aer` (no strings) was confirmed unaffected —
  this is specific to string-churn-heavy code, which is exactly the workload SSO was meant to help.
  A real fix would need to attack the underlying issue first: make the per-slab skip (or some other
  mechanism) actually help high-churn pools, not just growth-only ones, before a bigger `AerString`
  cell stops being a pure liability. Not attempted — a separate, nontrivial GC design question, not
  a quick follow-up to the SSO work itself.

  **Independent confirmation the high-churn-pool gap is real and not string-specific**: a broad
  `perf stat` sweep across every benchmark in `bench/` (prompted by "are there other benchmarks
  where we're lacking") found `bench/binary_trees.aer` — "the one benchmark in this suite that's
  actually about the GC" per its own comment, building and discarding many `TreeNode` structs, most
  dying young — has by far the worst IPC in the suite (0.45, versus 1.4-1.9 everywhere else) and the
  identical `pool_clear_marks`+`pool_sweep`-dominated profile (~75% of cycles) `struct_array_scan.aer`
  had *before* the per-slab-skip fix. `struct_pool`'s constant churn here permanently disables that
  skip the same way `string_pool` did for SSO — confirming this is a general high-churn-pool
  limitation, not something specific to strings.

  **DONE: the prerequisite itself — per-slab free lists, matching Luau's actual GC design.** The
  user pushed back on patching around the shared-free-list limitation (tagging a slab index onto
  reused cells while keeping one free list per pool) and asked specifically how Lua/Luau handle
  this. Verified from source rather than assumption: stock Lua 5.x embeds `next` (pointer) + `tt` +
  `marked` in every object (`CommonHeader`) and sweeps via an intrusive linked list. Luau —
  considered the more efficient of the two — does *not* avoid an embedded GC byte; its
  `CommonHeader` is `tt + marked + memcat`, three bytes, *more* than AER's one. What Luau actually
  did was remove Lua's linked list and move to page-based scanning (`sweepgcopage()`), much closer
  to AER's own slab/pool design than to stock Lua — and critically, `lua_Page` has its own per-page
  free list (plus a `busyBlocks` live-count), not one shared across pages, so a reused block's page
  is never ambiguous by construction. That's the actual mechanism AER's pools were missing: one
  shared free list per pool, not per slab, which is exactly why a reused cell's slab was unknowable
  without a search.

  Implemented directly (not the smaller tag-based patch): every `Pool` now has per-slab free lists
  (`slab_free_list[]`, parallel to `slabs[]`) plus an O(1) "which slabs currently have a free cell"
  thread (`free_slab_head`/`slab_free_next[]`), mirroring `lua_Page`'s own free list + `prev`/`next`
  page-linking. Confirmed (via two independent design reviews, one for an earlier tag-based draft
  and one for this design) that `pool_free()` is called from exactly 3 sites in the whole codebase:
  internally inside `pool_sweep`'s own loop (where the slab index is already the loop variable, free
  to use) and externally from `hashtable.c`'s own key/sparse-array pools, which `gc_collect` never
  passes through `pool_clear_marks`/`pool_sweep` with `young_only` at all — so those two pool kinds
  are provably unaffected and needed zero changes. Since AER has no manual/explicit object
  destruction (cells only die via a GC sweep finding them unreachable), *every* GC-tracked pool's
  frees now flow through the new per-slab path, making `slab_young_count[]` always exact — the old
  `Pool.reused` "give up forever" fallback and its skip-check guards were removed entirely as dead
  code, nothing sets it anymore.

  Verified with the full four-suite regression (Windows + Pi), a new differential test
  (`tests/test_pool_churn.aer` — a `binary_trees`-shaped build-and-discard pattern across structs,
  arrays, and dicts, plus a reuse-correctness check that every reused cell holds exactly its own
  fresh value, not a stale leftover), and a clean ASAN run on the Pi (this touches raw pointer-offset
  math in the allocator hot path).

  **Real numbers, both directions.** `binary_trees.aer` on the Pi: `pool_clear_marks`+`pool_sweep`+
  `gc_collect`'s combined share dropped from ~77% of cycles to ~34%, total cycles from 9.49B to
  3.30B, wall-clock from 5.26s to 1.79s — **a ~2.95x speedup that flips the 3-language comparison
  from a loss to a decisive win** (0.67x → ~2.0x over Python, 0.62x → ~1.96x over Lua).
  `log_processing.aer`: wall-clock 6.06s → 3.27s (~1.85x), and the 3-language ratio improved
  substantially (0.17x → 0.34x vs. Python, 0.41x → 0.80x vs. Lua, nearly a tie with Lua now) but
  **remains a loss against both** — its profile still shows `gc_collect`+`pool_clear_marks`+
  `pool_sweep` at a combined ~52% of cycles, plus real `malloc`/`free` overhead from actual string
  *payload* allocations (`_int_malloc`/`_int_free`/`cfree` visibly in the profile). That's the
  separate, already-documented no-SSO limitation just above, not this fix's target — `log_processing`
  simply allocates strings at a far higher *rate* than `binary_trees` allocates structs, so it still
  spends more absolute time in GC bookkeeping even with per-slab scanning now fully efficient.

  **Take 2: tried again on top of the resolved prerequisite, real measurement, still not landed.**
  Re-implemented the same way (inline `AerString.inline_buf`, `aer_make_string_copy` for
  borrowed/stack sources, every hot call site in `vm.c`/`lexer.c`/`parser.c`/`aer_string.c`/
  `aer_collection.c` converted), with the take-1 `chunk_add_pool` landmine (freeing an inline
  string's own `inline_buf` address instead of skipping it) fixed proactively this time instead of
  found via crash. Full four-suite regression + ASAN clean on both Windows and the Pi.

  At the original 15-byte inline threshold, `log_processing.aer` got *worse*, not better: 3.27s →
  3.79-3.86s. `perf record` showed why — `malloc`/`free` overhead did drop as designed (~14% → ~4%
  of cycles), but `gc_collect`+`pool_clear_marks`+`pool_sweep` rose from ~59% to ~71%, a net loss.
  Root cause this time isn't the churn-skip mechanism (that prerequisite really is fixed) but a new
  one: `AerString` grew from 24 to 40 bytes (adding a 16-byte `inline_buf` on top of `gc_state`+
  `data`+`length`), and scanning bigger cells costs more per GC cycle than the avoided allocations
  save, on a workload that's dominated by allocation *rate* in the first place.

  Tuned `AER_STRING_INLINE_MAX` down to stay inside the same struct-alignment bucket as the original
  (no `inline_buf` at all): at 15 the struct rounds up to 40 bytes; at 11 or 7 it rounds to 32; only
  at 3 or smaller does it stay at 24 — identical to the original, zero-growth size. Measured all
  three: 15 → 3.79-3.86s (worse), 7/11 → 3.31-3.47s (still worse than 3.27s baseline), 3 → 3.09-3.12s
  (a real ~5-6% win, the only setting where growth is actually zero). But at `MAX=3` — still only
  inlining 3-character-or-shorter strings — `struct_array_scan.aer` (a benchmark with no meaningful
  string activity in its hot loop) picked up a small, repeatable ~2% regression across three paired
  runs each side (49.85s → 50.89s average), most likely an icache/code-layout tax from the `vm.c`
  changes rather than anything string-pool-size-related. `binary_trees`/`dict_bench`/
  `small_dict_bench`/`fib_bench` showed no measurable change either way.

  **Take 2 deferred, not landed at the time.** A ~5-6% win on the one benchmark this exists for,
  paid for with a ~2% tax on an unrelated one plus the `aer_make_string`/`aer_make_string_copy`
  dual-API surface and the `chunk_add_pool` landmine risk, was a mixed result, not a clean win — and
  it was the second time this specific idea had come back marginal after a real prerequisite fix
  looked like it should have unblocked it. The full take-2 diff (converted call sites, `MAX=3`
  tuning included) was kept as a git stash on the `fixed-width-opcodes` worktree, tagged
  `sso-take2-deferred-mixed-result-2026-07-28`, rather than landed or discarded, against a future
  pass finding either the actual source of the `struct_array_scan` tax or a different target
  workload that changes the cost/benefit balance.

  **Take 3: landed.** That future pass came from profiling a different benchmark family than the
  one take 2 was tuned for. `perf record -e cycles` + `perf report --stdio` against `dict_bench.aer`
  (large numbers of short-lived, short (`"key_0"`..`"key_199999"`-shaped) dict-key strings, a
  pattern take 1/2 never targeted) showed malloc/free/`gc_collect`/`aer_format_int`/`memcmp` combined
  at over 43% of cycles — the ephemeral-key-string allocation rate `dict_bench`/`small_dict_bench`/
  `lookup_table_bench` all share is exactly the cost class inlining removes. Re-applied the preserved
  take-2 stash on top of current `HEAD`, resolving conflicts by keeping the stashed (SSO) side and
  converting the same `xmalloc`+`memcpy`+`aer_make_string` call sites to `aer_make_string_copy`
  throughout `vm.c`/`lexer.c`/`parser.c`, with the take-1 `chunk_add_pool` landmine still fixed
  proactively as in take 2. Chose `AER_STRING_INLINE_MAX = 11` — a third, independent threshold
  from take 2's 15/3, sized for the actual target key strings rather than for the struct-alignment
  boundary alone (11 still rounds `AerString` to the same 32-byte bucket as 7, but covers the target
  keys, most of which run 5-11 bytes).

  Measured on the Pi (`perf stat`, `performance` governor, 2 samples/side): `dict_bench` -22.5%
  instructions, `small_dict_bench` -10.0%, `lookup_table_bench` -22.2% — plus an unexpected bonus win
  on `log_processing` (-17.0%), the exact benchmark take 2's `MAX=15`/`MAX=7`/`MAX=11` settings had
  all made *worse*, now improved instead, most likely because the rest of the session's since-landed
  GC/allocator work (per-slab free lists, adaptive minor-GC threshold) changed the balance take 2 was
  measured against. `struct_array_scan` — the one benchmark that regressed under every prior
  attempt — showed only noise-level movement this time (+0.04%), not a repeat of the ~2% tax. Full
  four-suite regression (Windows + Pi) green, a comprehensive compile sweep across every `.aer` file
  in the repo clean, ASAN clean on the 3 target benchmarks (one pre-existing, unrelated 8-byte
  `main.c:58` CLI-arg leak, not a new one), and a 300-iteration ASAN fuzz run on the Pi (0 crashes;
  the 1 hang found was reproduced identically against the exact pre-SSO commit via `git archive`,
  confirming it predates this change). Cross-language ratios improved substantially on all three
  target benchmarks against Luau (e.g. `lookup_table_bench` from 85% behind to 44% behind).

  **Follow-up: `AER_STRING_INLINE_MAX` raised 11 -> 15, still free.** Compiling `struct AerString`
  at each threshold showed `sizeof` holds at 32 bytes anywhere from 7 through 15 (take 3's field
  order — `gc_state, length, data, inline_buf` — packs `length` into what would otherwise be
  padding ahead of the 8-byte-aligned `data` pointer; it only jumps to 40 bytes at 16). Take 2's
  struct, by contrast, used `gc_state, data, length, inline_buf`, which hits 32 bytes already at 7
  and 40 at 15 — the real reason take 2's own `MAX=15` attempt taxed `struct_array_scan`: that cost
  came from a bigger cell, not from inlining more bytes, and take 3's layout doesn't pay it. 15 is
  the top of the current free range, and `log_processing.aer`'s `path_counts` keys give it real
  work: `"/favicon.ico"` (12 bytes) and `"/static/app.js"` (14 bytes) both missed the original
  11-byte cutoff and were still heap-allocating every line under it. Measured on the Pi: `dict_bench`/
  `small_dict_bench`/`lookup_table_bench` unchanged (their longest strings already fit under 11),
  `struct_array_scan` unchanged (confirming the cell really doesn't grow), `log_processing` a
  further ~0.76% instruction-count win on top of the -17.0% above, and a spot check of
  `binary_trees`/`fib_bench`/`mandelbrot`/`nbody`/`sieve` (no meaningful string activity) all
  unchanged. Full test suite green on Windows and the Pi.
- **DONE: array-reserve builtin — `collection.reserve(arr, n)`.** Mirrors `hashtable_reserve`'s
  existing dict contract: pre-sizes `items`/`capacity` once, `xrealloc` immediately to `n` rather
  than doubling on every overflow, a no-op if already big enough, never shrinks. Verified with a
  differential test against plain `append()`-only construction. Complementary to, not a fix for, the
  O(n²) minor-GC rescan work above (this is about reallocation cost, not rescan frequency).
- **DONE: opt-in 32-bit (`int32`/`float32`) fields for structs**, via the same `i`/`f` literal-suffix
  convention as narrow typed arrays (`x = 42i`, `y = 0.0f` inside a `struct` body — see
  [Narrow Struct Fields](README.md#narrow-struct-fields)). Landed in two passes. First
  (correctness-only): `Shape.field_offsets[]` (vm.c, `OP_DEFINE_STRUCT`'s handler) already was a
  genuine per-field cumulative sum, not a fixed stride, so a third (4-byte) case slotted in
  directly; `Shape.field_narrow[]` records which fields are narrow, threaded through the
  field-access inline cache (`FieldCacheEntry.narrow`) so `vm_struct_field_read_at`/`write_at` --
  the one path every ordinary (non-specialized) `.field` opcode uses -- can dispatch correctly.
  Second (the raw-opcode fast path + packed-array support, landed once real interest in measuring
  nbody.aer's narrow-field performance justified the work): every `field_count * 8` packed-array
  element-size computation (10 call sites across `lbl_index_field_get/set/compound` and
  `lbl_array_repeat`) became `shape->instance_bytes`, the real per-element stride, so a struct with
  a narrow field packs fine now (`[Point(); n]`); and shape specialization's raw-unboxed-local fast
  path gained 12 narrow counterparts of the whole `OP_FIELD_GET_RAW_INT/REAL` family
  (`OP_FIELD_GET_RAW_INT32/FLOAT32` etc.) -- these widen a field's 4-byte storage into the same
  int64_t/double `raw_ints[]`/`raw_reals[]` slots every raw arithmetic opcode already uses, and
  narrow the result back down on write, so a narrow field specializes exactly like a wide one would.
  `shape_find_field` (parser.c) reports a field's narrowness precisely so all 5 specialization
  call sites can select the right opcode variant. One real bug found and fixed along the way: the
  debug-tools disassembler's `OP_DEFINE_STRUCT` decode indexed a type-name array with the RAW
  (un-masked) field-type word, an out-of-bounds read/crash the moment a narrow field's `0x100`
  marker bit was set -- masking it out (mirroring the real VM decode) fixed it. The narrow int32
  raw-opcode SET/COMPOUND family does not range-check on overflow (silently truncates), matching
  every other raw arithmetic opcode's existing "raw means unchecked, for speed" convention -- unlike
  the boxed path's own `vm_check_narrow_field_write`, which does check.
- **DONE: 32-bit-safe fast path for integer modulo.** This build targets 32-bit ARM (armv7l Pi)
  with `ARCH_FLAGS` deliberately empty (portable by default, see the makefile). `perf record -e
  cycles` on `lookup_table_bench.aer` (`n % 500`, both operands always well inside int32 range)
  showed `__udivmoddi4` -- the 64-bit software modulo libgcc falls back to here -- at 8.2% of
  cycles. Compiled `a % b` at both widths on the actual target rather than assuming: even 32-bit
  modulo isn't a hardware instruction on this toolchain's default target (no guaranteed
  integer-divide extension without `-mcpu`), but `__aeabi_(u)idivmod` (32-bit software long
  division) is meaningfully cheaper than `__aeabi_ldivmod` (64-bit). `aer_mod_int64()` (`vm.c`) now
  does the modulo at 32-bit width and widens back when both operands fit `int32_t` -- used by both
  the boxed `OP_MOD` path and the raw `OP_RAW_MOD_INT` fast path. `rv != -1` is a required guard,
  not incidental: `INT32_MIN % -1` is undefined behavior (32-bit division overflow) even though the
  identical values are fine at 64-bit width. Measured on the Pi: `lookup_table_bench` -3.5%
  instructions, `log_processing` (also uses `%`) a further -1.3%; every benchmark with no modulo in
  its hot loop unchanged.
- **DONE: pool the hashtable's dense entry array.** Profiling `small_dict_bench.aer` (perf record)
  after the modulo fix still showed `pool_alloc`/`hashtable_key_dup`/`hashtable_put_hashed` plus
  `gc_collect`/`hashtable_free`/`_int_malloc`/`_int_free` dominating -- `rec = {"id": i, "name":
  ..., ...}` builds a brand-new `HashTable` every iteration. The sparse probe array and every key
  already come from `HashPools`' size-classed tiers (see `2.2`/dict-payload-pooling); the dense
  entry array (`HashTableEntry[]`) was the one piece still going through plain `xrealloc`/`free`.
  Added `DENSE_TIER_CAPACITY = {4,8,16,32,64,128}` (entry counts, matching `dense_grow_if_needed`'s
  own doubling sequence exactly, so `dense_capacity` always lands on a tier boundary) with the same
  alloc-new/free-old shape `sparse_array_alloc/free` already uses. First attempt regressed
  `dict_bench.aer` (+3.1% instructions): forcing every growth step through alloc-new+memcpy+free-old
  unconditionally throws away `xrealloc`'s ability to extend a large standalone allocation in place,
  which matters for a table that grows one entry at a time with no `hashtable_reserve` up front (its
  actual pattern, climbing well past the largest tier). Fixed by keeping plain `xrealloc` once both
  the old and new capacity are already past the last tier -- only the climb through the tiers
  themselves pays the copy. Measured on the Pi: `small_dict_bench` -11.4% instructions,
  `dict_bench`/`lookup_table_bench`/`log_processing`/`struct_array_scan` all within noise of the
  modulo-fix-only numbers (confirming no regression and that `lookup_table_bench`'s 500-entry table,
  past the tier ceiling, correctly gets no extra benefit here). ASAN clean (one pre-existing,
  unrelated `main.c:58` leak, not new) plus a 200-iteration fuzz pass.
- **Considered, declined: sharing constant dict-literal keys instead of duplicating them.**
  `OP_DICT_NEW` calls `hashtable_key_dup` for every key on every evaluation, even though a literal
  like `{"id": i, ...}`'s keys are the same constant-pool bytes every time. Avoiding the duplicate
  safely needs `HashTableEntry` to know, per entry, whether it owns its key or is borrowing a
  constant -- `hashtable_free`/`rehash_sparse`/the duplicate-key overwrite path in
  `hashtable_put_hashed` all free a key unconditionally today. That requires either a new field on
  every entry (`HashTableEntry` is 40 bytes; even a 1-byte ownership flag rounds it up to 48 -- an
  8-byte tax on every entry in every dict in the language, literal-keyed or not) or stealing a bit
  from `length` (no memory cost, but `hash_match` -- the hottest function in every dict lookup,
  called on every probe -- would need to mask it out on every single comparison, everywhere).
  Same economics that already killed `OP_COMPOUND_BINARY` and `OP_MOD_POW2_INT` earlier this
  session: a cost paid by every dict operation in the language, for a win that only helps repeated
  dict-*literal* construction specifically, not the more common "build once, read many times"
  pattern. The more honest version of this idea doesn't shrink the tax, it avoids it by going
  further: recognize a dict literal whose keys are always literal strings and give it a
  struct-shaped fast path via the existing `Shape` machinery instead of a hashtable at all -- a
  materially bigger feature (has to preserve dicts' dynamic-key-insertion semantics), not a small
  follow-on. Declined for now; revisit only if that bigger shape-specialization idea gets picked up.

---

## Appendix: every allocation site, and exactly when it fires

Two entirely separate allocation lifetimes exist in this codebase. Don't conflate them:

- **Compile-time / permanent** — grows a `Chunk`'s own bookkeeping arrays. Lives for the process's
  life (or the importing module's life), never GC-tracked, freed only by `chunk_free` (or, for
  `shapes`/`functions`, not even then — see below).
- **Runtime / GC-tracked** — a pool cell (one of the 8 pools, §2.1) plus, for variable-length types,
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

**Prefer a check over a comment.** If a comment exists to warn ("don't reorder these fields", "this
must stay byte 0"), a `_Static_assert` says the same thing and cannot be skimmed past — it fails the
build with the reason attached. `AerString` and every pooled type use this for their layout
constraints. Reach for it before writing the paragraph.

**Enforced, not just stated.** `make check-comments` (run in CI) fails on any block over 6 lines.
The blocks that predate the check are grandfathered in `tools/comment_baseline.txt`; that file is a
ratchet, so removing an entry is welcome and adding one needs a real reason. This rule lived here as
prose for a long time and drifted back to 20% comment density anyway — the check is the same rule
with teeth.

### Formatting

`.clang-format` at the repo root is authoritative; `make format` applies it, `make check-format`
verifies it, and CI runs the latter. The settings that were a real judgment call:

- **`AllowShortIfStatementsOnASingleLine: WithoutElse`** — `if (!p) return;` stays on one line,
  which was already the most common form. Anything with an `else` gets braces, where the one-line
  version genuinely misleads.
- **`ColumnLimit: 110`** — wide enough for this codebase's operand-heavy VM code without the
  638-character lines that existed before.
- **`AlignConsecutive*: false`** — column-aligned declarations look tidy, but one rename re-diffs
  the whole block and buries real changes in whitespace.
- **`SortIncludes: false`** — include order is hand-maintained and parts of it are load-bearing.

Reformat commits must be separate from behavior changes, so a large whitespace diff never hides a
real one.

### Dead code

If a function, opcode, branch, or file is confirmed unused, delete it outright — don't comment it
out, don't leave a `// no longer used` marker, don't keep a re-export for compatibility. Verify
with a real search (callers, computed-goto dispatch tables, macro-generated references) before
deleting, since this codebase dispatches through function-pointer tables and packed opcodes that a
plain text search can miss.
