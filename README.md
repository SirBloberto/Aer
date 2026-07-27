# AER

AER is a small, dynamically-typed scripting language written in C. The name comes from the ancient
Greek and Latin word for *air* — the language is designed around three values: **light** (minimal
codebase, no bloat), **fast** (bytecode VM, not tree-walking), and **easy to understand** (both the
language syntax and the implementation).

## Contents

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
  - [Hashtables](#hashtables)
  - [Structs](#structs)
  - [Method Calls and Pipes](#method-calls-and-pipes)
  - [Error Handling](#error-handling)
  - [Modularity](#modularity)
  - [Concurrency](#concurrency)
- [Standard Library](#standard-library)
- [Embedding](#embedding)
- [Pitfalls and Limitations](#pitfalls-and-limitations)
- [Architecture](#architecture)
- [Design Decisions](#design-decisions)
- [Memory and Security](#memory-and-security)
- [File Map](#file-map)
- [License](#license)

---

## Practical Applications

AER is genuinely suited to:

| Use case | Why AER fits |
|----------|-------------|
| Embedded scripting DSL | Self-contained C VM, no external deps, embeds in any C project |
| Game logic / NPC behaviour | First-class functions, structs, hashtables, and arrays cover most logic patterns; lightweight enough for per-frame calls |
| Config with computation | More expressive than JSON/TOML; simpler to embed than Lua |
| Teaching language design | Full compiler + VM in a few thousand lines of readable C; fits on one screen at a time |
| Automation scripts | Real data structures without Python startup overhead |

Runnable examples live in `examples/` (`./binary/aer examples/<name>.aer`):

| File | Demonstrates |
|------|-------------|
| `battle.aer` | Structs, the pipe operator, `random`/`string` |
| `fetch_page.aer` | `net` — a hand-built HTTP GET client over a raw TCP socket |
| `log_redact.aer` | `regex` — classifying and redacting lines with match/find/replace |
| `wordcount.aer` | `io`/`io.args()` — a small CLI utility, run as `./binary/aer examples/wordcount.aer <file>` |
| `actor_worker.aer` + `actors_demo.aer` | `actor`/`scheduler` — spawn, synchronous calls, the mailbox, and the cooperative scheduler |
| `embedding_example.c` | Embedding AER in a host C program (`make example-embed`) — the short version; `tests/embed_smoke_test.c` is the exhaustive one |

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
One build for everything — it carries debug symbols and is the same binary the test suite runs.
(PGO (`-fprofile-generate`, run against a representative `.aer` workload, rebuild with
`-fprofile-use`) was measured again on this VM's current opcode set: a few percent faster on
`nbody.aer`, but ~35-40% SLOWER on `struct_array_scan.aer` — reproducible even when the profile is
trained on that exact benchmark alone, so it's a real GCC hot/cold layout decision going wrong for
that loop, not a training-mix artifact. Given a flagship benchmark regresses this badly, it isn't
wired into the makefile.)

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

A handful of global flags work in any mode (before the script path, if there is one):

```sh
./binary/aer --no-io script.aer            # disable the io module for this run
./binary/aer --no-net script.aer           # disable the net module for this run
./binary/aer --no-import script.aer        # disable file-based import (fixed stdlib modules still work)
./binary/aer --memory-size=64M script.aer  # cap live GC cells (not bytes) at 64,000,000 — see below
```

`--memory-size` is a cell-count ceiling with a familiar-looking suffix, not a byte-accurate memory
limit — `aer_gc_set_ceiling()` (the function this maps to) counts live GC cells, and cell sizes
differ per pool (a string cell isn't the size of a hashtable cell), so there's no accurate bytes-to-cells
conversion without a much bigger per-allocation byte-accounting subsystem this project doesn't
have. `K`/`M`/`G` multiply by 1,000/1,000,000/1,000,000,000 cells. See
[Embedding](#embedding)/[Security concerns](#security-concerns) for what these flags actually do and
don't provide.

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

AER has **20 reserved words**, plus the two boolean literals. That's the entire list — nothing else
in the language is reserved:

| Keyword | Role |
|---------|------|
| `if` / `else` | conditional statement |
| `for` | the single iteration keyword — while, for-each, and ranges all use it |
| `in` | membership test, and the `for x in ...` iteration form |
| `and` / `or` / `not` | logical operators (see [Operators](#operators) for precedence) |
| `struct` | declare a fixed-shape record type |
| `function` | declare a named function, or start an anonymous function value |
| `return` | return from a function, optionally with a value (or several) |
| `raise` | signal a recoverable failure from a function (see [Error Handling](#error-handling)) |
| `break` / `continue` | loop control |
| `null` | the absence-of-a-value literal |
| `import` | bring a native or file-based module into scope |
| `true` / `false` | boolean literals |
| `integer` / `float` / `boolean` | primitive type names — only meaningful as a cast call (`integer(x)`), but reserved everywhere so they can never be shadowed |
| `array` / `hashtable` | collection type names — not valid cast targets themselves (there's no generic value-to-collection conversion), but reserved for the same reason |

`string` is deliberately **not** on this list, even though it's a valid cast call (`string(x)`) — it
collides with the stdlib `string` module (`import string`), so it stays an ordinary identifier like
every other module name, matched by text rather than reserved. This costs nothing in practice:
`string(x)` used as a cast and `string.upper(s)` used as a module call are both recognized by the
parser from context, so `string` was never shadowable to begin with.

**Everything else is an ordinary identifier**, including every stdlib module name
(`math`, `random`, `string`, `time`, `collection`, `net`, `regex`, `json`, `io`) — none of these are
keywords, and they can be shadowed by a local variable or parameter of the same name, composing with
everything else a function value can (passed around, stored in a variable, piped through `|>`).

**One narrower exception:** `print`, `length`, `type`, `assert`, `panic`, and `Result` (see
[Built-in Functions](#built-in-functions)) aren't reserved words — they're ordinary identifiers,
same as a stdlib module name — but calling one, e.g. `Result(value, err)`, always resolves to the
real builtin regardless of any same-named local variable or function, and declaring a function with
one of these names is a compile error. This is deliberately stricter than module-name shadowing:
ordinary function-call resolution checks user-defined functions before falling back to a builtin, so
without this guard a script defining its own `function Result(a, b): ...` would silently hijack
every `Result(...)` call site with no error at all — module names don't carry this risk, since
shadowing one is an intentional, well-understood feature, not an accidental collision with a
fixed, load-bearing builtin.

## Operators

Binary operators are looked up by one precedence table, from lowest to highest:

| Precedence | Operators | Meaning |
|:-:|-----------|---------|
| 1 (lowest) | `or`  `\|>` | logical or (short-circuit) · pipe |
| 2 | `and` | logical and (short-circuit) |
| 3 | `not` | logical not |
| 4 | `\|` | bitwise or |
| 5 | `^` | bitwise xor |
| 6 | `&` | bitwise and |
| 7 | `==`  `!=` | equality |
| 8 | `<`  `>`  `<=`  `>=`  `in` | comparison · membership |
| 9 | `<<`  `>>` | bit shift |
| 10 | `+`  `-` | add · subtract |
| 11 (highest) | `*`  `/`  `%`  `//` | multiply · true-divide · modulo · floor-divide |

`in` sits at comparison precedence, not its own tier — `a == b in list` parses as `a == (b in list)`.

There is no cast/shape-check operator in this table at all — `integer(x)`/`float(x)`/`boolean(x)`/
`string(x)` and struct shape-checks (`type(x) == "Point"`) are ordinary calls and comparisons, so
their "precedence" is just a call's parens and `==`'s own tier; there's no separate precedence rule
to learn for casting the way Rust's `as` needs one.

`not` sits between `and`/`or` and everything else — tighter than `and`/`or`, looser than
comparison/`in`/arithmetic — matching Python. This is why `not "age" in person` reads as
`not ("age" in person)` rather than `(not "age") in person`: `not`'s operand grabs the whole `in`
expression before `not` itself is applied. `-`/`~` don't share this: they bind tighter than any
binary operator.

**Unary** (bind tighter than any binary operator): `-x` (negate), `~x` (bitwise not). `not` is
unary too but sits at its own, looser precedence — see above, not this list.

**Assignment** is a statement, not an expression, and isn't part of the precedence table at all:
`=`, and the arithmetic compound forms `+=  -=  *=  /=  %=  //=`. Compound assignment
works on plain names, indexed targets, and dot-field targets alike — `x += 1`, `arr[i] += 1`, and
`p.x += 1` are all supported, at any chain depth (`bodies[i].pos[0].x += v`). There are
deliberately no bitwise compound forms (`&=`, `<<=`, ...) — a second spelling of `x = x & m` with
no new capability; write it out.

`/` always performs true division and returns a float; `//` is floor division, rounding toward
negative infinity (matching Python, not C):

```
10 / 3          # 3.333...
10 // 3         # 3
-7 // 2         # -4   — floors toward negative infinity, not toward zero
```

`and` and `or` **return the deciding operand itself**, not a coerced boolean (Python/Lua semantics,
not C/JS's strict-boolean `&&`/`||`): `a or b` is `a` if `a` is truthy, else `b`; `a and b` is `a` if
`a` is falsy, else `b`. Both short-circuit — the right-hand side is only evaluated if the left side
doesn't already determine the result. This is what makes `x = x or "default"` work as a default-value
idiom:

```
name = user_input or "Anonymous"    # "Anonymous" if user_input is falsy (null, "", 0, ...)
```

## Built-in Functions

Six **core builtins** are always available, with no `import` — ordinary identifiers dispatched
directly by the VM, not syntax. The bar for being a builtin is "meaningful for (almost) any value":

| Function | Signature | Behaviour |
|----------|-----------|-----------|
| `print(x)` | 1 arg | writes `x`'s string form to stdout, followed by a newline |
| `type(x)` | 1 arg | returns `x`'s type name as a string (a struct instance returns its declared name) |
| `length(x)` | 1 arg | element count of an array, entry count of a hashtable, or a string's character count |
| `assert(cond, msg)` | 2 args | prints `ASSERT FAILED: msg` on a false `cond` and keeps running — see [Error Handling](#error-handling) |
| `panic(msg)` | 1 arg | aborts like any runtime error, with your own message — see [Error Handling](#error-handling) |
| `Result(value, err)` | 2 args | builds a genuine `Result` — exactly one argument must be null — capitalized like a struct constructor, not a plain builtin (see [Error Handling](#error-handling)) |

Everything past this — `math`, `random`, `string`, `time`, `collection`, `io` — requires an
explicit `import` and is covered in [Standard Library](#standard-library). In particular
`append`/`delete` live in `collection`, alongside every other array/hashtable operation.

## Syntax

Indentation is significant, Python-style — a block starts with a line ending in `:` and is indented
one level deeper than its header; there are no braces or `end` keywords.

```
if x > 0:
    print(x)
```

`if`, `else`, `for`, and `function` bodies are **always** the indented block form — there is no
same-line single-statement shortcut. This is a deliberate one-way-to-do-it choice: a same-line form
would be a second, purely visual spelling of the identical thing.

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
| Hashtable | `{"a": 1}` | Mutable string-keyed; reference semantics — see [Hashtables](#hashtables) |

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

Missing hashtable keys return `null` rather than erroring — see [Hashtables](#hashtables).

### Numbers

Mixing an integer and a float promotes the integer:

```
1 + 2.5         # 3.5
```

### Strings

String literals use double quotes. Supported escape sequences:

| Sequence | Result |
|----------|--------|
| `\n` | newline |
| `\t` | tab |
| `\r` | carriage return |
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

Strings support the same comparison operators numbers do — `<`/`>`/`<=`/`>=` compare
lexicographically (byte-wise, then by length if one is a prefix of the other; the same order
`collection.sort()` already uses for an array of strings), and `in` tests substring membership:

```
print("apple" < "banana")   # true
print("ell" in "hello")     # true — same question string.contains() answers
```

Strings are immutable — `word[0] = "H"` is a runtime error.

**Interpolation**: any `{expr}` inside a string is replaced with that expression's string form, no
prefix required — a bare identifier is just the simplest case; calls, arithmetic, indexing, and
field access all work too:

```
name = "Robert"
age  = 30
print("Hello {name}, you are {age} years old")
# Hello Robert, you are 30 years old

function double(n):
    return n * 2
print("{name} is {double(age)} in dog years")
# Robert is 60 in dog years
```

`{expr}` is parsed as a genuine sub-expression (its own independent lex/parse pass, saved and
restored around the outer string), not a text-to-variable lookup — so it accepts anything
`parse_binary` does, including a nested `{}` (a hashtable literal). The one thing it *doesn't* handle is
a nested string literal's own quotes: the outer string's own lexing decides where the whole string
token ends before interpolation ever runs, and it has no idea `{}` exists — so a literal `"` inside
an interpolated expression still needs the same `\"` escaping any other embedded quote would:
`"{greet(\"world\")}"`. To include a literal `{` (not an interpolation), escape it: `"\{name}"`
prints `{name}`.

### Casting and Shape-Checking

Primitive casts are ordinary function calls — `integer(x)`, `float(x)`, `boolean(x)`, `string(x)` —
each **coercing** `x` to that primitive type:

```
print(integer("42"))     # 42
print(float(42))         # 42.0
print(string(42))        # "42"
print(integer(3.7))      # 3   (truncates toward zero, does not round)
print(boolean(0))        # false — numeric/string coercion to boolean uses the same truthiness rule as `if`
```

`integer("abc")`/`float("abc")` is a runtime error, not a silent `0` — the whole string (leading/
trailing whitespace aside) must be a valid number, or it's rejected rather than fabricating a
plausible-looking wrong value.

Checking a struct's shape is an ordinary comparison against `type(x)`, the same builtin that reports
any value's type name (see [Built-in Functions](#built-in-functions)):

```
p = Point(1.0, 2.0)
print(type(p) == "Point")     # true
if type(e) != "Point":
    panic("expected a Point")  # write the "assert or crash" behaviour explicitly when you want it
```

This is a genuine query, not an assertion — unlike a cast, a shape check never errors on a mismatch
by itself; you decide what to do with the `false`. There's still no way to convert one struct shape
into another — build the target explicitly: `Point(some_table["x"], ...)`.

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

There is no inline conditional expression — assign inside each branch instead:

```
if x >= 0:
    abs_x = x
else:
    abs_x = -x
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

**Hashtable iteration — keys only, or key-value pairs:**

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
struct, array, or hashtable — all reference types), instead of relying on a function reaching outward
by bare name:

```
struct Counter:
    value = 0

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

import collection
collection.append(arr, 4)  # append (mutates in place; the returned array is redundant to capture)
collection.delete(arr, 1)  # remove index 1, shifting subsequent elements down (preserves order)
print(length(arr))    # length
```

Growing, shrinking, reordering, and copying all live in the `collection` module — see
[Standard Library](#standard-library) for the full list (`append`, `delete`, `insert`, `index_of`,
`copy`, `keys`, `sort`). `collection.delete()` on an array removes by index (negative indices
allowed, out-of-range errors) rather than by value, and returns the array. There is no separate
"remove the last element" function; `collection.delete(arr, -1)` covers it.

Slicing returns a new array — `[a:b]`, `[a:]`, `[:b]`, `[:]`. Out-of-range bounds are clamped
rather than erroring, matching Python's slicing behavior:

```
nums = [0, 1, 2, 3, 4, 5]
print(nums[1:4])     # [1, 2, 3]
print(nums[3:])      # [3, 4, 5]
print(nums[:3])      # [0, 1, 2]
```

Index access chains — nested arrays and hashtables support `[...][...]` syntax:

```
matrix = [[1, 2], [3, 4]]
print(matrix[0][1])           # 2

matrix[0][1] = 99            # nested assignment
print(matrix[0])              # [1, 99]
```

## Hashtables

Hashtables are mutable string-keyed hash tables with reference semantics. Missing keys return `null`
rather than erroring.

```
d = {"x": 1, "y": 2}
print(d["x"])           # 1
print(d["missing"])     # null

d["z"] = 3
"z" in d                # true
collection.delete(d, "y")  # remove key (import collection)
length(d)                # number of entries
```

Hashtable keys are always strings — no mixed-type key lookups, no hash collision between
integer `1` and string `"1"`.

Iterating a hashtable (`for k in d:`, `print(d)`, `json.encode(d)`) visits entries in an unspecified
internal order that can change across insertions and removals — don't rely on a hashtable
preserving the order its keys were added in.

## Structs

Structs are AER's fixed-shape record type — the closest thing to a "class," minus methods (see
[Method Calls and Pipes](#method-calls-and-pipes) for how behaviour attaches to them instead).
Declared with a field list, each field given a literal default:

```
struct Point:
    x = 0.0
    y = 0.0
```

**Every field must have an explicit default — there's no separate type annotation at all.** A
field's type is always exactly its default's type: `0.0` makes `x` a `float` field, `0` would make
it `integer`, `""` a `string`, `[]` an `array`, `{}` a `hashtable`. `null` is the one default with no
matching type — it leaves that field genuinely unconstrained, since there's no dedicated "accepts
anything" keyword (a nested struct instance, which can't be written as a literal default, is the
main reason to reach for this — see `Outer`/`Inner` below). Defaults must be literals — no
arbitrary expressions — since the whole declaration compiles to a single instruction that registers
the shape. Assigning a value of the wrong type to a typed field is a runtime error, the same
protection function parameters don't get; a `null`-defaulted field has no such protection, by
design.

```
struct Inner:
    v = 0
struct Outer:
    inner = null    # unconstrained -- a nested struct's own type isn't a valid literal default
o = Outer(Inner(5))
```

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
don't mix. Bracket indexing, slicing, `collection.append()`, and `collection.delete()` are all rejected on a struct
instance:

```
# p1[0]           # error: use '.' not '[]'
# collection.append(p1, 1)    # error: structs have a fixed shape
```

AER has no methods — all functions live in one flat global scope. A function that operates on a
struct just takes it as a plain parameter:

```
function point_translate(p, dx, dy):
    return Point(p.x + dx, p.y + dy)

point_translate(p1, 1.0, 1.0)
```

`type(p) == "Point"` is a standalone runtime shape check, not part of a function signature — see
[Casting and Shape-Checking](#casting-and-shape-checking).

### Narrow Struct Fields

An `i`- or `f`-suffixed literal default (`x = 42i`, `y = 0.0f`) declares that field as narrow —
stored in 4 bytes (`int32`/`float32`) instead of the usual 8 (`integer`/`float`, which stay 64-bit
everywhere else in the language; this is purely a storage-size opt-in, not a second numeric type
you can write ordinary arithmetic against differently):

```
struct Point:
    x = 0i
    y = 0.0f
    label = "point"

p = Point(5, 2.5, "p1")
print(p.x)   # 5 -- reads back as an ordinary integer
p.x += 10    # ordinary compound assignment works
```

A narrow field reads back as an ordinary `integer`/`float` value — nothing about using it looks any
different from a normal field. `p.x = 5000000000` (or a construction argument, or the field's own
default) is a runtime error if it doesn't fit an `int32`, rather than silently wrapping.

Narrow fields get both the memory-density win (4 bytes instead of 8, in a plain struct instance or
as a packed-array element alike — `[Point(); n]` works fine with narrow fields) and the same
per-access speed a wide field gets: the VM's shape-specialization fast path has its own narrow
counterparts of every raw field-access opcode, widening a field's 4-byte storage into an ordinary
64-bit register for computation and narrowing the result back down on write — so a tight loop
reading/writing a narrow field specializes exactly like a wide one would, just with a smaller
memory footprint to stream through.

### Repeat-Literal Arrays

`[value; count]` evaluates `value` exactly once, then builds a dense array of `count` copies —
Rust's own repeat-literal syntax, the one place in AER's grammar with a semicolon. What kind of
array comes out depends entirely on `value`'s own type:

- **A struct instance** (`Point()`, `Point(1.0, 2.0)`, ...) produces a **packed array**: `count`
  instances of that struct, packed inline in one contiguous block instead of `count` separately
  heap-allocated instances linked through an ordinary array of references. This is AER's answer to
  data-oriented design: a tight loop over a large packed array touches far fewer cache lines than
  the same loop over an ordinary array of struct instances. Measured at scale (N=1024, Raspberry Pi,
  5-run-averaged `perf stat`): a packed 1024-body array versus an ordinary array of heap-allocated
  instances is -20% instructions, -23% cycles, ~8.8x fewer cache misses, -18.6% wall clock — the
  cache-miss reduction is packed arrays' actual value proposition, not visible at small N where
  everything fits in L1.
- **A number** (`0`, `0.0`, or an `i`/`f`-suffixed narrow literal — see below) produces a **typed
  array**: a dense, uniformly-typed numeric array with no struct or `Shape` involved at all.

```
struct Body:
    x = 0.0
    y = 0.0
    mass = 0.0

bodies = [Body(); 1024]
bodies[0].x = 1.5
print(bodies[0].x)      # 1.5
print(length(bodies))   # 1024

scores = [0; 100]        # a plain integer[] array, all 100 elements starting at 0
scores[3] = 42
```

Unlike the old `Type[count]` this replaced, the fill value isn't limited to a struct's own declared
defaults — `[Particle(1.0, 2.0, 5.0); n]` builds a packed array where every element starts with
those exact field values, not `Particle()`'s defaults.

**Struct eligibility is per-struct-type**, checked at construction (a VM runtime check, not a
parse-time one): every field must be `integer`, `float`, or `boolean` — a `string`, `array`,
`hashtable`, or unconstrained (`null`-defaulted) field all disqualify a struct from packed
construction (they can't be packed at a uniform byte width), even though that same struct works
fine as an ordinary, individually-constructed instance (`Type()`).

**Narrow numeric arrays**: an `i`- or `f`-suffixed literal *written directly as the fill value*
(`[0i; n]`, `[0.0f; n]`) selects 32-bit storage (`int32`/`float32`) instead of the default 64-bit
(`integer`/`float`) — the same suffix convention as [Narrow Struct Fields](#narrow-struct-fields).
This only works for a literal in that exact position — the suffix is parse-time-only information,
so `x = 0.0f; [x; n]` builds an ordinary (wide) `float[]` array, not a narrow one.

**Packed arrays: field access only, no standalone per-element reference.** `arr[i].field` (get and
set, including compound assignment) is the only supported form; a bare `arr[i]` alone is a compile
error (`Cannot index type`), and so is `for x in arr:` — a packed array can't be iterated directly
(index with an ordinary counting loop instead: `for i in 0..length(arr): ... arr[i].field ...`).
This isn't an arbitrary restriction: a packed element has no standalone value to hand back — its
"address" is pure arithmetic (`base + i * stride`), recomputed at each `.field` access, not a
pointer to save or a value to box. **Typed arrays have no such restriction** — an element is a
plain scalar, so `arr[i]` alone and `for x in arr:` both work exactly like an ordinary array's.

**Not yet supported for a packed or typed array specifically** (an ordinary struct array works fine
with all of these): `json.encode()` (errors — "cannot serialize a packed/typed array value"), and
crossing into or out of host code through `AerNativeFn` — the embedding API's `AerVal` boundary
doesn't yet handle either one on either side of that call.

`type()` reports a distinct name for each: `"Point[]"` for a packed array (not `"Point"`, so packed
and ordinary instances of the same struct are still distinguishable at runtime), and
`"integer[]"`/`"float[]"`/`"int32[]"`/`"float32[]"` for a typed array depending on its element kind.

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
`f` is always resolved through AER's ordinary flat function scope, exactly as if you'd written
`f(x, args)` yourself — the target itself is never dispatched by type. This is what gives AER a
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

**One exception:** `|>` checks whether `x` itself — the value on its left — is a `Result` (see
[Error Handling](#error-handling)). If it is, the call becomes conditional: a failed `Result`
(non-null `err`) skips the call entirely and the whole expression is just that same failed
`Result`, unchanged; a successful one is unwrapped to its `.value` before `f` is called. An
ordinary value is completely unaffected by this — the check only ever fires for a genuine
`Result`, so `5 |> increment(3)` above costs nothing extra and behaves exactly as shown. This is
what lets a chain of fallible native calls read like a single pipeline instead of a staircase of
`if err == null` guards:

```
# every stage after a failure is skipped, not just the one that failed --
# a missing file means json.decode() never runs at all, and err carries
# io.read()'s own message straight through.
value, err = io.read("config.json") |> json.decode()
if err != null:
    print(err)
else:
    print(value)
```

A pipe chain also works as a bare statement, its result discarded — useful when the target mutates
in place and the return value is redundant to capture:

```
b.neighbors |> collection.append(a)   # same as collection.append(b.neighbors, a) — mutates in place
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
returns its success value on its own (`return value`) or signals failure with `raise <reason>`; the
caller checks with a plain `if`. AER has no exceptions — no `try`/`catch`, no distant handler.

```
function safe_div(a, b):
    if b == 0:
        raise "division by zero"
    return a / b

result, err = safe_div(10, 0)
if err != null:
    print("Error: {err}")
```

A function's plain success path (`return value`) doesn't need to build anything special for this to
work — destructuring an ordinary value at the call site (`a, b = ...`) treats it as `(value, null)`
automatically, the same "not a real Result? just a plain value" rule the pipe operator (`|>`, below)
already applies on its own left operand. Every fallible *native* stdlib function (`io.read`,
`json.decode`, etc.) and every `raise` both return something stricter: a real `Result` type, distinct
from an ordinary array (`type(x)` reports `"Result"`, not `"array"`). Destructuring looks identical
either way — `value, err = io.read(path)` works whether the failure came from the stdlib or your own
`raise` — the difference only shows up if you try to misuse one: indexing anything but `0`/`1` on a
`Result` is a clear error rather than quietly doing whatever an out-of-bounds array access does, and
a `Result` can't be silently handed to something expecting a plain array. A bare `Result` is also
truthy exactly when it succeeded, so `if io.read(path): ...` reads as "if that worked" without
destructuring first. `==`/`!=` on a `Result` is reference equality, same as arrays/hashtables — two
separately-built Results with identical contents are not `==` to each other, only a `Result`
compared against itself (or a variable holding the same one) is.

**`raise <expr>`** is the one way user code signals a recoverable failure — a full, visible,
keyword-led statement, exactly like `return`/`break`/`continue`, never a symbol embedded in an
expression and never unwinding across more than the enclosing function's own return:

```
function parse_positive(n):
    if n <= 0:
        raise "must be positive"
    return n
```

`<expr>` is usually a string message, but can be any value — a struct instance works too, if you
want a caller to distinguish *kinds* of failure via `type(err)` rather than matching a message
string, with no separate exception-class feature needed (structs and `type()` already do this).
Under the hood, `raise` builds exactly what `Result(value, err)` does (below), with the value forced
to `null` — a plain `return` always means a success value and `raise` always means failure, never
mixed within the same function, so there's no shape ambiguity to check for.

**`Result(value, err)`** builds a genuine `Result` directly, for the rarer case of constructing or
forwarding one outside a `return`/`raise` statement (storing one in a variable or an array, say).
Capitalized like a struct constructor (`Basket(...)`, not `print(...)`) rather than a plain builtin —
deliberately, since a lowercase `result` is exactly the kind of name a script would otherwise pick
for an ordinary local variable, and this way it can't be silently shadowed by one. Exactly one of
the two arguments must be null; passing both (or neither) is a runtime error.

**Runtime errors** are a VM-level thing and are not values at all — out-of-bounds access, wrong
argument count, dividing by zero with `/`, a shape mismatch from `as Type`, and similar faults print
a message identifying the file, line, and enclosing function (if any), e.g.
`test.aer:2, in c(): Error: Division by zero` — and, if the fault happened inside a chain of nested
calls, one `called from line N, in fn()` line per enclosing call beneath it, innermost first. A
frame reached via tail-call optimization has no separate identity left to report (the whole point of
the optimization is that it doesn't keep one), so that's called out explicitly
(`(+N tail call(s) not shown)`) instead of silently presenting an incomplete trace as a complete one.
Then:

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
same abort behavior, same REPL-vs-file distinction, same unreachability from AER code. It's
deliberately a plain function rather than a keyword, unlike `raise` — the two are meant to look
different at a glance precisely because they mean different things: `raise` hands control back to
the caller, `panic()` ends things.

**`assert(condition, message)`** is a separate thing entirely — a check for tests and invariants,
not an error. A failed assertion prints `ASSERT FAILED: message` and the script **keeps running** —
it does not abort the current statement the way a runtime error does:

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

There are ten native modules — `math`, `random`, `string`, `time`, `json`, `collection`, `net`,
`regex`, `actor`, `scheduler` — not files on disk, but a hardcoded set the parser recognizes (see
[Standard Library](#standard-library) for the full function list; `io` is an eleventh stdlib
module, described there too, wired in slightly differently — see that section). `import math`
itself emits no
bytecode; it just records "math" as a known module name for the rest of the file (or REPL
session), consulted entirely at parse time. There's no `TYPE_MODULE` value, and a module name is
never a real scope variable, so `x = math` (assigning the module itself, rather than calling
something on it) is a parse error.

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

A quoted string form also exists, for paths the dotted form can't express (explicit relative
components, an absolute path) — used exactly as written, never dot-converted:

```
import "../shared/helpers.aer"          # binds as helpers, derived from the path's own last segment
import shared "../shared/helpers.aer"   # binds as shared instead — the alias, name-before-string,
                                         # matching Go's own import-alias convention
```

The alias form is also the way to bind a name the path can't derive on its own (an extensionless
path, or one whose last segment isn't a valid identifier).

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

**No OS-thread parallelism yet.** AER has no threads, `async`/`await`, or event loop — the VM is a
single, synchronous dispatch loop. **Multiple independent `VM`+`Chunk` pairs can coexist in one
process** (file-based `import` already relies on this — each imported file gets its own), and as of
this heap-independence work, each `VM` genuinely owns its own GC-managed heap (`VmHeap`,
`source/core/vm.h`/`vm.c`) — allocating, collecting, and freeing entirely on its own, with no shared
pools between VMs. That was the harder half of what real thread-safety would need, and it's done.
What's still missing is everything *around* it: the scheduler (`vm_run_slice`) is still cooperative
and single-threaded, and process-global state elsewhere (the error-unwind target, the currently-
active-VM-for-errors pointer, the lexer/parser's own file-static state) still assumes only one VM
ever dispatches at a time. Two VMs running on separate OS threads today would no longer race on the
*allocator*, but would still race on that remaining shared state — a real, but now much smaller,
remaining gap than a from-scratch rewrite.

**Cooperative, single-threaded concurrency exists at the language level** via the `actor` and
`scheduler` modules:

```
import actor
import scheduler

worker, err = actor.spawn("worker.aer")     # an independent, long-lived VM running worker.aer
scheduler.add(worker, "handle_request", 42)  # queue a call to worker's handle_request(42)
scheduler.run()                              # drives every added task to completion, round-robin

msg = actor.receive(worker)                  # drain worker's mailbox (actor.send() from either side)
result, call_err = actor.call(worker, "add", 3, 4)   # a direct, synchronous call outside the scheduler
```

`actor.spawn(path)` starts an independent VM (the same instantiation `import` uses internally, with
its own independent heap — see above). `actor.send`/`actor.receive` move plain strings through a host-side
mailbox — a value from one actor's pools is meaningless in another's, so message content (JSON,
typically) is entirely up to each side's own `json.encode()`/`decode()` calls; the mailbox itself
never moves a live value. `actor.call` runs a named function on an actor synchronously, to
completion, right away — no scheduler involved, useful for a one-off request/response.

`scheduler.add(handle, fn, args...)` queues a function call on an already-spawned actor;
`scheduler.run()` then round-robins every queued task in small instruction-count slices
(`source/core/vm.c`'s `vm_run_slice`) until each either finishes or errors, so a long-running task
can't starve a short one — interleaving comes from visiting every unfinished task once per round,
not from any actor knowing about the others. A task that errors is isolated (removed from the run
queue) exactly like a normal `actor.call` failure; a task that never finishes (a genuine infinite
loop) means `scheduler.run()` never returns, the same as any other infinite loop in AER already
behaves.

**What this is not:** the scheduler does not preempt a native call. An actor blocked inside
`net.recv()`/`net.accept()`, a slow `io.read()`, or a pathological regex still runs that call to completion before
the next scheduler slice can fire, because it's a C function call the instruction-budget check
can't see inside of. Cooperative scheduling interleaves AER-level work between actors; it does not
make any single blocking call itself non-blocking.

---

## Standard Library

There are currently ten native modules — `math`, `random`, `string`, `time`, `json`,
`collection`, `net`, `regex`, `actor`, and `scheduler` — plus `io`, wired in slightly differently
under the hood but just as unconditionally available (see [File I/O](#file-io--io) below). See
[Modularity](#modularity) for how `import` resolves these.

```
math.sqrt(x)          # square root, always returns a float; x must be non-negative
math.pow(x, y)        # x to the power of y, always returns a float; a negative x requires a whole-number y
math.floor(x)         # round toward negative infinity, returns an integer
math.ceil(x)          # round toward positive infinity, returns an integer
math.round(x)         # round to the nearest integer (halves away from zero), returns an integer
math.abs(x)           # absolute value, preserves integer/float
math.min(a, b)        # the smaller of two numbers, preserves whichever argument's type
math.max(a, b)        # the larger of two numbers, preserves whichever argument's type
math.sin(x), math.cos(x), math.tan(x)   # standard trig, x in radians
math.exp(x)           # e to the power of x
math.log(x)           # natural log, x must be positive
math.log2(x), math.log10(x)  # base-2 / base-10 log, x must be positive
math.pi()             # the constant, as a function — every native module exposes functions only
random.random()       # a float in [0, 1)
random.randint(a, b)  # an integer in [a, b], inclusive of both ends
random.seed(n)        # reseeds the RNG — makes subsequent random()/randint() calls reproducible
random.choice(arr)    # one element of a non-empty array, uniformly
random.shuffle(arr)   # permutes the array in place (Fisher-Yates) and returns it
string.upper(s)       # ASCII-only uppercase
string.lower(s)       # ASCII-only lowercase
string.trim(s)        # strips leading/trailing whitespace
string.contains(s, sub) # true if sub occurs anywhere in s
string.index_of(s, sub) # byte index of sub's first occurrence in s, or -1
string.starts_with(s, prefix), string.ends_with(s, suffix)
string.repeat(s, n)   # s repeated n times (n must be >= 0)
string.replace(s, old, new)  # every occurrence of old (non-empty) replaced with new
string.split(s, sep)  # splits on a non-empty separator, returns an array of strings
string.join(arr, sep) # joins an array of strings with sep, returns a string
time.now()            # current epoch time as a float, with sub-second precision
time.sleep(s)         # pauses for s seconds (integer or float, e.g. 0.25)
time.strftime(t, fmt) # formats an epoch time (e.g. from time.now()) using C strftime format codes,
                      # in local time — time.strftime(time.now(), "%Y-%m-%d %H:%M:%S")
time.parse(s, fmt)    # the strptime side of strftime — %Y %m %d %H %M %S %% only, hand-rolled
                      # (strptime itself isn't reliably present on the MinGW target); returns an
                      # epoch time as a float, or a runtime error if s doesn't match fmt exactly
json.encode(value)    # returns a JSON string
json.decode(s)        # returns (value, err) — err non-null on malformed input
```

`random` runs its own xoshiro256** generator, not C `rand()` — `random.seed(n)` reproduces the
identical sequence on every platform, `randint` is bias-free (rejection sampling), and the full
range of any span is reachable. (C `rand()` was dropped after `RAND_MAX` turned out to be 32767 on
the MinGW target, which silently capped `randint` at the bottom 32,768 values of a wide range.)

`time.now()` is wall-clock (`clock_gettime(CLOCK_REALTIME, ...)`), not monotonic — a system clock
adjustment (NTP sync, manual change) could in principle make two successive calls disagree about
ordering. Good enough for logging and for measuring durations in ordinary scripts; not a substitute
for a monotonic clock in code that must be robust to clock adjustments mid-run.

### Collections — `collection`

Everything that grows, shrinks, reorders, or duplicates an array or hashtable lives here — one module,
rather than a few blessed global builtins, so the global namespace stays tiny and every collection
operation is spelled the same way.

```
import collection

collection.append(arr, x)     # adds x at the end; mutates in place, returns arr (redundant to capture)
collection.delete(x, key)     # removes index key from an array (negative ok) or key from a hashtable; mutates in place
collection.insert(arr, i, x)  # places x at index i, shifting the rest up; i == length(arr) appends
collection.index_of(arr, x)   # index of the first element equal to x, or -1
collection.copy(x)            # a new array/hashtable with the same entries — a shallow copy, one level deep
collection.keys(d)            # a hashtable's keys as a new array (unspecified order — sort it for determinism)
collection.sort(arr)          # sorts in place (ascending) and returns the array
```

`collection.sort(arr)` requires every element to be a number (compared numerically, integer and
float mix freely) or every element to be a string (compared lexicographically) — mixing the two is
rejected rather than falling back to some arbitrary tie-break.

`collection.copy` is shallow: the new container has the same *values*, so nested arrays/hashtables are
still shared references. Struct instances are excluded — construct a fresh one instead.

### JSON — `json`

```
data = {"name": "AER", "version": 2, "tags": ["scripting", "small"]}
s = json.encode(data)
print(s)    # a JSON object with all three keys — order follows hashtable iteration order
            # (unspecified, not insertion order — see Hashtables)

decoded, err = json.decode(s)
if err == null:
    print(decoded["name"])    # AER
```

`json.encode` maps null/boolean/integer/float/string/array/hashtable onto their obvious JSON
counterparts. A struct instance encodes as a JSON *object* keyed by its field names (not a bare
positional array), so the field names survive — but since JSON itself has no struct types, decoding
always produces a plain hashtable back, never the original struct. Encoding a function value is a
runtime error — there's nothing to serialize. `json.decode` follows the same `(value, err)`
convention as the rest of the fallible stdlib rather than aborting the script on malformed input.

### Networking — `net`

Blocking TCP only — connect/send/recv/close plus listen/accept, the common "talk to a server" and
"be a server" cases. No HTTP/TLS layer; deliberately scoped small rather than half-implementing a
much bigger surface.

```
import net

handle, err = net.connect(host, port)   # a connection handle, or an error string
if err == null:
    sent, err2 = net.send(handle, "hello\n")
    reply, err3 = net.recv(handle, 4096)  # up to 4096 bytes; "" means the peer closed the connection
    net.close(handle)
```

```
listen_handle, err = net.listen(port)     # binds + listens on every local IPv4 interface
if err == null:
    conn, aerr = net.accept(listen_handle)  # blocks (bounded, see below) for the next incoming connection
    if aerr == null:
        # conn works with send()/recv()/close() exactly like a connect()-returned handle
        net.close(conn)
    net.close(listen_handle)
```

Every call is blocking, matching AER's single-threaded execution model — no async I/O, no event
loop. `connect()`, `send()`, `recv()`, and `accept()` all share the same 10-second timeout: a plain
blocking call has no bound of its own and can hang far longer than a normal refusal/EOF against a
peer that's merely slow or silent (an unreachable address for `connect()`, a connection that's open
but never sends for `recv()`, a full receive buffer on the other end for `send()`, no client ever
connecting for `accept()`) — past 10 seconds each reports the same kind of `Result` error every
other AER fault does, rather than hanging the whole script (and, for `accept()` specifically,
rather than freezing every other actor `scheduler.run()` is trying to interleave). A script standing
up a long-lived server calls `accept()` in a loop and treats a timeout as "no client yet, try
again," not a fatal error.

A connection or listening handle is a plain integer, but never a raw OS socket cast through one —
both are resolved through the same small internal registry, so a wrong or stale handle (a typo,
reusing one after `close()`) is a clean, reported error, never an operation on whatever OS handle
that integer happens to collide with.

### Regular Expressions — `regex`

A small backtracking engine covering the practical common subset: literals, `.`, character classes
(`[abc]`, `[^a-z]`, `\d \w \s \D \W \S`), `*`/`+`/`?` quantifiers (on single atoms and on groups),
`^`/`$` anchors, `|` alternation, and `(...)` grouping (non-capturing — none of the three functions
below need to extract a sub-match, only the overall match). No backreferences, no named groups, no
lazy quantifiers. Rolled in-house rather than depending on a system library — POSIX `<regex.h>`
isn't reliably available on the MinGW target, and this matches the project's existing precedent
(its own PRNG instead of libc `rand()`, its own GC).

```
import regex

regex.match(s, pattern)          # true/false — does pattern occur anywhere in s
regex.find(s, pattern)           # the first matching substring, or null
regex.find_all(s, pattern)       # every non-overlapping matching substring, as an array (empty, not null, if none)
regex.replace(s, pattern, repl)  # every non-overlapping match replaced with repl (a literal string, no backreferences)
```

Nested unbounded quantifiers (`(a*)*b` against a long non-matching run of `a`s — the textbook
catastrophic-backtracking shape) are exponential in any naive backtracker, this one included. A
step budget bounds worst-case time instead of letting a pathological pattern hang a script: past
it, a match attempt just reports failure early rather than exhaustively searching every partition.

### Actors and Scheduling — `actor` / `scheduler`

See [Concurrency](#concurrency) for the full explanation of what these do and don't provide
(cooperative, single-threaded interleaving — not parallelism, not preemption of a blocking call).

```
import actor
import scheduler

worker, err = actor.spawn("worker.aer")           # an independent, long-lived VM
actor.send(worker, "a message")                   # host-side mailbox, plain strings only
msg = actor.receive(worker)                       # null if nothing's queued, never blocks

result, call_err = actor.call(worker, "fn", 1, 2)  # synchronous, runs to completion right away

scheduler.add(worker, "fn", 1, 2)   # queue a call instead of running it immediately
scheduler.run()                    # drives every queued task, round-robin, until all finish
```

`actor.spawn` fails (a `Result` error) if the path doesn't compile or errors at its own top level.
An actor handle is a plain integer, never a raw pointer — passing back a wrong or stale handle is a
normal recoverable error, not a crash. `actor.call`/`scheduler`'s queued tasks isolate a runtime
error to that one call: an actor that errors is still usable afterward, and one task's failure
under `scheduler.run()` doesn't stop the others from running.

### File I/O — `io`

`io` is registered slightly differently under the hood than the other native modules above — it
goes through the same generic host-function mechanism a real embedding host would use for its own
custom functions (`aer_register_function`, see [Embedding](#embedding)) rather than a fixed
dispatch-table entry — but `vm_init()` calls `aer_io_register()` itself now, once per process, so
every host gets it automatically with no action of its own. It's just as unconditionally available
as `math`/`collection`/`net`/everything else; the only thing left genuinely host-specific is a
host's own *custom* functions (a game's `spawn_enemy()`, say) via that same mechanism.

```
write_err, err = io.append("log.txt", "a line\n")
if err != null:
    print("could not write: " + err)
```

```
io.read(path)     # opens, reads the whole file, and closes it in one call — returns (contents, err)
io.write(path, s) # opens (truncating), writes s, and closes it — returns (null, err)
io.append(path, s) # same as io.write(), but opens in append mode instead of truncating
io.exists(path)   # plain boolean — "no" is an answer here, not an error, so no Result
io.remove(path)   # deletes the file — returns (null, err)
io.stdin()        # returns a handle for piped input (always succeeds) — pass it to io.read() instead of a path
io.args()         # the script's own command-line arguments (everything after the script path), as an array of strings
io.basename(path) # the substring after the last '/' or '\' (the whole string if neither is present)
io.dirname(path)  # the substring before the last '/' or '\' ('.' if neither is present, POSIX-style)
io.join(a, b)     # a + b with exactly one '/' inserted between them — no filesystem access, pure string logic
```

Every `io` function follows the `(value, err)` convention `safe_div` establishes at the user level
(see [Error Handling](#error-handling)) — except the value it returns on both sides is a real
`Result`, not a plain array (more on that distinction there). A wrong argument *type* (not a string
path, not `io.stdin()`'s handle) is a VM-level runtime error like any other stdlib type mismatch; a
missing file or a write that fails partway is the fallible case and comes back as `err` instead.
There's no path sandboxing within `io` at all — same trust model as any language's file API. Since
`io` is unconditionally available now (not a host opt-in), running untrusted AER source is not a
safe sandbox on its own — that would need a real permission system, a different feature from
anything described here.

`io.read()` takes either a path (opens, reads the whole file, closes it) or `io.stdin()`'s handle —
piped, non-seekable input falls back to reading until EOF instead of the seek-and-presize approach
a regular file uses, but the call site is identical either way. There's no `io.open`/`io.close`:
every file operation here is one-shot, so there's no handle for user code to leak or use with the
wrong mode.

---

## Embedding

AER is meant to be linkable directly into a host C application, not just run as a standalone
CLI — the [Practical Applications](#practical-applications) most worth taking seriously
(game logic, an embedded scripting DSL) require it. A host links everything under `source/`
except `source/main.c` (which has its own `main()`), plus `include/aer.h`.

**A runtime error does not terminate the host process.** A scripting bug (wrong argument count,
division by zero, an out-of-bounds index) fails only the script call that triggered it — `vm_run()`
returns cleanly on error, in every mode. `aer_run_source()` (`include/aer.h`) is the whole thing in
one call — it wraps the lex/parse/emit-HALT/run sequence so a host never needs to assemble it by
hand:

```c
#include "aer.h"
#include "vm.h"

Chunk chunk;
VM    vm;
chunk_init(&chunk);
vm_init(&vm, &chunk);

bool ok = aer_run_source(&vm, &chunk, "print(1 / 0)\n");  /* false — but the process is still running */
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
frame, for example) is what `aer_run_source()` is for: it calls `aer_vm_reset_for_reuse(vm)`
itself before every run, resetting stack/call-frame state in case the *previous* call errored
mid-execution (an aborted call leaves scopes pushed that normal execution would have unwound). A
host driving `vm_run()`/`vm_run_slice()` directly on its own `VM*` — bypassing `aer_run_source()`
entirely, e.g. to run already-parsed bytecode — needs to call `aer_vm_reset_for_reuse()` itself for
the same reason; see `tests/embed_smoke_test.c` for both used in practice.

**Not every embedding scenario fits `aer_run_source()`.** It always feeds fresh source text
through the lexer (`shell()` internally) and appends after whatever's already in `chunk` — the
right shape for "run this script," "eval this REPL line," or "call this game hook." It is not the
right primitive for driving an already-parsed, already-running `VM*` directly (a scheduler resuming
a suspended actor, for instance) — that case calls `vm_run()`/`vm_run_slice()` on its own, the way
`source/core/aer_scheduler.c` does.

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

static AerVal host_add(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count == 2 && aer_type(args[0]) == TYPE_INTEGER && aer_type(args[1]) == TYPE_INTEGER)
        return aer_int(aer_as_int(args[0]) + aer_as_int(args[1]));
    return aer_int(0);
}

aer_register_function("game", "add", host_add, NULL);   /* once, after vm_init() */

aer_run_source(&vm, &chunk, "import game\nprint(game.add(3, 4))\n");   /* -> 7 */
```

`args` is a plain `AerVal` array built fresh for this one call — read it via `aer_type()`/
`aer_as_int()` and friends (`value.h`), never raw field access, and don't hold onto the pointer
past the call. Return the result `AerVal` directly (`aer_int()`/`aer_real()`/`aer_bool()`/etc.); to
report an error, call `error()` (`source/utilities/error.h`), the same recoverable path every other
AER error goes through. Registration is process-global, like the error-reporting state above, and
must happen before any script that references the module is parsed. See `tests/embed_smoke_test.c`
for this exercised end-to-end, and `source/core/aer_host.h` for the full registry API.

Build and run the embedding smoke test (a minimal, complete example of everything above,
including the deliberate-error and VM-reuse cases) with `make test-embed`.

**`aer_io_register()`** (`source/stdlib/aer_io.h`) is the same registration mechanism applied to an
AER-provided (not host-defined) capability — see [File I/O](#file-io--io). Unlike a host's own
custom functions, `vm_init()` calls this one itself, once per process, so every host's scripts get
file access automatically — there's nothing for an embedding host to opt into or out of here.

**Coarse capability toggles** — `aer_set_io_enabled()`, `aer_set_net_enabled()`,
`aer_set_import_enabled()` (`include/aer.h`) — let a host (or the CLI, via `--no-io`/`--no-net`/
`--no-import`) disable a whole capability for every script that runs afterward. All three default
`true` (unchanged from every prior release). This is a blast-radius limiter, not a real permission
system: whole capability on/off, no path/host allowlisting, one process-wide flag rather than
per-`VM` granularity (`--no-import` specifically blocks only file-based `import`; fixed modules
like `math`/`net`/`regex` are dispatch, not a file read, and are unaffected). Disabling a capability
makes the corresponding operation fail with a normal, recoverable error — the same non-fatal path
every other AER fault takes, not a crash. See [Security concerns](#security-concerns) below for
what a real permission system would still need on top of this.

---

## Pitfalls and Limitations

### Behaviours that may surprise you

| Behaviour | What happens | Workaround |
|-----------|-------------|------------|
| `/` always returns float | `1 / 1` → `1.0` | Use `//` for integer floor division |
| Arrays and hashtables are references | `b = a; b[0] = 99` modifies `a` too | `b = collection.copy(a)` when you really want a distinct container (shallow — one level) |
| Referencing a name that was never assigned is a compile error | `print(x)` with no prior `x = ...` anywhere fails to compile | Assign it first (`x = null` if there's genuinely nothing better) |
| Missing hashtable key returns `null` | No error, silent | Use `key in hashtable` before access |
| `collection.append()`/`delete()`/`sort()`/`shuffle()` mutate in place and also return the container | `arr = collection.append(arr, v)` works but is redundant — the mutation already happened | Call them as statements |
| Repeated `s += x` in a loop is quadratic | Strings are immutable — every `+=` allocates a fresh buffer and copies the whole thing so far, not just the addition | Build a list with `collection.append()` and join once: `parts = []; for ...: collection.append(parts, x); s = string.join(parts, "")` |
| No struct-to-struct conversion | There's no built-in way to reshape a hashtable or another struct into a Point | Build the struct explicitly: `Point(some_table["x"], ...)` |
| Struct instances are still `AerArray` under the hood | `length(p)` works and returns the field count (not blocked) | Harmless but not the intended API — use dot access |
| Pipe rejects nested calls in target args | `x \|> f(g(1))` is a parse error, at any depth | Assign the inner call to a variable first: `t = g(1); x \|> f(t)` |

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
| Hashtable key length | 4 096 bytes |

### Missing features

- A generational garbage collector reclaims `AerString`/`AerArray`/`AerDict`/`AerFunction`
  (see [Memory and Security](#memory-and-security)), but struct-type registrations still grow
  monotonically for the life of the process — deliberately, since that's what makes REPL
  function/struct persistence work. The file-module registry grows the same way during normal
  execution, but an embedding host can explicitly reclaim it via `aer_module_free_all()` (see
  [Embedding](#embedding)) when tearing the process down.
- `net` is a minimal blocking TCP client only — no listen/accept (no way to *be* a server), no
  HTTP/TLS layer. `regex` covers a practical common subset (see [Regular
  Expressions](#regular-expressions--regex)), not the full PCRE feature set.
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
- No true parallelism — the `actor`/`scheduler` modules give cooperative, single-threaded
  interleaving only, and can't preempt a blocking native call — see [Concurrency](#concurrency).

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
[Operators](#operators)) handles all binary operators — including `in` and the pipe operator `|>`
(which special-cases its RHS parsing since it isn't a general expression) — without a chain of
separate grammar rules. Primitive casts and shape-checks aren't part of this table at all — they're
ordinary calls (`integer(x)`) and comparisons (`type(x) == "Point"`), ordinary primary/call
expressions rather than a dedicated binary operator.

String interpolation is resolved at compile time: each `{expr}` is sub-parsed via its own
independent lexer span (saved/restored around the outer string's own raw byte scan) into ordinary
expression bytecode, then lowered into a chain of `OP_TO_STR` / `OP_ADD` instructions alongside the
literal segments — no runtime parsing, and no different from typing that expression anywhere else.

For-each loops compile to register-based iterator opcodes (`OP_ITER_NEXT_ARRAY`/`OP_ITER_NEXT_PAIR`
for arrays/hashtables/strings, `OP_ITER_RANGE_PREP`/`OP_ITER_RANGE_LOOP` for `a..b..step` ranges) whose
state lives in a couple of registers the loop already owns — no heap allocation per iteration, and
the range form is loop-rotated (PREP once before the loop, LOOP at the bottom) to match Lua's own
FORLOOP shape rather than paying a separate top-of-loop check plus an unconditional back-edge jump.

`break` and `continue` are compiled with jumps patched directly to the loop's own exit/continue
targets, computed at compile time — there's no separate runtime scope or iterator stack to unwind,
since a loop's iteration state is just ordinary registers.

Struct **names** are tracked in a parse-time table (`struct_names`) as soon as a `struct` statement
is parsed, which is what lets `Type()` be recognized (as struct construction, not an ordinary
function call) immediately after — but the struct's actual shape (field names, types, defaults) is
only registered into the chunk's runtime shape registry when `OP_DEFINE_STRUCT` actually executes,
exactly like a function name is knowable before its body has run. A repeat-literal (`[Type(); n]`)
needs no equivalent parse-time recognition of its own — it's just an ordinary call inside ordinary
`[...]` grammar, with the struct-vs-number branch resolved at runtime (see
[Repeat-Literal Arrays](#repeat-literal-arrays)) once `value` has actually been evaluated.

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

**Packed arrays** (`[Type(); count]`, see [Repeat-Literal Arrays](#repeat-literal-arrays)) are a
separate value type, `AerPackedArray` — one raw byte block holding `count` instances of one struct
type's fields at a fixed per-field 8-byte slot, addressed by pure arithmetic
(`base + i * stride + field_offset`) instead of `count` individually heap-allocated, pointer-linked
instances. Every field is a fixed primitive (never a heap reference), so a packed array is a leaf
for the garbage collector — its own mark step never recurses into its contents. **Typed arrays**
(`[0; count]`/`[0i; count]`, the numeric half of the same repeat-literal) are `AerTypedArray` — the
same "one raw byte block, no per-element boxing" idea, but with no `Shape` at all: just one fixed
element kind (`int32`/`float32`/`integer`/`float`) for the whole array, since there are no named
fields to lay out.

**Collections:** `AerArray` and `AerDict` are heap-allocated structs held by pointer inside an
`AerVal`. Assignment copies the pointer — all aliases share the same data. Arrays grow with doubling
reallocation; hashtables use `source/utilities/hashtable.c`'s open-addressed hash table (FNV-1a, linear
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

FNV-1a hash, split into a small sparse array of open-addressed probe indices (linear probing,
growing at 70% load) pointing into a dense array of the actual key/hash/payload entries, packed in
insertion order with no holes. Growing the table only ever reallocates and repopulates the sparse
array — the dense array's entries are never moved by a rehash, only appended to, which keeps the
randomly-probed structure small (4 bytes/slot) even at hundreds of thousands of entries. Deletion
uses the reinsert technique to repair the sparse array's probe chain, with no tombstones, exactly
as before; the dense array separately stays hole-free by swapping the last entry into the vacated
slot. This also means iteration order is no longer hash-bucket order — it's dense-array order,
locally perturbed by swap-compaction on removal, still unspecified from a script's perspective
either way. Backs every `AerDict` and, less visibly, `Chunk.name_index` (the pool's own
string-constant dedup table) — both consumers share the same size-classed slab pools for small key
buffers and the sparse array, falling back to plain malloc only past the sparse array's largest
size class. The dense array is a separate, plain xrealloc-doubling allocation, not pooled at all —
its access pattern is sequential append/scan, not the random-access pattern pooling helps with.

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
harmless null back instead of an arbitrary tag. `AerVal` is also the one value type at the
embedding boundary (`AerNativeFn`, `include/aer.h`) — no separate public/internal representation to
keep in sync, and no sentinel magic needed either way.

### Backpatching

Jump targets are unknown at emit time (the body hasn't been compiled yet). AER emits `0` as a
placeholder, then overwrites it once the target address is known. Every `if`, `for`, function body,
and short-circuit operator uses this pattern.

### Precedence climbing

One function with a table handles all binary operators (see [Operators](#operators)). To add a new
operator, add one row to the table — `in` and `|>` both went in this way, `|>` needing a small
special-case branch in the climbing loop because its right-hand side isn't a general expression.

### Iterator state lives in ordinary registers

`for x in arr:` keeps its collection reference and index in two registers the loop already owns,
advanced in place by `OP_ITER_NEXT_ARRAY`/`OP_ITER_NEXT_PAIR` each iteration — no heap allocation,
and no separate iterator stack to unwind on `break`, since `break`/`continue` compile straight to a
jump. The integer-range form (`a..b..step`) goes further: `OP_ITER_RANGE_PREP` computes a total
iteration count once before the loop, and `OP_ITER_RANGE_LOOP` counts down at the *bottom* of the
loop body, matching Lua's own FORLOOP shape — no unconditional back-edge jump instruction is ever
emitted for this loop form at all.

### Short-circuit `and`/`or` return the deciding operand

`and`/`or` return whichever operand's own value decided the result (Python/Lua semantics), not a
coerced `true`/`false` — `x = x or "default"` works exactly like it does in Python. `vm_truthy`
(the same general truthy-coercion `if`/`while` conditions already use for every type) decides which
operand wins; the codegen (`compile_and`/`compile_or`, parser.c) just leaves that operand's own
register in place instead of loading a fresh boolean constant.

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
calls still works — just explicitly, by mutating a struct/array/hashtable you were actually passed as a
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

### No dedicated cast/shape-check operator, by design

An earlier design used a single `as` operator for both jobs (`x as integer` to coerce, `x as Point`
to verify a shape). It was removed: `as` was the only keyword whose entire purpose was being a cast
target, and an optional convenience like this has to earn its place in the grammar rather than just
being "a fine word" for the job. Primitive casts are ordinary function calls (`integer(x)`), and
shape-checking reuses the `type()` builtin that already existed for other reasons
(`type(x) == "Point"`) — zero new grammar, and a strictly more general shape check in the bargain
(a query that returns a boolean, rather than an operator that could only ever assert-or-crash).

### Pipe enforces no nested calls in its target's arguments

`x |> f(g(1))` is rejected, at any nesting depth, including a nested pipe's own call. This keeps
a pipe chain a flat, linear sequence you can read top-to-bottom, rather than a spot where calls can
hide inside calls the way they can in ordinary nested function-call syntax.

### REPL runtime errors abort the statement, not the process

A runtime error (out-of-bounds, wrong type, etc.) in the REPL stops the rest of the current
statement from executing and returns control to the prompt — it does not continue running with a
placeholder value, and it does not exit the process. File execution still exits immediately on any
error, unchanged.

### Hashtable keys are always strings

No mixed-type key lookups, no hash collision between integer `1` and string `"1"`. Keeps the
hashmap implementation simple and the semantics predictable.

### REPL chunk monotonic growth

The bytecode chunk never resets across REPL calls — new bytecode is appended. Function bodies and
struct declarations compiled in earlier calls remain valid. Each REPL call starts the VM at the new
code's start address. This is why functions and structs defined interactively persist across calls.

### Modules resolved entirely at parse time, no runtime module value

Neither a native module (`math`) nor a file-based one (`helpers`) is ever a real scope variable or
runtime `AerVal` — `math.sqrt(x)` and `helpers.double(x)` are recognized and compiled directly by
the parser into a dedicated call opcode the moment it sees a name it already knows was `import`ed.
This keeps the `AerVal` union exactly as lean as it was before modules existed — no `TYPE_MODULE`
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

AER runs a **generational mark-and-sweep collector** over seven pooled heap types (`AerString`,
`AerArray`, `AerDict`, `AerFunction`, struct instances in their own pool separate from ordinary
arrays, `AerPackedArray`, and `AerResult` — `source/utilities/pool.c`). Every pool cell carries one byte of
state: a mark bit (this collection cycle only), a generation bit (young/old — promoted the first
time a cell survives any collection), a free-list bit, and a remembered-set bit (see below).
Allocation is unchanged from the pooled/slab design (a free-list pop, or a bump into the current
slab) — collection is what's new.

**Two collection modes, each VM's own independent heap.** A *minor* collection traces the normal
roots (the VM stack and every live call frame's registers) plus a *remembered set* — old
objects a write barrier caught being mutated to hold a young reference — and only sweeps young
cells; old cells are presumed live and left untouched, which is what keeps minor collections cheap.
A *major* collection (run periodically, after a fixed number of minor ones) traces the same roots
with no remembered set needed and sweeps both generations. The seven pools live on a `VmHeap`
embedded in each `VM` (`source/core/vm.h`), not a process-global — the main VM, every file-module's
own VM, and every actor's own VM (`import`/`actor.spawn` both run their target in a fully separate
`Chunk`+`VM` — see [Modularity](#modularity)) each collect only their own heap. A collection
triggered by one VM's allocation pressure never marks or sweeps any other VM's cells.

**The write barrier** — the mechanism that makes minor collections safe — only has two real call
sites: array item writes (index-assignment, `append`, struct field assignment) and hashtable entry
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
alone. `aer_gc_set_ceiling(max_live_cells)` caps total live cells across all seven pools — a
`-Xmx`-style limit, `0` (the default) meaning unlimited. Hitting the ceiling doesn't crash the host:
the collector forces one extra major pass first (in case a cheap collection alone would've freed
enough), and only if the script is *still* over the limit does it abort with a normal, recoverable
runtime error (`aer_last_error()`) — the same non-fatal path every other runtime fault already takes.

### Security concerns

| Issue | Risk | Status |
|-------|------|--------|
| Out-of-memory is still fatal | `aer_report_fatal()` calls `exit(1)` — the one remaining unconditional process exit | A configurable memory ceiling (rejecting an allocation, or forcing a collection first) is a natural next step now that the collector exists, but isn't built |
| No arithmetic overflow checks | `arr[9999999999999]` on a 32-bit platform behaves unexpectedly | Array bounds are checked; index arithmetic is not |
| `io` has no path restriction within itself | Every AER script gets real file access (see [File I/O](#file-io--io)) | A host can disable it entirely with `aer_set_io_enabled(false)`/`--no-io` (see [Embedding](#embedding)), but there's no path allowlisting *within* `io` — it's all-or-nothing, not a real permission system |
| File-based `import` reads arbitrary files by name | `import` resolves and executes `<name>.aer` (or a file found via `AER_PATH`) from disk | `aer_set_import_enabled(false)`/`--no-import` disables file-based import entirely (fixed stdlib modules are unaffected); same-directory/`AER_PATH` resolution limits the blast radius further when it's left on |
| `net` makes outbound TCP connections and accepts inbound ones, with no restriction | Any script can `net.connect()` to any host/port reachable from the process, or `net.listen()`/`net.accept()` to receive connections from anywhere reachable to it | `aer_set_net_enabled(false)`/`--no-net` disables the whole module, inbound and outbound alike — deliberately one flag, one meaning, rather than a separate toggle for listening; no host/port allowlisting, and no way to permit outbound while refusing inbound, within `net` itself |

The three toggles above (`io`/`net`/file-based `import`) are a blast-radius limiter, not a real
permission system — whole capability on/off, process-wide, no allowlisting of specific paths/hosts,
no per-`VM` granularity. Running genuinely untrusted AER source still needs more than this — a real
permission system would need per-`VM` capability sets and, for `io`/`import` specifically, path
allowlisting, neither of which exist today.

Hashtable key lookups (`in`, `delete`, index get/set) build a null-terminated copy of the key into a
fixed `VM_KEY_MAX` (4096 byte) stack buffer rather than a length-sized VLA, so an oversized key
raises a normal AER runtime error instead of risking a stack overflow.

---

## File Map

| File | Purpose |
|------|---------|
| `source/main.c` | Entry point, REPL loop, file runner |
| `source/terminal.h/c` | Raw-mode interactive REPL terminal |
| `source/value.h` | `AerVal` — the one tagged-union value type, used internally and at the embedding boundary alike: null / boolean / integer / float / string / function / array / hashtable / packed array, and the `Shape` forward declaration |
| `source/compiler/lexer.h/c` | Source text → token stream, indent/dedent tracking |
| `source/compiler/parser.h/c` | Single-pass compiler: tokens → register-based bytecode, escape processing |
| `source/core/vm.h/c` | Bytecode chunk, register-based VM (`CallFrame`/bump-pointer register stack), struct-type registry, computed-goto dispatch loop, built-ins |
| `source/stdlib/aer_stdlib.h` | Declares the entire native-module surface (math/random/string/time/json/collection/net/regex/actor/scheduler/io) — one header for a fixed, closed set |
| `source/stdlib/aer_stdlib.c` | `aer_stdlib_is_native_module()` — the hardcoded module names `import` accepts |
| `source/stdlib/aer_math.c` / `aer_random.c` / `aer_string.c` / `aer_time.c` / `aer_collection.c` / `aer_net.c` / `aer_regex.c` | One file per hardcoded native module, dispatched by `vm.c`'s `OP_CALL_MODULE` switch |
| `source/stdlib/aer_json.c` | `json` module — encode/decode, dispatched the same way as the modules above |
| `source/stdlib/aer_actor_module.c` / `aer_scheduler_module.c` | Script-facing `actor`/`scheduler` modules — thin shims translating `AerVal` args to `aer_actor.c`/`aer_scheduler.c`'s own C signatures |
| `source/stdlib/aer_io.c` | `io` module (file open/read/write/close) — registered via the generic host-function mechanism (`aer_register_function`), but called by `vm_init()` itself, so it's just as unconditionally available as the hardcoded modules above |
| `source/core/aer_module.h/c` | File-based `import` — resolution, isolated per-file `Chunk`/`VM`, cross-VM call trampoline; `aer_vm_instantiate_from_file()` is the shared "spin up an independent VM+Chunk and run its top-level code once" primitive this and `aer_actor.c` both use |
| `source/core/aer_actor.h/c` | Independent long-lived VMs plus a host-side byte-string mailbox — spawn/call/send/receive, reachable from AER scripts via the `actor` module — see [Concurrency](#concurrency) |
| `source/core/aer_scheduler.h/c` | Cooperative round-robin scheduler over already-spawned actors, driving each queued task in bounded `vm_run_slice()` instruction budgets instead of to completion — see [Concurrency](#concurrency) |
| `source/core/aer_host.h/c` | Host-registered native function registry (`aer_register_function`) — reached from AER the same way as `math`/`random`/`string` |
| `source/utilities/hashtable.h/c` | FNV-1a open-addressing hash table backing every `AerDict` and `Chunk`'s own string-constant dedup table, with size-classed slab pools for small key/bucket allocations |
| `source/utilities/pool.h/c` | Slab (bump/arena) allocator extended for the generational mark-sweep garbage collector — every pool-managed struct (`AerString`/`AerArray`/`AerDict`/`AerFunction`/`AerPackedArray`) carries its own one-byte GC state as its literal first field |
| `source/utilities/error.h/c` | Error reporting with source location and column pointer; recoverable-error sink (callback or stderr), `aer_report_fatal` for genuinely unrecoverable conditions, `assert_failure_count` |
| `include/aer.h` | Public embedding API: version constant, error callback/query functions, custom native-function registration (see [Embedding](#embedding)) |
| `tests/test_*.aer` | Runnable documentation and regression suites — `assert()`-based, exits nonzero on any failure. Run with `make test`. |
| `examples/` | Standalone, runnable programs meant to be read — see [Practical Applications](#practical-applications) for the full list |
| `ARCHITECTURE.md` | Internals-facing reference — value representation, allocator/GC, the register VM, the compiler, and the optimizations layered on top. The [Architecture](#architecture) section here is the summary; that document is the full story. |
| `tests/embed_smoke_test.c` | Minimal standalone embedding host — proves a runtime error doesn't kill the process, demonstrates the VM-reuse-after-error contract, and registers/calls a custom host function. Build/run with `make test-embed`. |
| `tests/smoke_test.c` | Register-VM unit test — hand-built bytecode plus real-source coverage below the level of a full `.aer` file. Build/run with `make test-smoke`. |
| `tests/fuzz.py` | Mutation-based fuzzer against an ASAN build — reports crashes and hangs. Run with `make fuzz`. |
| `.github/workflows/ci.yml` | CI: Ubuntu (`make test`, `make test-embed`, `make fuzz`) and Windows/MSYS2 (`make test`, `make test-embed`, `make test-smoke`) |
| `LICENSE` | MIT |

---

## License

MIT — see [`LICENSE`](LICENSE).
