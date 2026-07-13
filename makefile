# Windows/MSYS2 needs a .exe suffix, kernel32 (terminal.c's Win32 console API), and
# -static (otherwise the exe depends on MSYS2's libwinpthread-1.dll and silently fails
# to launch outside the exact shell it was built in). Detected via `uname -s`, not
# $(OS) — that env var doesn't reliably survive into an MSYS2 login shell.
# Match "_NT" rather than "MINGW": a bash launched without MSYSTEM=MINGW64 set (e.g.
# a plain Git Bash / VS Code terminal session) reports "MSYS_NT-..." instead of
# "MINGW64_NT-...", which "MINGW" alone misses — silently skipping this whole branch
# and producing an unstatic, un-kernel32-linked .exe that looks like a normal build.
ifneq (,$(findstring _NT,$(shell uname -s 2>/dev/null)))
    EXE     := .exe
    WINLIBS := -lkernel32 -static
else
    EXE     :=
    WINLIBS :=
endif

FLAGS := -O2 -g -Wall -Wextra -I include -I source -I source/compiler -I source/core -I source/utilities

SOURCE := $(wildcard source/*.c source/compiler/*.c source/core/*.c source/utilities/*.c)
OBJECT := $(patsubst source/%.c,object/%.o,$(SOURCE))

# Everything except main.c — conflicts with test-embed's/test-smoke's own main() below.
LIBOBJECT := $(filter-out object/main.o,$(OBJECT))

all: $(OBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -o binary/aer$(EXE) $^ -lm $(WINLIBS)

object/%.o: source/%.c
	@mkdir -p $(dir $@)
	gcc $(FLAGS) -c $< -o $@

# No -g — smaller binary, no debug symbols. Use `all` (the default) for anything that
# might need gdb or a readable ASAN/fuzzer backtrace; this is for a release artifact only.
PERFORMANCE_FLAGS := -O2 -Wall -Wextra -I include -I source -I source/compiler -I source/core -I source/utilities

performance: $(SOURCE)
	@mkdir -p binary
	gcc $(PERFORMANCE_FLAGS) -o binary/aer-performance$(EXE) $(SOURCE) -lm $(WINLIBS)

# Disassembler + opcode/memory profiling (source/core/disasm.c, AER_DEBUG_TOOLS-gated code in
# vm.c/vm.h) — entirely absent from every other target, including `all`. Run with AER_DISASSEMBLE
# set (a path, or "-" for stderr) to dump a disassembly + hit-count summary + memory report after
# the script runs; unset behaves exactly like a normal build.
debug-tools: $(SOURCE)
	@mkdir -p binary
	gcc $(FLAGS) -DAER_DEBUG_TOOLS -o binary/aer-debug$(EXE) $(SOURCE) -lm $(WINLIBS)

# Split across focused files rather than one monolith — run all in sequence, stop at the first failure.
TESTS := tests/test_core.aer \
         tests/test_collections.aer \
         tests/test_functions.aer \
         tests/test_structs.aer \
         tests/test_errors_scope.aer \
         tests/test_stdlib_modules.aer \
         tests/test_memory_gc.aer \
         tests/test_perf_fusion.aer

test: all
	@for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./binary/aer$(EXE) $$t || exit 1; \
	done
	@echo "=== tests/test_stdin.aer (piped input) ==="
	@echo "expected stdin content" | ./binary/aer$(EXE) tests/test_stdin.aer

# Builds and runs tests/embed_smoke_test.c, which links the library directly (no main.c/CLI).
test-embed: $(LIBOBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -c tests/embed_smoke_test.c -o object/embed_smoke_test.o
	gcc $(FLAGS) -o binary/embed_smoke_test$(EXE) $(LIBOBJECT) object/embed_smoke_test.o -lm $(WINLIBS)
	./binary/embed_smoke_test$(EXE)

# Register-VM unit test (tests/smoke_test.c) — exercises the allocator and opcodes directly,
# below the level of a real .aer file (hand-built register trees, REPL-persistence behavior, etc.),
# complementing the `test` target's end-to-end .aer coverage. Has its own main(), so main.c is
# excluded here too, matching test-embed's pattern above.
test-smoke: $(LIBOBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -c tests/smoke_test.c -o object/smoke_test.o
	gcc $(FLAGS) -o binary/smoke_test$(EXE) $(LIBOBJECT) object/smoke_test.o -lm $(WINLIBS)
	./binary/smoke_test$(EXE)

# ASAN build for tests/fuzz.py — catches non-crashing memory bugs a plain build misses.
# Needs libasan (standard on Linux/macOS); may not link on a bare MinGW/MSYS2 install.
asan: $(SOURCE)
	@mkdir -p binary
	gcc $(FLAGS) -fsanitize=address -fno-omit-frame-pointer -o binary/aer-asan$(EXE) $(SOURCE) -lm $(WINLIBS)

# Fixed seed keeps CI deterministic (see tests/fuzz.py's docstring on its one known
# non-actionable "hang" class). Override for exploratory runs, e.g. FUZZ_SEED= FUZZ_ITERATIONS=5000.
FUZZ_ITERATIONS := 300
FUZZ_SEED       := --seed 100

fuzz: asan
	python3 tests/fuzz.py --binary binary/aer-asan$(EXE) --iterations $(FUZZ_ITERATIONS) $(FUZZ_SEED)

# Profile-guided optimization: a two-pass build, not a source change. Pass 1 instruments a build
# with -fprofile-generate and runs it against nbody.aer (the actual workload this targets) plus the
# full test suite (broader code-path coverage), producing real execution-frequency data in
# object-pgo/*.gcda; pass 2 recompiles with -fprofile-use so gcc lays out hot/cold code from that
# real profile instead of static heuristics.
PGO_DIR := object-pgo

pgo: $(SOURCE)
	@rm -rf $(PGO_DIR)
	@mkdir -p binary $(PGO_DIR)
	gcc $(FLAGS) -fprofile-generate=$(PGO_DIR) -o binary/aer-pgo-gen$(EXE) $(SOURCE) -lm $(WINLIBS)
	./binary/aer-pgo-gen$(EXE) nbody.aer
	@for t in $(TESTS); do ./binary/aer-pgo-gen$(EXE) $$t >/dev/null 2>&1 || true; done
	gcc $(FLAGS) -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-coverage-mismatch -Wno-missing-profile -o binary/aer-pgo$(EXE) $(SOURCE) -lm $(WINLIBS)

COVOBJECT := $(patsubst source/%.c,object-cov/%.o,$(SOURCE))

object-cov/%.o: source/%.c
	@mkdir -p $(dir $@)
	gcc $(FLAGS) --coverage -O0 -c $< -o $@

# Coverage build: runs the test suite + a fuzz pass, then prints a per-file/overall
# line-coverage percentage via gcov (no lcov dependency). Raw .gcov files land in object-cov/.
coverage: $(COVOBJECT)
	@mkdir -p binary
	gcc $(FLAGS) --coverage -O0 -o binary/aer-cov$(EXE) $(COVOBJECT) -lm $(WINLIBS)
	@for t in $(TESTS); do ./binary/aer-cov$(EXE) $$t >/dev/null 2>&1 || true; done
	@echo "expected stdin content" | ./binary/aer-cov$(EXE) tests/test_stdin.aer >/dev/null 2>&1 || true
	python3 tests/fuzz.py --binary binary/aer-cov$(EXE) --iterations 200 $(FUZZ_SEED) || true
	@for o in $(COVOBJECT); do \
		subdir=$$(dirname $$o); \
		src=$${o#object-cov/}; src=source/$${src%.o}.c; \
		gcov -abcfu -o $$subdir $$src > /dev/null 2>&1; \
	done
	@mv -f *.gcov object-cov/ 2>/dev/null || true
	python3 tests/coverage_summary.py object-cov

clean:
	rm -rf binary/* object/* object-cov/* object-pgo/* tests/fuzz_crashes
