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

/* Embedding: error reporting. Process-global, not per-VM, not thread-safe. A runtime error does not
   terminate the process -- vm_run() returns cleanly. */

/* Where error()/error_at()/aer_report_fatal() report instead of stderr. `message` is owned by AER --
   copy it to keep it. NULL restores stderr. */
typedef void (*AerErrorCallback)(const char* message, void* userdata);
void aer_set_error_callback(AerErrorCallback callback, void* userdata);

/* The last reported message, or "" since the last aer_clear_error(), for a host that polls rather
   than installing a callback. */
const char* aer_last_error(void);

/* parse_had_error or runtime_had_error, without depending on either directly. */
bool aer_had_error(void);

/* Call before each top-level invocation for a clean read of that one's outcome. */
void aer_clear_error(void);

/* Embedding: custom native functions, reached like any module -- `import game; game.spawn_enemy(x, y)`.
   Process-global, and must be registered before parsing a script that references it. */

/* `args` is a fresh copy valid for this call only, not a pointer into the VM's stack; build and read
   the values with aer_int()/aer_as_int() and friends. Report failure with error(). */
typedef AerVal (*AerNativeFn)(VM* vm, int arg_count, AerVal* args, void* userdata);

/* Registers `fn` as `module.name`. False if the registry is full (MAX_HOST_FUNCTIONS). */
bool aer_register_function(const char* module, const char* name, AerNativeFn fn, void* userdata);

/* Failed assert() calls, which report and continue. A run can have none of the errors above and
   still have failed assertions, so a host deciding whether it succeeded must check both. */
unsigned int aer_assert_failure_count(void);

/* Embedding: GC introspection and tuning. See the README's Memory and Security section. */

/* Any out-param may be NULL. live_cells is the pools' own bookkeeping, not a fresh trace, so it
   counts garbage not yet swept. Reports on the most recently run VM, not the process. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections, unsigned int* major_collections);

/* Defaults are a 2048-cell minor threshold and a major every 10th minor. 0 leaves one unchanged.
   Safe before any VM exists: later VMs inherit it, and the current one updates immediately. */
void aer_gc_configure(unsigned int minor_threshold, unsigned int major_every_n_minor);

/* A -Xmx-style ceiling on one VM's live cells; 0 means unlimited. Still over it after a forced
   collection is a recoverable runtime error, not a process exit. */
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
