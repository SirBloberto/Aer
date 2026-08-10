#include <stdlib.h>
#include <string.h>
#include "aer_host.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"
#include "hashtable.h"
#include "vm.h"

/* Chunk.name_index has no owning VM (a Chunk can conceptually outlive/exist independently of any
   one VM), so unlike an AerDict -- which gets its key/sparse-array storage from its owning VM's
   own heap -- every Chunk's name_index shares this single, process-global HashPools instead. */
static HashPools chunk_name_index_pools;

void chunk_init(Chunk* c) {
    memset(c, 0, sizeof(*c));
    hashtable_pools_init(&chunk_name_index_pools);
    c->name_index.pools = &chunk_name_index_pools;
}

/* Every call site frees the owning VM first (vm_free(vm); chunk_free(chunk);) -- vm_free's
   pool_finalize_all already frees every live cell's own payload in that VM's heap, including every
   string constant's data reachable from c->pool[] (they live in the same heap, not owned by the
   Chunk). Freeing them again here would be a double free; this only tears down what's genuinely
   Chunk-owned, not heap-owned. */
void chunk_free(Chunk* c) {
    free(c->source_filename);
    free(c->code);
    free(c->pool);
    hashtable_free(&c->name_index);
    free(c->line_mark_offsets);
    free(c->line_mark_lines);
    for (unsigned int i = 0; i < c->import_count; i++)
        free(c->imported_modules[i]);
    free(c->imported_modules);
    /* functions[]/shapes[] are normally left unfreed (a Chunk usually outlives the process), but
       chunk_free is only reached via aer_module_free_all -- the one path that must actually free
       everything, not rely on process exit. */
    for (unsigned int i = 0; i < c->function_count; i++) {
        free(c->functions[i].defaults);
        free(c->functions[i].specializations);
        free(c->functions[i].source_span);
    }
    free(c->functions);
    for (unsigned int i = 0; i < c->shape_count; i++)
        free(c->shapes[i]);
    free(c->shapes);
    /* Not each entry's shape -- every populated slot's Shape* is owned by c->shapes, never separately owned. */
    free(c->field_cache);
    free(c->call_spec_cache);
    free(c->shape_by_name);
#ifdef AER_DEBUG_TOOLS
    free(c->debug_hits);
#endif
    memset(c, 0, sizeof(*c));
}

void chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line) {
    if (c->line_mark_count > 0 && c->line_mark_offsets[c->line_mark_count - 1] >= offset) return;
    if (c->line_mark_count >= c->line_mark_cap) {
        c->line_mark_cap = c->line_mark_cap ? c->line_mark_cap * 2 : 64;
        c->line_mark_offsets = xrealloc(c->line_mark_offsets, sizeof(unsigned int) * c->line_mark_cap);
        c->line_mark_lines = xrealloc(c->line_mark_lines, sizeof(unsigned int) * c->line_mark_cap);
    }
    c->line_mark_offsets[c->line_mark_count] = offset;
    c->line_mark_lines[c->line_mark_count] = line;
    c->line_mark_count++;
}

unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset) {
    if (c->line_mark_count == 0) return 0;
    unsigned int lo = 0, hi = c->line_mark_count; /* find first mark with offset > target */
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        if (c->line_mark_offsets[mid] <= offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo == 0 ? 0 : c->line_mark_lines[lo - 1];
}

void chunk_emit(Chunk* c, uint32_t word) {
    if (c->count >= c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 64;
        c->code = xrealloc(c->code, sizeof(uint32_t) * c->capacity);
    }
    c->code[c->count++] = word;
}

/* Appends v to the pool and returns its index; shared tail for both paths of chunk_add_pool. */
static unsigned int chunk_pool_append(Chunk* c, AerVal v) {
    if (c->pool_count >= c->pool_cap) {
        c->pool_cap = c->pool_cap ? c->pool_cap * 2 : 16;
        c->pool = xrealloc(c->pool, sizeof(AerVal) * c->pool_cap);
    }
    c->pool[c->pool_count] = v;
    return c->pool_count++;
}

unsigned int chunk_add_pool(Chunk* c, AerVal v) {
    /* Strings dominate call volume and the REPL never resets the pool between lines, so dedup them via name_index (O(1)) instead of the O(n) linear scan below, kept for rarer non-string literals. */
    if (aer_type(v) == TYPE_STRING) {
        /* vs->data works as the lookup key directly: every string reaching here was built by
           aer_make_string_copy, which always NUL-terminates, inline or heap. */
        AerString* vs = aer_as_string(v);
        unsigned int key_len = hashtable_key_true_len(vs->data, vs->length);
        AerVal* existing = hashtable_get(&c->name_index, vs->data, key_len);
        if (existing) return (unsigned int)aer_as_int(*existing);

        unsigned int idx = chunk_pool_append(c, v);
        /* name_index keeps its own copy so it and the pool entry free independently. */
        char* index_key = hashtable_key_dup(c->name_index.pools, vs->data, key_len, NULL);
        hashtable_put(&c->name_index, index_key, key_len, aer_int((int64_t)idx));
        return idx;
    }

    for (unsigned int i = 0; i < c->pool_count; i++) {
        AerVal* e = &c->pool[i];
        if (aer_type(*e) != aer_type(v)) continue;
        if (aer_type(v) == TYPE_NULL) return i;
        if (aer_type(v) == TYPE_INTEGER && aer_as_int(*e) == aer_as_int(v)) return i;
        if (aer_type(v) == TYPE_REAL && aer_as_real(*e) == aer_as_real(v)) return i;
        if (aer_type(v) == TYPE_BOOLEAN && aer_as_bool(*e) == aer_as_bool(v)) return i;
        if (aer_type(v) == TYPE_FUNCTION &&
            aer_as_function(*e)->code_offset == aer_as_function(v)->code_offset &&
            aer_as_function(*e)->arity == aer_as_function(v)->arity)
            return i;
    }
    return chunk_pool_append(c, v);
}

/* Newest-first so a redeclared struct (e.g. re-running a REPL block) shadows the old one for new lookups, without invalidating instances still pointing at the old Shape. */
Shape* chunk_find_shape(Chunk* c, const char* name) {
    for (unsigned int i = c->shape_count; i > 0; i--) {
        Shape* s = c->shapes[i - 1];
        if (strcmp(aer_as_string(c->pool[s->name])->data, name) == 0) return s;
    }
    return NULL;
}

/* Appended by func_register at the same moment it updates its own parse-time lookup tables. */
void chunk_add_function(Chunk* c, unsigned int name_idx, unsigned int code_offset, unsigned int arity,
                        unsigned int min_arity, AerVal* defaults) {
    if (c->function_count >= c->function_cap) {
        c->function_cap = c->function_cap ? c->function_cap * 2 : 8;
        c->functions = xrealloc(c->functions, sizeof(ChunkFunction) * c->function_cap);
    }
    ChunkFunction* f = &c->functions[c->function_count++];
    f->name = name_idx;
    f->code_offset = code_offset;
    f->arity = arity;
    f->min_arity = min_arity;
    f->defaults = defaults;
    /* Placeholder until parse_function patches in the real peak -- never an under-allocation even
       for in-body self-reference, the one case that reads it early. */
    f->max_registers = FRAME_REGISTERS;
    f->max_raw_ints = RAW_REGISTERS_INT;
    f->max_raw_reals = RAW_REGISTERS_REAL;
    f->shape_sensitive_mask = 0;
    f->source_span = NULL;
    f->source_span_len = 0;
    f->source_span_line = 0;
    f->specializations = NULL; /* allocated on first specialization -- see ChunkFunction */
    f->specialization_count = 0;
    f->megamorphic = false;
}

/* Newest-first, same convention as chunk_find_shape. */
ChunkFunction* chunk_find_function(Chunk* c, const char* name) {
    for (unsigned int i = c->function_count; i > 0; i--) {
        ChunkFunction* f = &c->functions[i - 1];
        if (strcmp(aer_as_string(c->pool[f->name])->data, name) == 0) return f;
    }
    return NULL;
}

/* Dedup'd pool index -- a plain int compare, no strcmp. Newest-first. */
ChunkFunction* chunk_find_function_by_name_idx(Chunk* c, unsigned int name_idx) {
    for (unsigned int i = c->function_count; i > 0; i--) {
        ChunkFunction* f = &c->functions[i - 1];
        if (f->name == name_idx) return f;
    }
    return NULL;
}

ChunkFunction* chunk_find_function_by_offset(Chunk* c, unsigned int code_offset) {
    for (unsigned int i = c->function_count; i > 0; i--) {
        ChunkFunction* f = &c->functions[i - 1];
        if (f->code_offset == code_offset) return f;
    }
    return NULL;
}

bool chunk_is_imported(Chunk* c, const char* name, unsigned int len) {
    for (unsigned int i = 0; i < c->import_count; i++)
        if (strlen(c->imported_modules[i]) == len && strncmp(c->imported_modules[i], name, len) == 0)
            return true;
    return false;
}

bool chunk_add_import(Chunk* c, const char* name, unsigned int len, const char* path_name,
                      unsigned int path_len) {
    bool is_native_or_host = aer_stdlib_is_native_module(name, len) || aer_host_is_module(name, len);
    /* --no-import/aer_set_import_enabled(false) only blocks file-based import (arbitrary path
       reads) -- math/net/regex/etc. are fixed dispatch, not a file read, and stay available;
       io/net have their own separate toggles for that. Checked at parse time, same as every other
       import failure (aer_module_load reports its own errors the same way, error_at() below). */
    if (!is_native_or_host && !aer_import_enabled) {
        error_at("File-based import is disabled for this run (--no-import)");
        return false;
    }
    /* Anything not a native/host module is attempted as a file-based import; aer_module_load() reports its own errors for that path. */
    if (!is_native_or_host && !aer_module_load(name, len, path_name, path_len)) {
        return false;
    }
    if (chunk_is_imported(c, name, len)) return true; /* re-importing is harmless, not an error */
    if (c->import_count >= c->import_cap) {
        c->import_cap = c->import_cap ? c->import_cap * 2 : 8;
        c->imported_modules = xrealloc(c->imported_modules, sizeof(char*) * c->import_cap);
    }
    char* copy = xmalloc(len + 1);
    memcpy(copy, name, len);
    copy[len] = '\0';
    c->imported_modules[c->import_count++] = copy;
    return true;
}
