# Match "_NT" not "MINGW": plain Git Bash reports "MSYS_NT-...", which "MINGW" misses — and
# without -static the exe silently depends on MSYS2 DLLs and won't launch outside its build shell.
ifneq (,$(findstring _NT,$(shell uname -s 2>/dev/null)))
    EXE     := .exe
    WINLIBS := -lkernel32 -lws2_32 -static
else
    EXE     :=
    WINLIBS :=
endif

# -flto is load-bearing: pool.c's tiny hot helpers are called constantly from vm.c cross-TU.
#
# ARCH_FLAGS is deliberately empty by default -- this build must run on whatever ARM/x86 machine
# it's copied to, not just the one it was built on. On x86-64 that costs nothing: SSE2 (128-bit
# SIMD) is part of the baseline x86-64 ABI, so typed-array elementwise ops (vm.c) auto-vectorize
# with no extra flags at all. On ARM, NEON is NOT guaranteed present for the generic
# arm-linux-gnueabihf target this compiles for by default, so the compiler conservatively won't
# vectorize those same loops without an explicit opt-in -- confirmed on a Raspberry Pi 4: the exact
# same source vectorizes with `make ARCH_FLAGS=-mcpu=native` (or -mcpu=cortex-a72, etc.) and doesn't
# without it. Set ARCH_FLAGS yourself if you're building specifically for one known machine and
# want that win; leave it unset for a build that has to run anywhere.
FLAGS := -O2 -g -flto -Wall -Wextra $(ARCH_FLAGS) -I include -I source -I source/compiler -I source/core -I source/stdlib -I source/utilities

