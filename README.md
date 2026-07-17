# AER

AER is a small, dynamically-typed scripting language written in C. The name comes from the ancient
Greek and Latin word for *air* — the language is designed around three values: **light** (minimal
codebase, no bloat), **fast** (bytecode VM, not tree-walking), and **easy to understand** (both the
language syntax and the implementation).

It is a learning and research project, not a production language. The goal is to see how far a clean,
principled design can go with very little code.

---

## Contents

- [What Makes AER Interesting](#what-makes-aer-interesting)
- [Practical Applications](#practical-applications)
- [Getting Started](#getting-started)
- **Guide**
  - [Keywords](#keywords)
  - [Operators](#operators)
  - [Built-in Functions](#built-in-functions)
  - [Syntax](#syntax)
  - [Values](#values)
  - [Variables](#variables)
  - [Control Flow](#control-flow)
  - [Functions](#functions)
  - [Lists](#lists)
  - [Maps](#maps)
  - [Structs](#structs)
  - [Method Calls and Pipes](#method-calls-and-pipes)
  - [Error Handling](#error-handling)
  - [Modularity](#modularity)
  - [Concurrency](#concurrency)
- [Standard Library](#standard-library)
- [Embedding](#embedding)
- [Pitfalls and Limitations](#pitfalls-and-limitations)
- [Benchmarking](#benchmarking)
- [Architecture](#architecture)
- [Design Decisions](#design-decisions)
- [Memory and Security](#memory-and-security)
- [File Map](#file-map)

---

## What Makes AER Interesting

**Single-pass elegance.** Most textbook compilers make three separate passes: lex → parse → codegen.
AER collapses all three. Every `parse_*` function is simultaneously the grammar rule and the code
generator.

**Precedence climbing.** One function and a table (see [Operators](#operators)) replace the
traditional cascade of `parse_addition`, `parse_multiplication`, `parse_unary`, etc. Adding an
operator is one line — plus, for the handful whose right-hand side isn't a general expression
(`in` is; `as` and `|>` aren't), one small special case.

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
just falls out naturally from the design — no special handling required.

**`OP_CALL_VALUE`.** Functions stored in arrays and dicts can be called directly:
`ops[0](3, 4)` or `dispatch["add"](10, 20)`. The opcode removes the function value from the stack
in-place before jumping, which keeps the call convention identical to a named call.

**Structs without a new value type.** A struct instance is an array with a shape pointer — printing,
reference semantics, and heap layout are all inherited for free. The interesting engineering is
entirely in the guards that keep structs from silently behaving like arrays where that would be
surprising.

---

## Practical Applications

AER is genuinely suited to:

| Use case | Why AER fits |
|----------|-------------|
| Embedded scripting DSL | Self-contained C VM, no external deps, embeds in any C project |
| Game logic / NPC behaviour | First-class functions, structs, dicts, and arrays cover most logic patterns; lightweight enough for per-frame calls |
| Config with computation | More expressive than JSON/TOML; simpler to embed than Lua |
| Teaching language design | Full compiler + VM in a few thousand lines of readable C; fits on one screen at a time |
| Automation scripts | Real data structures without Python startup overhead |

---

## Getting Started

### Dependencies

No external libraries — the standard C library is the only dependency, on every platform. The VM's
computed-goto dispatch loop (`&&label` / `goto *ptr`) is a GCC/Clang extension MSVC doesn't support,
which is why Windows needs MinGW-w64 rather than `cl.exe`.

- **Linux / WSL** — GCC + Make, nothing else.
- **macOS** — Xcode Command Line Tools (`xcode-select --install`). `gcc` resolves to Clang, which
  supports the same computed-goto extension; the Makefile is unmodified for this platform.
- **Windows** — [MSYS2](https://www.msys2.org/), then from an MSYS2 shell:
  `pacman -S mingw-w64-x86_64-gcc make`. Build and run from that shell (or the MINGW64 shell it
  installs) — plain `cmd.exe`/PowerShell aren't a supported invocation path (the Makefile's
  `mkdir -p`/`rm -rf` need a real POSIX-ish shell, which MSYS2 provides).

### Building

```sh
make
```

Output is written to `binary/aer` (`binary/aer.exe` on Windows — the Makefile handles the suffix).
This is the dev build (debug symbols, no profile data) — use it for anything that might need gdb
or a readable ASAN/fuzzer backtrace.

For a release build, use profile-guided optimization instead:

```sh
make pgo
```

This is a two-pass build: pass 1 instruments a build and runs it against a representative workload
plus the full test suite to record real execution-frequency data; pass 2 recompiles from that
profile so the compiler lays out hot/cold code using actual behavior instead of static heuristics.
Output is `binary/aer-pgo` (measured ~6-9% fewer instructions/cycles than the plain build on this
project's own reference benchmark — re-run the comparison on your own workload before relying on
that number, since PGO's benefit is workload-shaped by construction). Rebuild with `make pgo`
whenever the source changes — the profile data is regenerated from the current binary each time,
not cached.

To clean:

```sh
make clean
```

### Running

```sh
./binary/aer              # interactive REPL
./binary/aer script.aer   # run a source file
./binary/aer version      # print version
./binary/aer help         # print usage
```

(`./binary/aer.exe` on Windows — from the same MSYS2 shell used to build it.)

In the REPL, blocks are entered by ending a line with `:` and terminated with a blank line:

```
>>> x = 10
>>> if x > 5:
...     print(x)
...
10
```

**Ctrl-C cancels the current line only** (like Python's REPL) — it does not exit. **Ctrl-D exits.**
There's no `exit()`/`quit()` builtin, so Ctrl-D is the only way out short of closing the terminal;
pressing Ctrl-C prints a reminder of this every time.

On Linux/macOS, scripts support shebang lines for direct execution (Windows has no shebang/`chmod`
convention — run `aer.exe script.aer` directly instead):

```sh
#!/usr/bin/env aer
print("Hello, world")
```

```sh
chmod +x script.aer
./script.aer
```

---

# Guide

Everything below is the language itself: reserved words, operators, and built-ins first (for quick
lookup), then the grammar and semantics organized by topic.

## Keywords

AER has **13 reserved words**, plus the two boolean literals. That's the entire list — nothing else
in the language is reserved:

| Keyword | Role |
|---------|------|
| `if` / `else` | conditional statement and inline expression |
| `for` | the single iteration keyword — while, for-each, and ranges all use it |
| `in` | membership test, and the `for x in ...` iteration form |
| `as` | cast to a primitive type, or shape-check against a struct type |
| `struct` | declare a fixed-shape record type |
| `function` | declare a named function, or start an anonymous function value |
| `return` | return from a function, optionally with a value (or several) |
| `break` / `continue` | loop control |
| `null` | the absence-of-a-value literal |
| `import` | bring a native or file-based module into scope |
| `true` / `false` | boolean literals |

**Everything else is an ordinary identifier**, including `print`, `length`, `append`, `delete`,
`type`, `assert`, and `panic` (see [Built-in Functions](#built-in-functions)) and every stdlib name
(`math`, `random`, `string`, `time`). None of these are keywords — they can be shadowed by a local
variable or parameter of the same name, and they compose with everything else a function value can
(passed around, stored in a variable, piped through `|>`).

## Operators

Binary operators are looked up by one precedence table, from lowest to highest:

| Precedence | Operators | Meaning |
|:-:|-----------|---------|
| 1 (lowest) | `\|\|`  `\|>` | logical or (short-circuit) · pipe |
| 2 | `&&` | logical and (short-circuit) |
| 3 | `\|` | bitwise or |
| 4 | `^` | bitwise xor |
| 5 | `&` | bitwise and |
| 6 | `==`  `!=` | equality |
| 7 | `<`  `>`  `<=`  `>=`  `in` | comparison · membership |
| 8 | `<<`  `>>` | bit shift |
| 9 | `+`  `-` | add · subtract |
| 10 | `*`  `/`  `%`  `//` | multiply · true-divide · modulo · floor-divide |
| 11 (highest) | `as` | cast / shape-check |

`as` binding tighter than everything else means `x as integer + 1` reads as `(x as integer) + 1`,
matching Rust's `as` precedence convention. `in` sits at comparison precedence, not its own tier —
`a == b in list` parses as `a == (b in list)`.

**Unary** (bind tighter than any binary operator): `!x` (logical not), `-x` (negate), `~x` (bitwise
not).

**Assignment** is a statement, not an expression, and isn't part of the precedence table at all:
`=`, and the compound forms `+=  -=  *=  /=  %=  //=  <<=  >>=  &=  |=  ^=`. Compound assignment
works on plain names, indexed targets, and dot-field targets alike — `x += 1`, `arr[i] += 1`, and
`p.x += 1` are all supported, at any chain depth (`bodies[i].pos[0].x += v`).

`/` always performs true division and returns a real; `//` is floor division, rounding toward
negative infinity (matching Python, not C):

```
10 / 3          # 3.333...
10 // 3         # 3
-7 // 2         # -4   — floors toward negative infinity, not toward zero
```

`&&` and `||` **always produce a boolean**, never one of the operand values — `x = x || "default"`
does not work the way it does in JavaScript; write `x = if x: x else: "default"` instead. Both
short-circuit: the right-hand side is only evaluated if the left side doesn't already determine the
result.

## Built-in Functions

Seven **core builtins** are always available, with no `import` — ordinary identifiers dispatched
directly by the VM, not syntax:

| Function | Signature | Behaviour |
|----------|-----------|-----------|
| `print(x)` | 1 arg | writes `x`'s string form to stdout, followed by a newline |
| `type(x)` | 1 arg | returns `x`'s type name as a string (a struct instance returns its declared name) |
| `length(x)` | 1 arg | element count of an array, entry count of a dict, or a string's character count |
| `append(arr, x)` | 2 args | mutates `arr` in place, adding `x` at the end; returns `arr` (redundant to capture) |
| `delete(x, key)` | 2 args | removes index `key` from an array or key `key` from a dict, mutating in place; returns `x` |
| `assert(cond, msg)` | 2 args | prints `ASSERT FAILED: msg` on a false `cond` and keeps running — see [Error Handling](#error-handling) |
| `panic(msg)` | 1 arg | aborts like any runtime error, with your own message — see [Error Handling](#error-handling) |

Everything past this — `math`, `random`, `string`, `time`, `io` — requires an explicit `import` and
is covered in [Standard Library](#standard-library).

## Syntax

Indentation is significant, Python-style — a block starts with a line ending in `:` and is indented
one level deeper than its header; there are no braces or `end` keywords.

```
if x > 0:
    print(x)
```

`if`, `else`, `for`, and `function` bodies are **always** the indented block form — there is no
same-line single-statement shortcut. This is a deliberate one-way-to-do-it choice: a same-line form
would be a second, purely visual spelling of the identical thing. (The inline *if-expression*, see
[Control Flow](#control-flow), looks similar but returns a value — a genuinely different capability,
not a layout variant.)

Comments run from `#` to end of line:

```
# full-line comment
x = 5  # inline comment
```

A statement is one of: an assignment, a bare function/pipe call (result discarded), or one of the
keyword-led forms (`if`, `for`, `return`, `break`, `continue`, `import`, `struct`,
`function`). There is no statement terminator — a newline ends a statement.

## Values

AER is dynamically typed. There are eight underlying value types:

| Type | Examples | Notes |
|------|---------|-------|
| Null | `null` | Absence of a value; falsy |
| Boolean | `true`, `false` | |
| Integer | `0`, `42`, `-7` | `long long`; true division always returns Real |
| Real | `3.14`, `-0.5` | `double` |
| String | `"hello"` | Immutable; supports indexing, slicing, iteration, interpolation and escapes |
| Function | `function foo(): ...` | First-class; stores code offset and arity |
| Array | `[1, 2, 3]` | Mutable; reference semantics — see [Lists](#lists) |
| Dict | `{"a": 1}` | Mutable string-keyed; reference semantics — see [Maps](#maps) |

Struct instances (see [Structs](#structs)) are a fixed-shape variant of Array — same reference
semantics, but dot-accessed only and reported by their declared name (`type(p)` returns `"Point"`,
not `"array"`).

### Null

`null` represents the absence of a value. It is falsy. `null == null` is `true`;
`null` compared to any other type is `false`.

```
x = null
print(x == null)      # true
if x:
    print("has value")
else:
    print("null")     # prints this
```

Missing dict keys return `null` rather than erroring — see [Maps](#maps).

### Numbers

Mixing an integer and a real promotes the integer:

```
1 + 2.5         # 3.5
```

### Strings

String literals use double quotes. Supported escape sequences:

| Sequence | Result |
|----------|--------|
| `\n` | newline |
| `\t` | tab |
| `\\` | backslash |
| `\"` | double quote |
| `\{` | literal `{` (suppresses interpolation) |

```
print("line1\nline2")
print("C:\\Users\\robert")
print("use \{x} for a literal brace")
```

Triple-quoted strings (`"""..."""`) span raw newlines and need no escaping for a bare `"` inside —
they end only on a literal `"""` or end of input:

```
block = """line one
line two with a "quote" in it
line three"""
```

Strings are indexable, sliceable, and iterable. There is no separate character type — indexing a
string returns a length-1 string:

```
word = "hello"
print(word[0])         # h
print(word[-1])        # o
print(word[1:3])       # el
print(word[2:])        # llo

for ch in word:
    print(ch)            # h e l l o
```

Strings are immutable — `word[0] = "H"` is a runtime error.

**Interpolation**: any `{identifier}` inside a string is replaced with that variable's string form,
no prefix required:

```
name = "Robert"
age  = 30
print("Hello {name}, you are {age} years old")
# Hello Robert, you are 30 years old
```

Interpolation only accepts a bare identifier, not a general expression — `"{x + 1}"` is a syntax
error; assign to a temporary first. To include a literal `{`, escape it: `"\{name}"` prints `{name}`.

### Casting and Shape-Checking — `as`

`x as T` has two meanings depending on `T`:

- If `T` is `integer`, `float`, `string`, or `boolean`, it **coerces** `x` to that primitive type.
- Otherwise `T` is treated as a struct type name and it becomes a **runtime shape check**: `x`
  passes through unchanged if it's exactly a struct instance of type `T`, otherwise it errors. It
  never converts one struct shape into another — there's no single well-defined way to do that.

```
print("42" as integer)     # 42
print(42 as float)         # 42.0
print(42 as string)        # "42"
print(3.7 as integer)      # 3   (truncates toward zero, does not round)
print(0 as boolean)        # false — numeric/string coercion to boolean uses the same truthiness rule as `if`

p = Point(1.0, 2.0)
checked = p as Point       # passes through unchanged
# e as Point                # errors if e isn't exactly a Point
```

`as` is also used to declare a struct-typed function parameter — see [Structs](#structs).

## Variables

Assignment creates a variable. There is no declaration keyword:

```
x = 10
name = "AER"
```

Compound assignment operators are covered in [Operators](#operators).

### Scope

Variables defined inside a block are local to that block. Variables defined outside are readable
and writable from inner blocks — there is no implicit shadowing.

```
x = 10
for x > 0:
    x -= 3          # modifies outer x
    inner = x * 2   # only exists inside this block

print(x)             # ok
print(inner)         # Error: 'inner' is not defined
```

### Destructuring

Multiple variables can be assigned from a comma-separated expression list in one statement:

```
a, b = 10, 20
a, b = b, a     # swap — RHS is fully evaluated before any assignment
```

## Control Flow

### Conditionals

```
if x > 0:
    print(x)
else:
    print(0)
```

The **inline if-expression** returns a value, unlike the statement form above:

```
abs_x = if x >= 0: x else: -x
print(if n % 2 == 0: "even" else: "odd")
label = if admin: "Admin"    # no else branch evaluates to null
```

### For Loop

`for` is the single iteration keyword. It covers while loops, for-each, and ranges.

**While form:**

```
i = 0
for i < 10:
    i += 1
```

**Range — exclusive upper bound, auto-reverse:**

```
for i in 0..10:    # 0, 1, 2 ... 9
    print(i)

for i in 10..0:    # 10, 9, 8 ... 1
    print(i)
```

**Array iteration:**

```
fruits = ["apple", "banana", "cherry"]
for fruit in fruits:
    print(fruit)
```

**String iteration** (yields one character at a time):

```
for ch in "abc":
    print(ch)
```

**Dict iteration — keys only, or key-value pairs:**

```
scores = {"alice": 95, "bob": 87}
for name in scores:
    print(name)

for name, score in scores:
    print("{name}: {score}")
```

`break` exits the loop immediately. `continue` skips to the next iteration. Both work correctly
across nested loops and nested `if` blocks:

```
for i in 0..10:
    if i % 2 == 0:
        continue
    if i > 7:
        break
    print(i)                    # 1 3 5 7
```

## Functions

Functions are defined with `function` and return a value with `return`. A bare `return` or
falling off the end of a function returns `null`. Parameters are dynamically typed. Recursive
and mutually recursive functions work without forward declarations.

A function body is always the indented block form, same as `if`/`else`:

```
function double(n):
    return n * 2

function add(a, b):
    return a + b

function factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

print(add(3, 4))       # 7
print(factorial(5))    # 120
```

Functions are first-class values — they can be stored in variables and passed as arguments:

```
function apply(f, x):
    return f(x)

print(apply(double, 5))    # 10
```

### Default Parameters

A trailing parameter can specify a default with `= <literal>` — the same compile-time-constant
restriction as [struct field defaults](#structs), not an arbitrary expression. Once one parameter
has a default, every parameter after it must too:

```
function greet(name, greeting = "Hello"):
    return "{greeting}, {name}!"

print(greet("World"))          # Hello, World!
print(greet("World", "Hi"))    # Hi, World!
```

Omitted trailing arguments take their default at the call site — the callee's own body never sees
the difference between an explicit argument and a supplied default. This works identically through
a named call, a function-value call (`f = greet; f("World")`), and a cross-module call.

### Tail Calls

`factorial` above is *not* tail-recursive — `n * factorial(n - 1)` still has work (the
multiplication) left to do after the recursive call returns, so each call needs its own stack
frame, capped at 64 deep (`VM_CALL_MAX`). A call in true tail position — `return name(args)` as the
**entire** statement, nothing wrapping it — reuses the current frame instead of pushing a new one,
so recursion in that shape costs no additional stack depth no matter how deep it goes:

```
function count_down(n, acc):
    if n <= 0:
        return acc
    return count_down(n - 1, acc + n)   # tail position — frame reused, not grown

print(count_down(100000, 0))    # 5000050000, no "Call stack overflow"
```

Mutual recursion between two functions gets the same treatment.

### Scope inside a function

A top-level variable is **entirely off-limits inside a function** — not readable, not
assignable, and its name can't be reused for a local or a parameter either. One name means one
variable, everywhere; a function's only inputs are its parameters, and its only output is its
return value:

```
outer = 10

# function read_it():
#     return outer      # error: 'outer' is a top-level variable — not accessible inside a
#                        # function; pass it as a parameter (or rename)
# function shadow_it(outer):    # error, for the same reason — the PARAMETER name collides too
#     return outer

function read_it(v):
    return v             # v is a parameter, not the top-level name — this is fine

print(read_it(outer))     # 10
```

This is stricter than "assignment is always local" — it's "the name doesn't exist in here at
all." To share state across calls, mutate something you were explicitly given a reference to (a
struct, array, or dict — all reference types), instead of relying on a function reaching outward
by bare name:

```
struct Counter:
    value: integer = 0

function bump(c):
    c.value += 1     # mutates the struct's own field, passed in explicitly

shared = Counter(0)
bump(shared)
bump(shared)
print(shared.value)    # 2
```

A function's data access is always either its own parameters/locals, or a reference explicitly
passed to it — never an implicit reach into an enclosing scope. An anonymous function nested
inside another function's body follows the same rule.

### Anonymous Function Expressions

`function(params): body` is a function **value**, not a declaration — it can be assigned, returned,
or passed as an argument, and follows the same block-body-only rule as a named function:

```
square = function(n):
    return n * n
print(square(5))    # 25
```

Named function statements (`function foo(): ...`) still cannot be nested at all — only function
*expressions* can appear inside a function body.

### Multiple Return Values

A function can return multiple values separated by commas. They are received as
destructuring targets (see [Variables](#variables)):

```
function min_max(arr):
    lo = arr[0]
    hi = arr[0]
    for v in arr:
        if v < lo:
            lo = v
        if v > hi:
            hi = v
    return lo, hi

lo, hi = min_max([3, 1, 4, 1, 5, 9])
print(lo)    # 1
print(hi)    # 9
```

Under the hood, `return lo, hi` allocates a real array to carry the values across to the caller's destructuring assignment — there's no special multi-value calling convention. A single-value `return lo` allocates nothing extra. For a function called in a hot loop, prefer a single return value (or an explicitly passed-in struct/array to write into) over multiple return values if the allocation matters.

## Lists

Arrays are mutable ordered sequences with reference semantics — assigning an array to a
variable copies the reference, not the data.

```
arr = [1, 2, 3]
print(arr[0])        # 1
print(arr[-1])       # 3  (negative indexing)

arr[1] = 99
print(arr)           # [1, 99, 3]

append(arr, 4)        # append (mutates in place; the returned array is redundant to capture)
delete(arr, 1)        # remove index 1, shifting subsequent elements down (preserves order)
print(length(arr))    # length
```

`delete()` on an array removes by index (negative indices allowed, out-of-range errors) rather than
by value, and returns the array. There is no separate "remove the last element" function;
`delete(arr, -1)` covers it.

Slicing returns a new array — `[a:b]`, `[a:]`, `[:b]`, `[:]`. Out-of-range bounds are clamped
rather than erroring, matching Python's slicing behavior:

```
nums = [0, 1, 2, 3, 4, 5]
print(nums[1:4])     # [1, 2, 3]
print(nums[3:])      # [3, 4, 5]
print(nums[:3])      # [0, 1, 2]
```

Index access chains — nested arrays and dicts support `[...][...]` syntax:

```
matrix = [[1, 2], [3, 4]]
print(matrix[0][1])           # 2

matrix[0][1] = 99            # nested assignment
print(matrix[0])              # [1, 99]
```

## Maps

Dicts are mutable string-keyed hash tables with reference semantics. Missing keys return `null`
rather than erroring.

```
d = {"x": 1, "y": 2}
print(d["x"])           # 1
print(d["missing"])     # null

d["z"] = 3
"z" in d                # true
delete(d, "y")          # remove key
length(d)                # number of entries
```

Dictionary keys are always strings — no mixed-type key lookups, no hash collision between
integer `1` and string `"1"`.

Iterating a dict (`for k in d:`, `print(d)`, `json.encode(d)`) visits entries in hash-bucket order,
not insertion order — don't rely on a dict preserving the order its keys were added in.

## Structs

Structs are AER's fixed-shape record type — the closest thing to a "class," minus methods (see
[Method Calls and Pipes](#method-calls-and-pipes) for how behaviour attaches to them instead).
Declared with a field list, each field mandatorily typed, and an optional literal default:

```
struct Point:
    x: float = 0.0
    y: float = 0.0
```

**Every field must declare both a type and an explicit default** — `x = 0.0` with no type, and
`x: float` with no default, are both compile errors. Eligible types are `integer`, `float`,
`boolean`, `string`, `any`, or another struct's name; `any` opts out of type checking entirely for
that field (and, along with `string` and a nested struct type, opts the whole struct out of
packed-array eligibility — see [Packed Arrays](#packed-arrays) below). Defaults must be literals —
no arbitrary expressions — since the whole declaration compiles to a single instruction that
registers the shape. Assigning a value of the wrong type to a typed field is a runtime error, the
same protection function parameters don't get.

**Instantiation** reuses ordinary call syntax, positionally in declared field order. Omitted
trailing arguments take their declared defaults; passing more arguments than fields is an error:

```
p1 = Point(1.0, 2.0)
p2 = Point(5.0)        # y defaults to 0.0
p3 = Point()           # x and y both default to 0.0
```

**Field access is dot notation**, read and write:

```
print(p1.x)             # 1
p1.x = 10.0
```

Accessing or assigning an unrecognized field name is an error — this is where typo protection
comes from; there's no separate validation step, it falls directly out of the field lookup.

Structs report their own declared name from `type()` and when printed:

```
print(type(p1))         # Point
print(p1)                # Point{x: 10, y: 2}
```

**Isolated data boundaries:** structs are dot-only, collections are bracket-only, and the two
don't mix. Bracket indexing, slicing, `append()`, and `delete()` are all rejected on a struct
instance:

```
# p1[0]           # error: use '.' not '[]'
# append(p1, 1)    # error: structs have a fixed shape
```

AER has no methods — all functions live in one flat global scope. A function that operates on a
struct just takes it as a plain parameter:

```
function point_translate(p, dx, dy):
    return Point(p.x + dx, p.y + dy)

point_translate(p1, 1.0, 1.0)
```

`x as Point` is a standalone runtime shape check, not part of a function signature — see
[Casting and Shape-Checking](#casting-and-shape-checking--as).

### Packed Arrays

`Type[count]` constructs a fixed-size, mass-allocated array of `count` instances of struct `Type`,
packed inline in one contiguous block instead of `count` separately heap-allocated instances
linked through an ordinary array of references. This is AER's answer to data-oriented design: a
tight loop over a large `Type[]` touches far fewer cache lines than the same loop over an ordinary
array of struct instances (see [Benchmarking](#benchmarking) for a measured, at-scale comparison).

```
struct Body:
    x: float = 0.0
    y: float = 0.0
    mass: float = 0.0

bodies = Body[1024]
bodies[0].x = 1.5
print(bodies[0].x)      # 1.5
print(length(bodies))   # 1024
```

**Eligibility is per-struct-type**, checked once at parse time: every field must be `integer`,
`float`, or `boolean` — `any`, `string`, and nested-struct fields all disqualify a struct from
`Type[count]` construction (they can't be packed at a uniform byte width), even though that same
struct works fine as an ordinary, individually-constructed instance (`Type()`).

**Field access only — no standalone per-element reference.** `arr[i].field` (get and set,
including compound assignment) is the only supported form; a bare `arr[i]` alone is a compile
error (`Cannot index type`), and so is `for x in arr:` — a packed array can't be iterated directly
(index with an ordinary counting loop instead: `for i in 0..length(arr): ... arr[i].field ...`).
This isn't an arbitrary restriction: a packed element has no standalone value to hand back — its
"address" is pure arithmetic (`base + i * stride`), recomputed at each `.field` access, not a
pointer to save or a value to box.

**Not yet supported for a packed array specifically** (an ordinary struct array works fine with
all of these): `json.encode()` (errors — "cannot serialize a packed array value"), and the
embedding API's `Value`/`aer_val_to_public` boundary (a packed array can't yet cross into or out of
host code through `AerNativeFn`).

`type()` reports a distinct name (`"Point[]"`, not `"Point"`) so packed and ordinary instances of
the same struct are still distinguishable at runtime.

## Method Calls and Pipes

AER has no dot-method-call syntax (`x.foo()`) on ordinary values — a struct's dot syntax is field
access only (see [Structs](#structs)). Two things stand in for "method calls":

**Module-qualified calls** — `math.sqrt(x)`, `string.upper(s)` — read like a method call but are
resolved as an ordinary function lookup against an imported module's table (see
[Modularity](#modularity) and [Standard Library](#standard-library)):

```
import string
print(string.upper("hello"))   # HELLO
```

**The pipe operator, `|>`** — `x |> f(args)` desugars to `f(x, args)`: `x` becomes argument zero.
It's a plain rewrite, not type-based dispatch — `f` is always resolved through AER's ordinary flat
function scope, exactly as if you'd written `f(x, args)` yourself. This is what gives AER a
method-chain *feel* without a real method table:

```
function increment(n, amount):
    return n + amount

function square(n):
    return n * n

print(5 |> increment(3))                  # 8
result = 3 |> increment(1) |> square()    # (3 + 1)^2 = 16
```

The target can be a module-qualified function too, exactly like calling it directly:

```
print("hello" |> string.upper())   # HELLO
```

A pipe chain also works as a bare statement, its result discarded — useful when the target mutates
in place and the return value is redundant to capture:

```
b.neighbors |> append(a)   # same as append(b.neighbors, a) — b.neighbors mutates in place
```

**Enforced:** a pipe target's arguments may not contain a function call at any nesting depth,
including a nested pipe's own call. `5 |> increment(square(2))` and `5 |> increment(2 |> square())`
are both parse errors — this keeps a pipe chain flat and readable instead of hiding calls inside
calls. If you need a nested computation, assign it to a variable first.

**Functions stored in a collection can also be called directly** — `ops[0](3, 4)` or
`dispatch["add"](10, 20)` — the closest AER gets to dynamic dispatch, useful for building a
dispatch table by hand instead of a method table:

```
dispatch = {"add": function(a, b): return a + b}
print(dispatch["add"](3, 4))   # 7
```

## Error Handling

Two unrelated things are both called "errors" in AER and it's worth keeping them apart.

**The `(value, err)` convention** is a coding pattern, not a language feature. A fallible function
returns `value, null` on success and `null, "message"` on failure; the caller checks with a plain
`if`. AER has no exceptions.

```
function safe_div(a, b):
    if b == 0:
        return null, "division by zero"
    return a / b, null

result, err = safe_div(10, 0)
if err != null:
    print("Error: {err}")
```

**Runtime errors** are a VM-level thing and are not values at all — out-of-bounds access, wrong
argument count, dividing by zero with `/`, a shape mismatch from `as Type`, and similar faults print
a message (prefixed with the source line it happened on, e.g. `Line 12: ...`) and then:

- In the REPL, **abort the rest of the current statement only** — the REPL keeps going and the next
  line is unaffected.
- When running a file, exit the process immediately.

There is no way to catch a runtime error from AER code — it is not a value you can inspect.

**`panic(message)`** is a user-invokable entry point into that exact same abort path — for a
script's own detected invariant violations, not VM-level faults:

```
function handle(mode):
    if mode == "a": return 1
    if mode == "b": return 2
    panic("unreachable: unknown mode " + mode)
```

`message` must be a string. `panic()` behaves identically to any other runtime error in every way —
same abort behavior, same REPL-vs-file distinction, same unreachability from AER code.

**`assert(condition, message)`** is a third, separate thing — a check for tests and invariants, not
an error. A failed assertion prints `ASSERT FAILED: message` and the script **keeps running** — it
does not abort the current statement the way a runtime error does:

```
assert(1 + 1 == 2, "arithmetic works")
assert(1 + 1 == 3, "this one fails")   # prints "ASSERT FAILED: this one fails", keeps going
print("still here")                    # runs
```

Running a file with one or more failed assertions still exits the process with a nonzero code,
checked independently of runtime errors. An embedding host reads the count via
`aer_assert_failure_count()` (see [Embedding](#embedding)).

## Modularity

AER draws a hard line between **core builtins** ([Built-in Functions](#built-in-functions) —
always available, no `import`) and **stdlib/host modules**, which require an explicit `import`.

```
import math
import random

print(math.sqrt(16.0))     # 4
print(random.randint(1, 6))
```

There are five native modules — `math`, `random`, `string`, `time`, `json` — not files on disk, but a
hardcoded set the parser recognizes (see [Standard Library](#standard-library) for the full
function list). `import math` itself emits no bytecode; it just records "math" as a known module
name for the rest of the file (or REPL session), consulted entirely at parse time. There's no
`TYPE_MODULE` value, and a module name is never a real scope variable, so `x = math` (assigning the
module itself, rather than calling something on it) is a parse error.

### File-based imports

`import` also accepts a name that isn't a native module, resolving it to `<name>.aer` in the same
directory as the importing file:

```
import helpers      # resolves ./helpers.aer
print(helpers.double(21))    # 42
```

If no same-directory file matches, resolution falls back to `AER_PATH` — an environment variable
holding a list of additional directories, `;`-separated on Windows and `:`-separated elsewhere,
searched in listed order, first match wins:

```sh
AER_PATH=/opt/aer/lib:/home/me/aer-modules aer script.aer
```

A dotted path is a directory hint, not part of the bound name — `import sub.mid` resolves
`sub/mid.aer` (relative to the importing file's own directory, same as any other import), but the
module still binds under its own bare filename, `mid`:

```
import sub.mid
print(mid.some_function())    # not sub.mid.some_function() — the dots only picked the file
```

There's no cap on how many distinct files a program imports — the module registry, the "currently
loading" cycle-detection stack, and the lexer's own open-file table all grow as needed.

This is a narrower slice of a real module system than `import` in most languages, tied directly to
how AER's single-pass compiler works:

- **`import` is parse-time, not runtime** — the imported file is fully read, parsed, and *executed*
  synchronously, before the importing file's own parse even continues. This is why `import` is only
  allowed at the top level of a file, not inside a function, loop, or `if` block.
- **Isolation is a genuinely separate VM**, not just a pushed scope. An imported file's top-level
  code runs in its own independent `Chunk` and `VM`, so its globals can never collide with the
  importing program's own same-named globals.
- **Member access is call-only**, matching the native modules — `helpers.double(x)` works,
  `helpers.SOME_CONSTANT` does not.
- **Importing the same file twice is a no-op**, cached by import name. **Circular imports are
  rejected** with a parse-time error.
- Module names are cached process-wide by the name given to `import`, not by resolved path — two
  different files imported under the same name from different directories would collide. Not a
  concern for typical single-directory projects, but `AER_PATH` makes it worth knowing for larger
  ones.

### Namespacing

Each file-module is a fully separate VM and namespace — nothing crosses that boundary implicitly:

- Cross-file access is always explicitly qualified (`module.function()`), never ambient. There's no
  way to reach into an imported file's globals except by calling one of its functions.
- Struct types are scoped to the file that declares them — a shape name is only meaningful within
  its own file's pool.
- Two files can each freely define a same-named function or struct with zero collision, precisely
  because nothing crosses the boundary implicitly. `helpers.aer` and `main.aer` can both define
  `double(n)` without either one shadowing or conflicting with the other.

### Registering host functions

An embedding host can add its own modules the same way — see [Embedding](#embedding) for
`aer_register_function`.

## Concurrency

**Not supported.** AER has no threads, coroutines, `async`/`await`, or event loop of any kind — the
VM is a single, synchronous dispatch loop, and a script runs start-to-finish on the thread that
calls into it. This is a deliberate scope limit for a "light" embedded scripting language, not an
oversight: an embedding host that needs concurrent script execution runs multiple independent
`Chunk`/`VM` pairs (see [Embedding](#embedding)) on its own threads — AER itself never has to know
about them, the same way file-based `import` already gets isolation from a genuinely separate VM
rather than any in-language concurrency primitive.

---

## Standard Library

There are currently five native modules — `math`, `random`, `string`, `time`, and `json` — plus one
opt-in host capability, `io`. See [Modularity](#modularity) for how `import` resolves these.

```
math.sqrt(x)          # square root, always returns a real
math.pow(x, y)        # x to the power of y, always returns a real
math.floor(x)         # round toward negative infinity, returns an integer
math.ceil(x)          # round toward positive infinity, returns an integer
math.abs(x)           # absolute value, preserves integer/real
math.min(a, b)        # the smaller of two numbers, preserves whichever argument's type
math.max(a, b)        # the larger of two numbers, preserves whichever argument's type
math.sort(arr)        # sorts in place (ascending) and returns the array
math.sin(x), math.cos(x)     # standard trig, x in radians
math.log(x)           # natural log, x must be positive
math.log2(x), math.log10(x)  # base-2 / base-10 log, x must be positive
math.pi()             # the constant, as a function — every native module exposes functions only
random.random()       # a real in [0, 1)
random.randint(a, b)  # an integer in [a, b], inclusive of both ends
random.seed(n)        # reseeds the RNG — makes subsequent random()/randint() calls reproducible
string.upper(s)       # ASCII-only uppercase
string.lower(s)       # ASCII-only lowercase
string.trim(s)        # strips leading/trailing whitespace
string.contains(s, sub) # true if sub occurs anywhere in s
string.starts_with(s, prefix), string.ends_with(s, suffix)
string.repeat(s, n)   # s repeated n times (n must be >= 0)
string.replace(s, old, new)  # every occurrence of old (non-empty) replaced with new
string.split(s, sep)  # splits on a non-empty separator, returns an array of strings
string.join(arr, sep) # joins an array of strings with sep, returns a string
time.now()            # current epoch time as a real, with sub-second precision
time.strftime(t, fmt) # formats an epoch time (e.g. from time.now()) using C strftime format codes,
                      # in local time — time.strftime(time.now(), "%Y-%m-%d %H:%M:%S")
json.encode(value)    # returns a JSON string
json.decode(s)        # returns (value, err) — err non-null on malformed input
```

`math.sort(arr)` requires every element to be a number (compared numerically, integer and real mix
freely) or every element to be a string (compared lexicographically) — mixing the two is rejected
rather than falling back to some arbitrary tie-break.

`random.randint`'s implementation uses `rand() % span`, which is very slightly biased toward the
low end of the range for spans that don't evenly divide `RAND_MAX + 1`. A bias-free version needs
rejection sampling; not worth the complexity here.

`time.now()` is wall-clock (`clock_gettime(CLOCK_REALTIME, ...)`), not monotonic — a system clock
adjustment (NTP sync, manual change) could in principle make two successive calls disagree about
ordering. Good enough for logging and for measuring durations in ordinary scripts; not a substitute
for a monotonic clock in code that must be robust to clock adjustments mid-run.

### JSON — `json`

```
data = {"name": "AER", "version": 2, "tags": ["scripting", "small"]}
s = json.encode(data)
print(s)    # a JSON object with all three keys — order follows dict iteration order
            # (hash-bucket order), not insertion order (see Maps)

decoded, err = json.decode(s)
if err == null:
    print(decoded["name"])    # AER
```

`json.encode` maps null/boolean/integer/real/string/array/dict onto their obvious JSON
counterparts. A struct instance encodes as a JSON *object* keyed by its field names (not a bare
positional array), so the field names survive — but since JSON itself has no struct types, decoding
always produces a plain dict back, never the original struct. Encoding a function value is a
runtime error — there's nothing to serialize. `json.decode` follows the same `(value, err)`
convention as the rest of the fallible stdlib rather than aborting the script on malformed input.

### File I/O — `io`, an opt-in host capability, not a native module

Unlike the five native modules above, `io` is **not** hardcoded into the parser's native-module
table — it's registered like any other host function, via `aer_register_function` (see
[Embedding](#embedding)). `source/main.c` (the reference CLI) registers it, so `./binary/aer
script.aer` has file access; an embedding host that links AER into a game or a config parser simply
doesn't call `aer_io_register()`, and scripts running under it have no file access at all — no flag,
no sandbox layer, the capability just isn't there unless a host explicitly grants it.

```
f, err = io.open("log.txt", "a")   # mode is "r", "w", or "a"
if err != null:
    print("could not open: " + err)
else:
    write_err, err2 = io.write(f, "a line\n")
    io.close(f)
```

```
io.open(path, mode)   # returns (handle, err) — handle is a plain integer, opaque outside io.*
io.read(handle)       # reads all remaining data as a string, returns (contents, err)
io.write(handle, s)   # returns (null, err) — only err is ever meaningful
io.close(handle)      # returns (null, err)
io.stdin()            # returns the stdin handle (always succeeds) — usable with io.read() like any other handle
```

Every `io` function follows the multi-return convention `safe_div` already establishes at the user
level (see [Error Handling](#error-handling)) — `(value, err)`, `err` non-null on failure. A wrong
argument *type* (not a string path, not an integer handle) is a VM-level runtime error like any
other stdlib type mismatch; a bad handle or a file that can't be opened is the fallible case and
comes back as `err` instead. There's no path sandboxing within `io` itself — same trust model as
any language's file API — the boundary is entirely "does this host expose `io` at all."

`io.read()` works the same way whether the handle is a regular file or `io.stdin()` — a piped,
non-seekable stream falls back to reading until EOF instead of the seek-and-presize approach a
regular file uses, but the call site is identical either way.

---

## Embedding

AER is meant to be linkable directly into a host C application, not just run as a standalone
CLI — the [Practical Applications](#practical-applications) most worth taking seriously
(game logic, an embedded scripting DSL) require it. A host links everything under `source/`
except `source/main.c` (which has its own `main()`), plus `include/aer.h`.

**A runtime error no longer terminates the host process.** Every parse/runtime error and fatal
condition previously called `exit(1)` in file mode — a single scripting bug (wrong argument count,
division by zero, an out-of-bounds index) would kill the entire embedding host, not just the script
call that triggered it. `vm_run()` now returns cleanly on error, in every mode:

```c
#include "aer.h"
#include "vm.h"
/* + lexer.h, parser.h, error.h — see tests/embed_smoke_test.c for a complete,
   minimal example, including how to reuse a VM* across multiple script calls. */

Chunk chunk;
VM    vm;
chunk_init(&chunk);
vm_init(&vm, &chunk);
mode = MODE_RUN;

shell("print(1 / 0)\n");   /* feeds source text directly, no file needed */
lex();
parse(&chunk);
chunk_emit(&chunk, OP_HALT);

bool ok = vm_run(&vm);     /* false — but the process is still running */
if (!ok) printf("script error: %s\n", aer_last_error());
```

`include/aer.h` is the host-facing error-reporting surface:

```
aer_set_error_callback(fn, userdata)  # redirect every error/fatal message somewhere other
                                       # than stderr — a game's own log, a GUI console, etc.
aer_last_error()                      # the most recent message, for polling instead of a callback
aer_had_error()                       # true if the last script invocation errored
aer_clear_error()                     # reset error state before the next invocation
```

**Reusing a `VM*` across multiple calls** (the common embedding pattern — one call per game
frame, for example) requires resetting its stack/call/scope state first if the *previous* call
may have errored mid-execution, since an aborted call leaves scopes pushed that normal execution
would have unwound. This is exactly what the REPL already does between statements internally;
a host driving its own `VM*` needs to replicate it — see `vm_run`'s comment in `source/core/vm.h`
for the precise steps, or `tests/embed_smoke_test.c` for it applied in practice.

**Tearing down file-module imports** — every file loaded via `import` normally lives for the
process's life (see [Memory and Security](#memory-and-security)), but a host that wants to tear
down and reuse the process can call `aer_module_free_all()` (`source/core/aer_module.h`) to free
every loaded module's VM and Chunk and clear the registry. Nothing calls this during normal script
execution; it exists purely for this teardown case.

**Registering custom native functions** lets AER scripts call back *out* to host-defined
functionality — `spawn_enemy()`, `play_sound()`, database access, anything the standard library
doesn't offer. A host function is reached exactly like the built-in `math`/`random`/`string`
modules: registered under a module name, then `import`ed and dot-called from AER:

```c
#include "aer.h"
#include "vm.h"

static Value host_add(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)userdata;
    Value r = {0};
    r.type = TYPE_INTEGER;
    if (arg_count == 2 && args[0].type == TYPE_INTEGER && args[1].type == TYPE_INTEGER)
        r.data.integer = args[0].data.integer + args[1].data.integer;
    return r;
}

aer_register_function("game", "add", host_add, NULL);   /* once, after vm_init() */

shell("import game\nprint(game.add(3, 4))\n");           /* -> 7 */
```

`args` is a plain `Value` array built fresh for this one call — read the values, don't hold onto
the pointer past the call. Return the result `Value` directly; to report an error, call
`error()` (`source/utilities/error.h`), the same recoverable path every other AER error goes
through. Registration is process-global, like the error-reporting state above, and must happen
before any script that references the module is parsed. See `tests/embed_smoke_test.c` for this
exercised end-to-end, and `source/core/aer_host.h` for the full registry API.

Build and run the embedding smoke test (a minimal, complete example of everything above,
including the deliberate-error and VM-reuse cases) with `make test-embed`.

**`aer_io_register()`** (`source/stdlib/aer_io.h`) is the same registration mechanism applied to an
AER-provided (not host-defined) capability — see [File I/O](#file-io--io-an-opt-in-host-capability-not-a-native-module).
It's not called automatically by `vm_init()` — `source/main.c` calls it explicitly, once, right
after `vm_init()`, so the reference CLI's scripts get file access. A different embedding host (a
game, a config parser) simply doesn't call it, and its scripts have none.

---

## Pitfalls and Limitations

### Behaviours that may surprise you

| Behaviour | What happens | Workaround |
|-----------|-------------|------------|
| `/` always returns real | `1 / 1` → `1.0` | Use `//` for integer floor division |
| Arrays and dicts are references | `b = a; b[0] = 99` modifies `a` too | No built-in copy; iterate to clone |
| `&&`/`||` return boolean, not operand | `x = x \|\| "default"` doesn't work | `x = if x: x else: "default"` |
| Undefined variable is a runtime error | `print(x)` before assignment crashes | Check with `x = null` first |
| Missing dict key returns `null` | No error, silent | Use `key in dict` before access |
| `append()`/`delete()` mutate arrays in place and also return them | `arr = append(arr, v)` works but is redundant — the mutation already happened | Call `append(arr, v)` / `delete(arr, i)` as a statement |
| String interpolation: identifiers only | `"{x + 1}"` is a syntax error | `tmp = x + 1; print("{tmp}")` |
| Compound assign not valid on indexed/field targets | `arr[i] += 1` and `p.x += 1` are parse errors | `arr[i] = arr[i] + 1` / `p.x = p.x + 1` |
| `as Type` never converts | `some_dict as Point` errors rather than reshaping the dict into a Point | Build the struct explicitly: `Point(some_dict["x"], ...)` |
| Struct instances are still `AerArray` under the hood | `length(p)` works and returns the field count (not blocked) | Harmless but not the intended API — use dot access |
| Pipe rejects nested calls in target args | `x \|> f(g(1))` is a parse error, at any depth | Assign the inner call to a variable first: `t = g(1); x \|> f(t)` |
| Windows REPL doesn't support piped/redirected stdin | `aer.exe < commands.txt` fails — `_getch()` reads the console directly, bypassing redirection | Run `aer.exe script.aer` (file mode) instead — unaffected, since it never touches the raw-mode terminal code. A file-mode script can still read the piped data itself via `io.stdin()`/`io.read()` (see [Standard Library](#standard-library)) |

### Hard limits

| Limit | Default cap |
|-------|-------------|
| Call stack depth | 64 frames — doesn't apply to true tail calls (see [Functions](#functions)), which reuse the current frame instead of consuming one |
| Value stack | 256 slots |
| Loop nesting | 16 levels |
| Distinct local names per function (parameters + body locals combined) | 32 |
| Destructuring targets | 16 |
| Struct fields | 16 |
| Interpolation buffer | 4 096 chars |
| Dict key length | 4 096 bytes |

### Missing features

- A generational garbage collector reclaims `AerString`/`AerArray`/`AerDict`/`AerFunction`
  (see [Memory and Security](#memory-and-security)), but struct-type registrations still grow
  monotonically for the life of the process — deliberately, since that's what makes REPL
  function/struct persistence work. The file-module registry grows the same way during normal
  execution, but an embedding host can explicitly reclaim it via `aer_module_free_all()` (see
  [Embedding](#embedding)) when tearing the process down.
- No networking, regex, or date/time parsing — `time` covers `now()`/`strftime()` only. File I/O
  exists but is a host-registered opt-in ([Standard Library](#standard-library)), not always
  available.
- No try/catch **at the AER language level** — runtime errors are still not catchable AER
  values; user-level fallibility still uses the multi-return `(value, null)` / `(null, message)`
  convention only. A *host* embedding AER can recover from a script's runtime error via
  `vm_run()`'s return value and `aer_last_error()` (see [Embedding](#embedding)) — what's still
  missing is *AER script code* catching its own errors, a deliberate non-goal.
- File-based `import` is call-only and parse-time-executed — no access to a module's non-function
  bindings, no re-exporting one module's bindings through another, no path-keyed module cache. See
  [Modularity](#modularity) for the full list.
- No variadic functions or keyword arguments. Default parameters exist (trailing, literal-only —
  see [Functions](#functions)), matching the same restriction struct field defaults already have.
- No general type annotations on function parameters — struct fields are the only mandatory-typed
  position (see [Structs](#structs)).
- No struct methods namespaced by type — see [Structs](#structs).
- No concurrency of any kind — see [Concurrency](#concurrency).

---

## Benchmarking

A benchmark script is included at [`tests/benchmark.sh`](tests/benchmark.sh). It compares AER against Python
(required) and Lua (optional) across five workloads: recursive Fibonacci, a tight counting loop, dict
insert/lookup, higher-order function calls, and struct field access.

```sh
chmod +x tests/benchmark.sh
./tests/benchmark.sh
```

**Expected results:** AER beats Python on every workload, and matches or beats Lua outside of the
raw tight-loop case (Lua's own bytecode VM is hard to beat on pure iteration without a JIT). The VM
has no JIT and no type specialisation, so it won't out-loop a specializing interpreter — its
advantage is startup time, simplicity of embedding, and being fast enough that the difference rarely
matters in practice.

### Packed arrays at scale

[`bench/nbody.aer`](bench/nbody.aer) (the classic n-body benchmark, N=5 bodies) is small enough
that its whole working set stays resident in L1 cache regardless of memory layout — it doesn't
exercise packed arrays' actual advantage. [`bench/nbody_large_packed.aer`](bench/nbody_large_packed.aer)
and [`bench/nbody_large_boxed.aer`](bench/nbody_large_boxed.aer) run the identical physics and deterministic
initial conditions at N=1024, differing only in one line: a packed `Body[1024]` versus an ordinary
array of individually heap-allocated `Body()` instances. Measured on a Raspberry Pi (5-run-averaged
`perf stat`, interleaved):

| Metric | Packed | Boxed | Difference |
|--------|-------:|------:|:-----------|
| Instructions | 25.86B | 32.33B | -20.0% |
| Cycles | 14.38B | 18.72B | -23.2% |
| Cache misses | 4.76M (0.048% of refs) | 42.0M (0.307% of refs) | ~8.8x fewer |
| Wall clock | 8.16s | 10.03s | -18.6% |

This is the honest, at-scale number for AER's stated data-oriented-design purpose — the cache-miss
reduction specifically is packed arrays' actual value proposition, not the smaller (and less
dramatic) instruction-count win alone.

### Profile-guided optimization

`make pgo` (see [Building](#building)) measured ~6-9% fewer instructions/cycles than the plain
build on `nbody.aer` — re-run the comparison on your own workload before relying on that exact
number, since PGO's benefit is workload-shaped by construction (it profiles against `nbody.aer`
plus the test suite, not your specific program).

---

## Architecture

This section is the summary; [`ARCHITECTURE.md`](ARCHITECTURE.md) is the full internals reference
(value representation, allocator/GC, the register VM, the compiler, and the optimization history).

AER uses a **single-pass compiler** — the parser emits bytecode directly as it recognises
constructs. There is no intermediate AST. The pipeline is:

```
Source  ──▶  Lexer  ──▶  Parser / Compiler  ──▶  VM
              token         Chunk (bytecode)     dispatch loop
```

### Lexer (`source/compiler/lexer.c`)

Converts source text into a token stream one token at a time. Binary operator tokens are laid out
contiguously in the enum — precedence is looked up with a single array index, not a switch.

Indentation is significant: the lexer emits `TOKEN_INDENT` and `TOKEN_DEDENT` tokens as leading
whitespace increases or decreases, giving the parser Python-style block structure without requiring
braces or `end` keywords.

Escape sequences are handled in the parser rather than the lexer. The lexer emits the raw source
span; the parser's `pool_escaped_string` helper processes `\n`, `\t`, `\\`, `\"`, and `\{` when
building pool entries. Triple-quoted strings are lexed separately (`lex_multiline_string`), allowing
raw newlines and unescaped `"` in their content.

### Parser (`source/compiler/parser.c`)

Recursive-descent. Each `parse_*` function emits bytecode directly into a `Chunk` as it
recognises constructs — no AST is built. Control flow uses **backpatching**: a `0` placeholder is
emitted for jump targets and overwritten once the target address is known.

Binary expressions use **precedence climbing**: one function with a table (see
[Operators](#operators)) handles all binary operators — including `in`, the pipe operator `|>`, and
the cast/shape-check operator `as` (both of which special-case their RHS parsing since it isn't a
general expression) — without a chain of separate grammar rules.

String interpolation is resolved at compile time: `"Hello {name}"` is lowered directly to a chain
of `OP_LOAD` / `OP_TO_STR` / `OP_ADD` instructions with no runtime parsing.

For-each loops compile to register-based iterator opcodes (`OP_ITER_NEXT_ARRAY`/`OP_ITER_NEXT_PAIR`
for arrays/dicts/strings, `OP_ITER_RANGE_PREP`/`OP_ITER_RANGE_LOOP` for `a..b..step` ranges) whose
state lives in a couple of registers the loop already owns — no heap allocation per iteration, and
the range form is loop-rotated (PREP once before the loop, LOOP at the bottom) to match Lua's own
FORLOOP shape rather than paying a separate top-of-loop check plus an unconditional back-edge jump.

`break` and `continue` are compiled with jumps patched directly to the loop's own exit/continue
targets, computed at compile time — there's no separate runtime scope or iterator stack to unwind,
since a loop's iteration state is just ordinary registers.

Struct **names** are tracked in a parse-time table (`struct_names`) as soon as a `struct` statement
is parsed, which is what lets `Type()`/`Type[count]` be recognized immediately after — but the
struct's actual shape (field names, types, defaults) is only registered into the chunk's runtime
shape registry when `OP_DEFINE_STRUCT` actually executes, exactly like a function name is knowable
before its body has run.

`import` is resolved entirely at parse time (see [Modularity](#modularity)) — a file-based import
runs the imported file's code synchronously, in an isolated `Chunk`/`VM`, before the importing
file's own parse continues, which is why it's restricted to the top level of a file.

### VM (`source/core/vm.c`)

A **register-based** virtual machine with a computed-goto dispatch loop (direct-threaded — each
instruction jumps straight to the next handler instead of looping back through a `switch`). Every
instruction word is fetched, masked down to its opcode, and dispatched via a jump table (`dt[]`) in
one `DISPATCH()` macro shared by every handler. No recursion, no pointer chasing to fetch the next
instruction — just sequential array reads.

**Instruction encoding:** most opcodes pack their operands (destination register, RK-encoded
operands, small tags) directly into the high bits of one 64-bit word alongside the opcode itself —
`PACK_BINARY`, `PACK_REG4`, `PACK_CALL_MODULE` and friends, each documented at its own definition in
`vm.h`. An operand that's "RK-encoded" (`vm_rk_ptr9`/`vm_rk_ptr20`) is either a register index or,
with a flag bit set, an index into the chunk's constant pool — one opcode per operator instead of a
family of opcodes per operand-kind combination. A handful of opcodes (`OP_LOADK`, `OP_DEFINE_STRUCT`,
`OP_CALL_MODULE`) can't fit every operand in one word and read one or more trailing plain words
instead, the same convention `source/core/disasm.c`'s decoder follows.

**Registers, not a scope chain:** every function call gets its own contiguous window into one
shared, bump-pointer `VM.register_stack` (`CallFrame.registers`/`frame_size`) — pushing a frame is
just `callee->registers = caller->registers + caller->frame_size`, no heap allocation. The parser
resolves every local (parameter or body variable) to a fixed register index at *parse* time
(`var_slot`, `parser.c`), so there's no runtime name lookup for a function's own locals at all — the
VM only ever reads/writes `registers[N]` directly. A top-level variable is a register in frame 0,
which never moves for the life of the VM (see [Scope inside a function](#scope-inside-a-function)
for why a function body can't reach it by name).

**Tail calls:** `OP_TAIL_CALL`/`OP_TAIL_CALL_VALUE` — emitted instead of `OP_CALL`/`OP_CALL_VALUE`
only when `return f(args)` is the *entire* return expression (see [Tail Calls](#tail-calls)) — share
the exact same dispatch handler as their non-tail counterparts; the opcode value itself is patched
in place once the call is confirmed to qualify. A qualifying tail call overwrites the *current*
frame's own argument registers and jumps straight to the callee's code, reusing the frame instead of
pushing a new one, which is what makes unbounded tail recursion run in constant stack space.

**"Primitive pass" (raw locals):** a local the compiler can prove is always a plain `integer` or
`float` (never reassigned to another type, never read across a branch that could disagree) gets
unboxed storage in `CallFrame.raw_ints`/`raw_reals` instead of an ordinary tagged `AerVal` register,
and a dedicated family of opcodes (`OP_RAW_ADD_INT`, `OP_RAW_LT_REAL`, ...) operates on it directly —
no tag check, no allocation. `OP_BOX_INT`/`OP_BOX_REAL` bridge a raw value back to an ordinary
register at the few places that need one (a call argument, a return value, a container element).

**Field access is inline-cached per callsite:** `Chunk.field_cache` remembers, for each
`.field`-access site in the bytecode, the last `(Shape*, slot)` pair that resolved there — a shape
match on the next visit skips the by-name field scan entirely. Populated lazily and never
invalidated (a struct's field layout never changes after `OP_DEFINE_STRUCT` registers it).

**Packed arrays** (`Type[count]`, see [Packed Arrays](#packed-arrays)) are a separate value type,
`AerPackedArray` — one raw byte block holding `count` instances of one struct type's fields at a
fixed per-field 8-byte slot, addressed by pure arithmetic (`base + i * stride + field_offset`)
instead of `count` individually heap-allocated, pointer-linked instances. Every field is a fixed
primitive (never a heap reference), so a packed array is a leaf for the garbage collector — its own
mark step never recurses into its contents.

**Collections:** `AerArray` and `AerDict` are heap-allocated structs held by pointer inside an
`AerVal`. Assignment copies the pointer — all aliases share the same data. Arrays grow with doubling
reallocation; dicts use `source/utilities/hashtable.c`'s open-addressed hash table (FNV-1a, linear
probing, size-classed slab pools for small key/bucket allocations — see below). A struct instance is
an `AerArray` with a non-`NULL` `shape` pointer into the chunk's struct-type registry — same
allocation and reference semantics as an ordinary array, with bracket/slice/append/delete rejected
at the point of use.

**Garbage collection:** a generational mark-sweep collector. Every pool-managed object
(`AerString`/`AerArray`/`AerDict`/`AerFunction`/`AerPackedArray`) carries its own one-byte GC state
as its literal first field (mark bit, generation bit, free-list bit, remembered-set bit — see
`source/utilities/pool.h`), so the collector never needs a side table to look up an object's state
from its pointer. A write barrier records old-generation objects that come to hold a reference to a
young one (the "remembered set") so a minor collection can trace from them without re-scanning every
old object. `source/utilities/pool.c` is a slab (bump/arena) allocator underneath all of this,
handing out a free-list cell if one exists or the next cell in the current slab otherwise.

**Error handling:** `error()`/`error_at()` (`source/utilities/error.c`) unwind directly back to the
currently-executing `vm_run()` call's own catch point via `setjmp`/`longjmp` (GCC's
`__builtin_setjmp`/`__builtin_longjmp` on MinGW specifically, to sidestep a Windows SEH incompatibility
— see `error.h`'s `AER_JMP_BUF`/`AER_SETJMP`/`AER_LONGJMP`), instead of polling an error flag on every
single dispatch. A nested `vm_run()` call (a cross-module call) installs and restores its own catch
point, so an error inside an imported module's function unwinds only as far as that module's own
call, not past it uncontrolled.

**REPL persistence:** the `Chunk` and `VM` are long-lived across REPL calls. Each line only extends
the bytecode and constant pool; global variables, functions, and struct types all persist because
their registers/registries are never reset between lines.

**Native and file-based modules:** the hardcoded native modules (`math`, `random`, `string`, `time`,
`json`) each dispatch by name from their own file in `source/stdlib/` (`aer_math.c` and friends), all
routed through `OP_CALL_MODULE`'s shared bridge in `vm.c`. `source/core/aer_module.c` handles
file-based imports — each imported file gets its own `Chunk` and `VM`, run to completion once at
import time; calling one of its functions later uses a small trampoline (`setup_call`) that copies
arguments across the VM boundary and runs the module's own VM just far enough to execute that one
call before returning control to the caller.

### Hash table (`source/utilities/hashtable.c`)

FNV-1a hash with open addressing and linear probing, growing at 70% load; deletion uses the reinsert
technique to preserve the probe-chain invariant without tombstones. Backs every `AerDict` and, less
visibly, `Chunk.name_index` (the pool's own string-constant dedup table) — both consumers share the
same size-classed slab pools for small key buffers and bucket arrays, falling back to plain
malloc/xcalloc only past the largest size class.

---

## Design Decisions

Every non-obvious decision in AER was made deliberately. This section documents the reasoning.

### Single-pass compiler

The parser emits bytecode directly as it recognises tokens. There is no intermediate AST and no
separate compilation phase. Each `parse_*` function simultaneously _is_ the grammar rule and the
code generator. The payoff is simplicity: adding a new construct means writing one function, not
touching three. A struct or function's *name* is known as soon as its declaration is parsed (each
kept in its own parse-time table), but its actual body/shape is only registered when that
declaration's own bytecode runs — the same "name known early, definition resolved at its own point
of execution" tradeoff applies to both.

### Packed instruction words, not a flat `int[]`

Bytecode is a `uint64_t[]`, not a plain `int[]` — most opcodes pack their destination register,
RK-encoded operands, and small tags directly into one word alongside the opcode itself (see the VM
section under [Architecture](#architecture)), so a single array read plus a handful of shifts/masks
decodes an entire instruction with no pointer chasing. Only opcodes that can't fit everything in one word
(`OP_LOADK`, `OP_DEFINE_STRUCT`, `OP_CALL_MODULE`, ...) read one or more trailing plain words after
it. The constant pool (`c->pool`) is a separate `AerVal[]` indexed from bytecode.

### `TYPE_NULL` first in enum

`(AerVal){0}` is zero-initialised and therefore always a valid null — `mark_vm_roots` (`vm.c`) can
scan every register unconditionally, including one no instruction has written to yet, and get a
harmless null back instead of an arbitrary tag. The same zero-value invariant holds for the public,
embedding-facing `Value` struct. No sentinel magic needed either way.

### Backpatching

Jump targets are unknown at emit time (the body hasn't been compiled yet). AER emits `0` as a
placeholder, then overwrites it once the target address is known. Every `if`, `for`, function body,
and short-circuit operator uses this pattern.

### Precedence climbing

One function with a table handles all binary operators (see [Operators](#operators)). To add a new
operator, add one row to the table — `in`, `|>`, and `as` all went in this way, the latter two
needing a small special-case branch in the climbing loop only because their right-hand side isn't a
general expression.

### Iterator state lives in ordinary registers

`for x in arr:` keeps its collection reference and index in two registers the loop already owns,
advanced in place by `OP_ITER_NEXT_ARRAY`/`OP_ITER_NEXT_PAIR` each iteration — no heap allocation,
and no separate iterator stack to unwind on `break`, since `break`/`continue` compile straight to a
jump. The integer-range form (`a..b..step`) goes further: `OP_ITER_RANGE_PREP` computes a total
iteration count once before the loop, and `OP_ITER_RANGE_LOOP` counts down at the *bottom* of the
loop body, matching Lua's own FORLOOP shape — no unconditional back-edge jump instruction is ever
emitted for this loop form at all.

### Short-circuit `&&`/`||` always produce boolean

Rather than returning the operand value (as JavaScript does), AER's `&&`/`||` always produce
`true` or `false`. `x = x || "default"` does not work the way it does in JavaScript.
`x = if x: x else: "default"` is the idiomatic replacement.

### Named functions can't nest; no closures at all

**Named** function statements (`function foo(): ...`) are rejected at parse time if nested — a
nested function's variable lookups would only work by accident, only while the enclosing call is
still on the stack. Anonymous function expressions (`function(...): ...`) *can* be defined inside
another function's body, but AER doesn't give them a capture mechanism: a nested function
expression can only see its own parameters and locals, exactly like a top-level function (top-level
variables are off-limits inside any function, nested or not — see
[Scope inside a function](#scope-inside-a-function)) — referencing the enclosing function's
variable is the same `'x' is not defined` (or, if the name happens to be a top-level variable, the
"not accessible inside a function" error) either way, not anything silently wrong.

This was a deliberate choice, not a missing feature: a closure is a function value carrying
*implicit* bound state from wherever it happened to be created, invisible at every later call site —
the same category of hidden indirection AER avoids elsewhere in the language. Sharing state across
calls still works — just explicitly, by mutating a struct/array/dict you were actually passed as a
parameter (see [Scope inside a function](#scope-inside-a-function)) — the same idiom C uses for a
callback that needs extra context (`void* userdata`), rather than a compiler-managed heap promotion
behind the scenes.

### Every local is a fixed slot; function calls only ever see their own scope or globals

Assignment inside a function body is always local (see
[Scope inside a function](#scope-inside-a-function)) — there is no walk-up-and-mutate-outer
ambiguity to resolve, so every name a function body assigns or reads (other than a reference to
another function/struct) is resolvable to a fixed register at *parse* time, not a runtime lookup.
Every local — parameter or body variable alike — gets exactly the same fast path: a direct register
index, read/written by whatever opcode the expression already needed (there's no separate
load/store opcode for "a local" versus any other register, since a local's storage *is* just a
register). An earlier "hybrid scopes" design (locals fast-pathed like parameters, everything else
name-based) was tried and reverted before this rule existed, specifically because the *old*
semantics — a plain assignment walking up to mutate an existing outer/global binding — couldn't be
resolved at parse time in the general case. Removing that walk-up rule in favor of always-local is
what made the full flat-register model possible.

A lookup that isn't resolved to a local at parse time is either a compile error (the name is a
top-level variable, off-limits from inside any function — see
[Scope inside a function](#scope-inside-a-function)) or a reference to another function/struct
name, resolved once at parse time exactly like a local would be — there is no runtime walk-up to
any enclosing or caller scope at all, since nested function definitions are rejected and there's no
capture path. The payoff for recursion is unchanged: resolving a recursive function's own name from
deep in a call chain still costs one hop, not one per stack frame.

### Structs are arrays with a shape, not a new value type

A struct instance is a plain `AerArray` carrying a `Shape*` — not a distinct `ValueType`. Every
existing array-shaped mechanic (heap allocation, reference-copy-on-assign, printing recursion)
applies for free. The tradeoff is that structs need explicit guards to stay conceptually distinct
from plain arrays (see [Structs](#structs) — bracket/slice/append/delete are rejected on a shaped
array precisely because the representation would otherwise let them through silently.

### `as` unifies casting and shape-checking under one operator

Rather than a separate keyword or syntax for "verify this is a Player," `as` was extended to cover
it: for primitive targets it coerces, for struct targets it verifies (never converts, since
struct-to-struct conversion has no single well-defined rule). One operator, one consistent
question — "produce or confirm a value of type T" — answered differently depending on what T is.

### Pipe enforces no nested calls in its target's arguments

`x |> f(g(1))` is rejected, at any nesting depth, including a nested pipe's own call. This keeps
a pipe chain a flat, linear sequence you can read top-to-bottom, rather than a spot where calls can
hide inside calls the way they can in ordinary nested function-call syntax.

### REPL runtime errors abort the statement, not the process

A runtime error (out-of-bounds, wrong type, etc.) in the REPL stops the rest of the current
statement from executing and returns control to the prompt — it does not continue running with a
placeholder value, and it does not exit the process. File execution still exits immediately on any
error, unchanged.

### Dictionary keys are always strings

No mixed-type key lookups, no hash collision between integer `1` and string `"1"`. Keeps the
hashmap implementation simple and the semantics predictable.

### REPL chunk monotonic growth

The bytecode chunk never resets across REPL calls — new bytecode is appended. Function bodies and
struct declarations compiled in earlier calls remain valid. Each REPL call starts the VM at the new
code's start address. This is why functions and structs defined interactively persist across calls.

### Modules resolved entirely at parse time, no runtime module value

Neither a native module (`math`) nor a file-based one (`helpers`) is ever a real scope variable or
runtime `Value` — `math.sqrt(x)` and `helpers.double(x)` are recognized and compiled directly by
the parser into a dedicated call opcode the moment it sees a name it already knows was `import`ed.
This keeps the `Value` union exactly as lean as it was before modules existed — no `TYPE_MODULE`
variant, no namespace object allocation — at the cost of member access being call-only for now (see
[Modularity](#modularity)) rather than general value access.

### File-based imports run in a fully separate VM, not a pushed scope

The obvious-looking shortcut — push a new scope and run the imported file's top-level code there —
doesn't provide real isolation, because a scope lookup that isn't found locally still falls back to
`scopes[0]`, the *same* global scope the importing program uses. An imported file's `x = 5` would
silently overwrite the importing program's own `x`. A genuinely separate `Chunk` and `VM` per
imported file avoids this entirely, at the cost of needing a small cross-VM call trampoline
(`aer_module_call`, reusing the shared `vm_setup_call` helper) to invoke one of its functions later.

---

## Memory and Security

### Memory management

AER runs a **generational mark-and-sweep collector** over six pooled heap types (`AerString`,
`AerArray`, `AerDict`, `AerFunction`, struct instances in their own pool separate from ordinary
arrays, and `AerPackedArray` — `source/utilities/pool.c`). Every pool cell carries one byte of
state: a mark bit (this collection cycle only), a generation bit (young/old — promoted the first
time a cell survives any collection), a free-list bit, and a remembered-set bit (see below).
Allocation is unchanged from the pooled/slab design (a free-list pop, or a bump into the current
slab) — collection is what's new.

**Two collection modes, one shared heap.** A *minor* collection traces the normal roots (the VM
stack and every live call frame's registers) plus a *remembered set* — old
objects a write barrier caught being mutated to hold a young reference — and only sweeps young
cells; old cells are presumed live and left untouched, which is what keeps minor collections cheap.
A *major* collection (run periodically, after a fixed number of minor ones) traces the same roots
with no remembered set needed and sweeps both generations. The six pools are process-global and
shared by the main VM *and* every file-module's own VM (`import` still runs each file in a fully
separate `Chunk`+`VM` — see [Modularity](#modularity)), so a collection triggered anywhere marks
every loaded module's roots too, not just the VM that triggered it.

**The write barrier** — the mechanism that makes minor collections safe — only has two real call
sites: array item writes (index-assignment, `append`, struct field assignment) and dict entry
writes. Stack and scope writes need no barrier at all: both are small and fully re-walked as roots
on *every* collection regardless of generation, so anything reachable from them is never missed by a
minor pass. Remembered-set entries are added but never proactively removed — simpler, and impossible
to get a removal check wrong — except that a later *major* collection can legitimately free an
object that's still listed; each entry's liveness is checked before it's re-traced, and dead entries
are quietly dropped when found, rather than ever dereferencing a stale one.

**What's still permanent, deliberately**: `Chunk`s, `Shape` (struct-type) registrations, and the
file-module registry are never collected — this is what makes REPL function/struct persistence work
("the bytecode array and struct registry grow monotonically and are never reset") and isn't a gap,
it's the same policy this project always had for those three, now with everything *else* actually
reclaimed around them.

Introspect a running VM via `aer_gc_stats()` (`include/aer.h`) — live cell count, minor collections
run, major collections run.

**Two knobs, both optional.** `aer_gc_configure(minor_threshold, major_every_n_minor)` overrides the
tuning constants above (2048 and 10 by default) — pass `0` for either argument to leave that one
alone. `aer_gc_set_ceiling(max_live_cells)` caps total live cells across all six pools — a
`-Xmx`-style limit, `0` (the default) meaning unlimited. Hitting the ceiling doesn't crash the host:
the collector forces one extra major pass first (in case a cheap collection alone would've freed
enough), and only if the script is *still* over the limit does it abort with a normal, recoverable
runtime error (`aer_last_error()`) — the same non-fatal path every other runtime fault already takes.

### Security concerns

| Issue | Risk | Status |
|-------|------|--------|
| Out-of-memory is still fatal | `aer_report_fatal()` calls `exit(1)` — the one remaining unconditional process exit | By design for this phase; a configurable memory ceiling (rejecting an allocation, or forcing a collection first) is a natural next step now that the collector exists, but isn't built |
| No arithmetic overflow checks | `arr[9999999999999]` on a 32-bit platform behaves unexpectedly | Array bounds are checked; index arithmetic is not |
| File I/O is host-registered opt-in, not ambient | `io` (see [Standard Library](#standard-library)) gives the reference CLI real file access; an embedding host simply doesn't get it unless it calls `aer_io_register()` itself | The sandboxing boundary is "does this host register `io`," not any restriction within `io` once granted — same trust model as any language's file API |
| File-based `import` reads arbitrary files by name | `import` resolves and executes `<name>.aer` (or a file found via `AER_PATH`) from disk with no sandboxing | Same-directory/`AER_PATH` resolution limits the blast radius somewhat; a real permissions model would be needed before embedding AER in a context where the script source isn't trusted |

Dict key lookups (`in`, `delete`, index get/set) build a null-terminated copy of the key into a
fixed `VM_KEY_MAX` (4096 byte) stack buffer rather than a length-sized VLA, so an oversized key
raises a normal AER runtime error instead of risking a stack overflow.

---

## File Map

| File | Purpose |
|------|---------|
| `source/main.c` | Entry point, REPL loop, file runner |
| `source/terminal.h/c` | Raw-mode interactive REPL terminal |
| `source/value.h` | `AerVal` (the internal tagged-union value) and `Value` (the public, embedding-boundary struct): null / boolean / integer / real / string / function / array / dict / packed array, and the `Shape` forward declaration |
| `source/compiler/lexer.h/c` | Source text → token stream, indent/dedent tracking |
| `source/compiler/parser.h/c` | Single-pass compiler: tokens → register-based bytecode, escape processing |
| `source/core/vm.h/c` | Bytecode chunk, register-based VM (`CallFrame`/bump-pointer register stack), struct-type registry, computed-goto dispatch loop, built-ins |
| `source/stdlib/aer_stdlib.h` | Declares the entire native-module surface (math/random/string/time/json/io) — one header for a fixed, closed set |
| `source/stdlib/aer_stdlib.c` | Native-module registry: `aer_stdlib_init()`, `aer_stdlib_is_native_module()` |
| `source/stdlib/aer_math.c` / `aer_random.c` / `aer_string.c` / `aer_time.c` | One file per hardcoded native module, dispatched by `vm.c`'s `OP_CALL_MODULE` switch |
| `source/stdlib/aer_json.c` | `json` module — encode/decode, dispatched the same way as the four above |
| `source/stdlib/aer_io.c` | `io` module (file open/read/write/close) — opt-in per host via `aer_io_register()`, not hardcoded like the others |
| `source/core/aer_module.h/c` | File-based `import` — resolution, isolated per-file `Chunk`/`VM`, cross-VM call trampoline |
| `source/core/aer_host.h/c` | Host-registered native function registry (`aer_register_function`) — reached from AER the same way as `math`/`random`/`string` |
| `source/utilities/hashtable.h/c` | FNV-1a open-addressing hash table backing every `AerDict` and `Chunk`'s own string-constant dedup table, with size-classed slab pools for small key/bucket allocations |
| `source/utilities/pool.h/c` | Slab (bump/arena) allocator extended for the generational mark-sweep garbage collector — every pool-managed struct (`AerString`/`AerArray`/`AerDict`/`AerFunction`/`AerPackedArray`) carries its own one-byte GC state as its literal first field |
| `source/utilities/error.h/c` | Error reporting with source location and column pointer; recoverable-error sink (callback or stderr), `aer_report_fatal` for genuinely unrecoverable conditions, `assert_failure_count` |
| `include/aer.h` | Public embedding API: version constant, error callback/query functions, custom native-function registration (see [Embedding](#embedding)) |
| `tests/test_*.aer` | Runnable documentation and regression suites — `assert()`-based, exits nonzero on any failure. Run with `make test`. |
| `ARCHITECTURE.md` | Internals-facing reference — value representation, allocator/GC, the register VM, the compiler, and the optimizations layered on top. The [Architecture](#architecture) section here is the summary; that document is the full story. |
| `tests/embed_smoke_test.c` | Minimal standalone embedding host — proves a runtime error doesn't kill the process, demonstrates the VM-reuse-after-error contract, and registers/calls a custom host function. Build/run with `make test-embed`. |
| `tests/smoke_test.c` | Register-VM unit test — hand-built bytecode plus real-source coverage below the level of a full `.aer` file. Build/run with `make test-smoke`. |
| `tests/fuzz.py` | Mutation-based fuzzer against an ASAN build — reports crashes and hangs. Run with `make fuzz`. |
| `tests/benchmark.sh` | Cross-language benchmark against Python/Lua (see [Benchmarking](#benchmarking)) |
| `bench/` | Standalone benchmark scripts — `nbody.aer`/`nbody_large_*.aer` (packed arrays, see [Benchmarking](#benchmarking)), `fib_bench.aer`, `sieve.aer`, micro-benches, plus the Python/Lua reference versions of nbody and sieve |
| `.github/workflows/ci.yml` | CI: builds, runs `make test`, `make test-embed`, `make fuzz` (ASAN, fixed seed), and `make coverage` |

---
