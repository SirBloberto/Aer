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
FLAGS := -O2 -g -flto -Wall -Wextra -I include -I source -I source/compiler -I source/core -I source/stdlib -I source/utilities

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
         tests/test_perf_fusion.aer \
         tests/test_primitive_pass.aer \
         tests/test_dict_pool_stress.aer \
         tests/test_packed_arrays.aer \
         tests/test_typed_arrays.aer \
         tests/test_narrow_fields.aer \
         tests/test_shape_specialization.aer \
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

# ASAN build for tests/fuzz.py; may not link on a bare MinGW install (needs libasan).
asan: $(SOURCE)
	@mkdir -p binary
	gcc $(FLAGS) -fsanitize=address -fno-omit-frame-pointer -o binary/aer-asan$(EXE) $(SOURCE) -lm $(WINLIBS)

# Fixed seed keeps CI deterministic; override for exploratory runs (FUZZ_SEED= FUZZ_ITERATIONS=5000).
FUZZ_ITERATIONS := 300
FUZZ_SEED       := --seed 100

fuzz: asan
	python3 tests/fuzz.py --binary binary/aer-asan$(EXE) --iterations $(FUZZ_ITERATIONS) $(FUZZ_SEED)

clean:
	rm -rf binary/* object/* tests/fuzz_crashes