SOURCE := $(wildcard source/*.c source/compiler/*.c source/core/*.c source/stdlib/*.c source/utilities/*.c)
OBJECT := $(patsubst source/%.c,object/%.o,$(SOURCE))

# Deliberately coarse: any header edit rebuilds everything. -MMD/-MP was tried and genuinely does
# not work under GNU Make on MSYS2, and a stale .o with a mismatched struct layout links cleanly
# and misbehaves silently at runtime.
HEADERS := $(wildcard include/*.h source/*.h source/compiler/*.h source/core/*.h source/stdlib/*.h source/utilities/*.h)

# Everything except main.c — conflicts with test-embed's/test-smoke's own main().
LIBOBJECT := $(filter-out object/main.o,$(OBJECT))

all: $(OBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -o binary/aer$(EXE) $^ -lm $(WINLIBS)

object/%.o: source/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	gcc $(FLAGS) -c $< -o $@

# Disassembler/profiler build (AER_DEBUG_TOOLS): pass --debug-path=<path|-> to dump after a run.
debug-tools: $(SOURCE)
	@mkdir -p binary
	gcc $(FLAGS) -DAER_DEBUG_TOOLS -o binary/aer-debug$(EXE) $(SOURCE) -lm $(WINLIBS)

TESTS := tests/test_core.aer \
         tests/test_collections.aer \
         tests/test_functions.aer \
         tests/test_structs.aer \
         tests/test_errors_scope.aer \
         tests/test_stdlib_modules.aer \
         tests/test_memory_gc.aer \
         tests/test_card_marking.aer \
         tests/test_pool_churn.aer \
         tests/test_perf_fusion.aer \
         tests/test_raw_fma_fusion.aer \
         tests/test_fma_plain_expr_fusion.aer \
         tests/test_toplevel_raw_promotion.aer \
         tests/test_raw_const_cmp_fusion.aer \
         tests/test_struct_pool_tiers.aer \
         tests/test_primitive_pass.aer \
         tests/test_dict_pool_stress.aer \
         tests/test_packed_arrays.aer \
         tests/test_typed_arrays.aer \
         tests/test_typed_array_chain2.aer \
         tests/test_narrow_fields.aer \
         tests/test_shape_specialization.aer \
         tests/test_int_fields_specialized.aer \
         tests/test_loop_cond_registers.aer \
         tests/test_loop_bound_hoisting.aer \
         tests/test_net.aer \
         tests/test_regex.aer \
         tests/test_actor.aer \
         tests/test_scheduler.aer

test: all
	@for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./binary/aer$(EXE) $$t || exit 1; \
	done
	@echo "=== tests/test_stdin.aer (piped input) ==="
	@echo "expected stdin content" | ./binary/aer$(EXE) tests/test_stdin.aer
	@echo "=== tests/error_lines (reported line numbers) ==="
	python3 tests/error_lines.py --binary binary/aer$(EXE)

# Embedding smoke test — links the library directly, no main.c/CLI.
test-embed: $(LIBOBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -c tests/embed_smoke_test.c -o object/embed_smoke_test.o
	gcc $(FLAGS) -o binary/embed_smoke_test$(EXE) $(LIBOBJECT) object/embed_smoke_test.o -lm $(WINLIBS)
	./binary/embed_smoke_test$(EXE)

# Register-VM unit test — exercises allocator/opcodes below the .aer-file level.
test-smoke: $(LIBOBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -c tests/smoke_test.c -o object/smoke_test.o
	gcc $(FLAGS) -o binary/smoke_test$(EXE) $(LIBOBJECT) object/smoke_test.o -lm $(WINLIBS)
	./binary/smoke_test$(EXE)

# Exhaustive per-opcode encoding round-trip check — see tests/opcode_roundtrip_test.c's own comment
# for why this needs direct coverage the ordinary .aer suite doesn't provide.
test-roundtrip: $(LIBOBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -c tests/opcode_roundtrip_test.c -o object/opcode_roundtrip_test.o
	gcc $(FLAGS) -o binary/opcode_roundtrip_test$(EXE) $(LIBOBJECT) object/opcode_roundtrip_test.o -lm $(WINLIBS)
	./binary/opcode_roundtrip_test$(EXE)

# The short, readable embedding example (examples/embedding_example.c) -- see tests/embed_smoke_test.c
# for the exhaustive version this project's own test suite actually relies on.
example-embed: $(LIBOBJECT)
	@mkdir -p binary object
	gcc $(FLAGS) -c examples/embedding_example.c -o object/embedding_example.o
	gcc $(FLAGS) -o binary/embedding_example$(EXE) $(LIBOBJECT) object/embedding_example.o -lm $(WINLIBS)
	./binary/embedding_example$(EXE)

# Standalone source formatter -- its own tokenizer (source/tools/aer_fmt.c), not linked against the
# real compiler at all (see that file's own comment for why).
fmt-tool:
	@mkdir -p binary object/tools
	gcc $(FLAGS) -c source/tools/aer_fmt.c -o object/tools/aer_fmt.o
	gcc $(FLAGS) -o binary/aer-fmt$(EXE) object/tools/aer_fmt.o

# Language server -- links the real compiler directly (LIBOBJECT, same everything-except-main.c
# set test-embed/example-embed already use), unlike aer-fmt: diagnostics are the real parser's own
# errors, not a reimplementation.
lsp-tool: $(LIBOBJECT)
	@mkdir -p binary object/tools
	gcc $(FLAGS) -c source/tools/aer_lsp.c -o object/tools/aer_lsp.o
	gcc $(FLAGS) -o binary/aer-lsp$(EXE) $(LIBOBJECT) object/tools/aer_lsp.o -lm $(WINLIBS)

# tests/fmt_input.aer (deliberately messy) must format to exactly tests/fmt_expected.aer, and that
# expected output must be a fixed point (formatting it again changes nothing) -- idempotency is
# the formatter's actual correctness bar, not "looks right".
test-fmt: fmt-tool
	./binary/aer-fmt$(EXE) tests/fmt_input.aer > object/fmt_test_out.aer
	diff tests/fmt_expected.aer object/fmt_test_out.aer
	./binary/aer-fmt$(EXE) object/fmt_test_out.aer > object/fmt_test_out2.aer
	diff object/fmt_test_out.aer object/fmt_test_out2.aer
	@echo "test-fmt: input formats to the expected canonical output, which is a fixed point"

# Profile-guided optimization -- two-phase build (make pgo). Re-tested from scratch against
# current HEAD: the codebase has changed substantially since an earlier attempt showed a severe
# struct_array_scan.aer regression (struct pool tiering, FMA fusion, several range-for rewrites all
# landed since). Uses its own object tree (object/pgo), not the ordinary object/, since a .gcda
# profile counter file is tied to the exact directory its .o was compiled into -- phase 2
# recompiles in place rather than deleting anything between phases, since wiping object/pgo before
# the -fprofile-use compile would also delete the just-recorded counters it depends on.
#
# Re-measured full 15-benchmark suite on a Raspberry Pi 4 (perf stat, performance governor): still
# genuinely mixed, not a clean win -- 6 benchmarks improve on wall-clock (mandelbrot -16.2%, nbody
# -6.6%, binary_trees -5.0%, sieve -4.7%, nbody_large_packed -4.2%, typed_array_bench -2.3%), 8
# regress (dict_bench +17.2%, nbody_large_packed_narrow +10.2%, small_dict_bench +9.4%,
# lookup_table_bench +5.3%, typed_elementwise +3.4%, fib_bench +3.1%, log_processing +3.0%,
# nbody_large_boxed +2.5%), and struct_array_scan is roughly flat (+1.5%, instructions -9.1% but
# branch-misses 1.45M->196M). Less severe than the earlier attempt, but dict_bench and
# struct_array_scan were BOTH in PGO_TRAIN below and still regressed/went flat -- PGO isn't
# reliably helping even on benchmarks it trained on. Kept as opt-in tooling only (this target isn't
# part of `all`/the default build), same treatment debug-tools/asan already get -- not because the
# mechanism is broken, but because "which half of your benchmarks do you want to sacrifice" isn't a
# decision this build should make silently by default.
PGO_OBJDIR := object/pgo
# Deliberately mixed shapes -- a numeric loop-heavy pair (nbody, sieve), a dict-heavy one
# (dict_bench), and two struct/packed-array-allocation-heavy ones (binary_trees,
# struct_array_scan) -- so the profile isn't overfit to one access pattern.
PGO_TRAIN := bench/nbody.aer bench/sieve.aer bench/dict_bench.aer bench/binary_trees.aer bench/struct_array_scan.aer

pgo:
	@rm -rf $(PGO_OBJDIR) binary/aer-pgo-gen$(EXE) binary/aer-pgo$(EXE)
	@mkdir -p $(PGO_OBJDIR) binary
	@for f in $(SOURCE); do \
		o=$(PGO_OBJDIR)/$$(echo $$f | sed -e 's|^source/||' -e 's|\.c$$|.o|'); \
		mkdir -p $$(dirname $$o); \
		gcc $(FLAGS) -fprofile-generate -c $$f -o $$o || exit 1; \
	done
	gcc $(FLAGS) -fprofile-generate -o binary/aer-pgo-gen$(EXE) $(patsubst source/%.c,$(PGO_OBJDIR)/%.o,$(SOURCE)) -lm $(WINLIBS)
	@echo "-- training on a representative benchmark mix (numeric loop / dict / struct-allocation heavy) --"
	@for b in $(PGO_TRAIN); do ./binary/aer-pgo-gen$(EXE) $$b >/dev/null || exit 1; done
	@for f in $(SOURCE); do \
		o=$(PGO_OBJDIR)/$$(echo $$f | sed -e 's|^source/||' -e 's|\.c$$|.o|'); \
		gcc $(FLAGS) -fprofile-use -fprofile-correction -c $$f -o $$o || exit 1; \
	done
	gcc $(FLAGS) -fprofile-use -fprofile-correction -o binary/aer-pgo$(EXE) $(patsubst source/%.c,$(PGO_OBJDIR)/%.o,$(SOURCE)) -lm $(WINLIBS)
	@echo "PGO build complete: binary/aer-pgo$(EXE) (training binary binary/aer-pgo-gen$(EXE) left in place too)"

# ASAN build for tests/fuzz.py; may not link on a bare MinGW install (needs libasan).
asan: $(SOURCE)
	@mkdir -p binary
	gcc $(FLAGS) -fsanitize=address -fno-omit-frame-pointer -o binary/aer-asan$(EXE) $(SOURCE) -lm $(WINLIBS)

# Fixed seed keeps CI deterministic; override for exploratory runs (FUZZ_SEED= FUZZ_ITERATIONS=5000).
FUZZ_ITERATIONS := 300
FUZZ_SEED       := --seed 100

fuzz: asan
	python3 tests/fuzz.py --binary binary/aer-asan$(EXE) --iterations $(FUZZ_ITERATIONS) $(FUZZ_SEED)

# Formatting and comment-density gates -- both run in CI, so a drifting tree fails the build
# rather than relying on whoever is editing to remember the conventions (ARCHITECTURE.md).
FORMAT_FILES := $(wildcard source/*.c source/*.h source/*/*.c source/*/*.h include/*.h)

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	clang-format --dry-run --Werror $(FORMAT_FILES)

check-comments:
	python3 tools/check_comments.py

check-style: check-format check-comments

# Clean-builds both refs on the Pi and prints a per-benchmark delta table. Always use this rather
# than comparing against a checkout that happens to be lying around -- tar preserves mtimes, so a
# stale tree can silently skip rebuilding and report a regression as noise.
bench:
	python3 tools/bench.py $(BENCH_ARGS)

clean:
	rm -rf binary/* object/* tests/fuzz_crashes
