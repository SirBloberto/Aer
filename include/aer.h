#ifndef AER_H
#define AER_H

#include <stdbool.h>
#include "vm.h"

#define VERSION "1.0"

/* Set by the makefile from `git describe`; a build made outside a checkout says so rather than
   lying about which commit it is. Reported by `aer version` and recorded in every benchmark table
   tools/compare_languages.py prints, so a result always carries the build it came from. */
#ifndef AER_BUILD_REV
#define AER_BUILD_REV "unknown"
#endif

/* Set by the makefile from `git describe`; a build made outside a checkout says so rather than
   lying about which commit it is. Reported by `aer version` and by tools/compare_languages.py, so a
   benchmark table always records the build it came from. */
#ifndef AER_BUILD_REV
#define AER_BUILD_REV "unknown"
#endif

/* Embedding: error reporting. By default every parse/runtime error and fatal condition prints to stderr; a runtime error does not terminate the process (vm_run() returns cleanly). This state is process-global, not per-VM, and not thread-safe. */

/* Registers a sink for every message error()/error_at()/aer_report_fatal() would otherwise print to stderr. `message` is a NUL-terminated, fully formatted string owned by AER -- copy it if it must outlive the callback call. Pass NULL to restore the default (stderr), also the behavior with no call at all. */
typedef void (*AerErrorCallback)(const char* message, void* userdata);
void aer_set_error_callback(AerErrorCallback callback, void* userdata);

/* The most recently reported error/fatal message, or "" if none since the last aer_clear_error() or process start; useful for a host that would rather poll after a call returns than intercept via callback. */
const char* aer_last_error(void);

/* Equivalent to checking both parse_had_error and runtime_had_error without depending on those globals directly. */
bool aer_had_error(void);

/* Resets aer_had_error()/aer_last_error() state. A host driving vm_run() directly on its own VM should call this before each top-level script invocation to get a clean read of that invocation's outcome. */
void aer_clear_error(void);

/* Embedding: custom native functions. A host registers a C function under a module name; AER scripts reach it like the built-in math/random/string modules, via `import` then dot-call -- `import game; game.spawn_enemy(x, y)`. Registration is process-global and must happen before parsing/running any script that references it. */

/* `args` points at a contiguous block of arg_count AerVal values (value.h -- construct/read via aer_int()/aer_as_int() and friends, never raw field access), copied fresh for this one call -- valid for the call's duration, not a live pointer into the calling VM's own stack. Return the call's result directly; to report an error, call error() (source/utilities/error.h), the same recoverable error path every other AER error goes through. */
typedef AerVal (*AerNativeFn)(VM* vm, int arg_count, AerVal* args, void* userdata);

/* Registers `fn` as `module.name`. Returns false if the registry is full (MAX_HOST_FUNCTIONS, source/core/aer_host.h). */
bool aer_register_function(const char* module, const char* name, AerNativeFn fn, void* userdata);

/* The number of assert() calls that have failed (reported and continued -- see the Language Reference's assert() entry) since the last aer_clear_error() or process start. A script can finish with zero parse/runtime errors (aer_had_error() false) and still have failed assertions; check both when deciding whether a run actually succeeded. */
unsigned int aer_assert_failure_count(void);

/* Embedding: garbage collector introspection and tuning. See the README's Memory and Security section for the generational design these report on and configure. */

/* Any of the three out-params may be NULL if that figure isn't wanted. live_cells is a snapshot from the pools' own bookkeeping (not a fresh trace), so it can include cells that are actually garbage but haven't been swept yet -- same caveat any generational collector's "live" figure has between collections. Each VM now has its own independent heap (see README's Memory and Security section) -- this reports on whichever VM most recently ran (or was vm_init()'d), not a single process-wide figure. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections, unsigned int* major_collections);

/* Overrides the collector's tuning constants (defaults: 2048 minor threshold, every 10th minor triggers a major). 0 for either argument leaves that one unchanged, so a host can override just one knob without needing to repeat the other's current value. Safe to call at any time, including before any VM exists yet -- every VM created afterward inherits the new defaults, and whichever VM is already current (if any) is updated immediately too. */
void aer_gc_configure(unsigned int minor_threshold, unsigned int major_every_n_minor);

/* Caps total live cells across one VM's own seven pools (each VM has its own independent heap -- see README's Memory and Security section) -- a -Xmx-style ceiling; 0 (the default) means unlimited. Like aer_gc_configure, safe to call before any VM exists (every VM created afterward inherits it) or on an already-running one. If a script is still over the ceiling immediately after a forced extra collection, it aborts with a normal, recoverable runtime error (aer_last_error()), the same non-fatal path every other runtime fault takes, not a process exit. */
void aer_gc_set_ceiling(unsigned int max_live_cells);

/* Embedding: running source text without hand-assembling lex()/parse()/vm_run() yourself -- these
   two calls bundle that sequence once. */

/* Resets vm's stack/call-frame state for a fresh top-level call -- needed before reusing a VM* a
   previous call may have left mid-scope (an aborted call leaves scopes pushed that normal
   execution would have unwound). aer_run_source() below calls this itself; exposed separately for
   a host driving vm_run()/vm_run_slice() directly on its own VM without going through
   aer_run_source(). */
void aer_vm_reset_for_reuse(VM* vm);

/* Parses source, appended after whatever is already in chunk -- the incremental-compile model the
   REPL relies on -- and runs it on vm, returning whatever vm_run returns. Does not check
   parse_had_error first, matching the REPL's recover-and-continue behavior; a host wanting file
   mode's stricter "run nothing if anything failed to compile" should check it before calling. */
bool aer_run_source(VM* vm, Chunk* chunk, const char* source);

/* Embedding: coarse capability toggles. All three default true (unchanged from every prior
   release). This is a blast-radius limiter for "don't let this script touch the filesystem/
   network/other files at all," not a real permission system -- no path/host allowlisting, no
   per-VM granularity. Disabling a capability makes the corresponding operation fail with a normal,
   recoverable error (aer_last_error()), the same non-fatal path every other runtime fault takes.
   See the README's Sandboxing note for what a real permission system would still need. */
void aer_set_io_enabled(bool enabled);
void aer_set_net_enabled(bool enabled);
void aer_set_import_enabled(bool enabled);

#endif
