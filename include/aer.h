#ifndef AER_H
#define AER_H

#include <stdbool.h>
#include "vm.h"

#define VERSION "0.0"

/* Embedding: error reporting. By default every parse/runtime error and fatal condition prints to stderr, and a runtime error no longer terminates the process (vm_run() returns cleanly); this state is process-global, not per-VM, and not thread-safe. */

/* Registers a sink for every message error()/error_at()/aer_report_fatal() would otherwise print to stderr. `message` is a NUL-terminated, fully formatted string owned by AER — copy it if it must outlive the callback call. Pass NULL to restore the default (stderr), also the behavior with no call at all. */
typedef void (*AerErrorCallback)(const char* message, void* userdata);
void aer_set_error_callback(AerErrorCallback callback, void* userdata);

/* The most recently reported error/fatal message, or "" if none since the last aer_clear_error() or process start; useful for a host that would rather poll after a call returns than intercept via callback. */
const char* aer_last_error(void);

/* Equivalent to checking both parse_had_error and runtime_had_error without depending on those globals directly. */
bool aer_had_error(void);

/* Resets aer_had_error()/aer_last_error() state. A host driving vm_run() directly on its own VM should call this before each top-level script invocation to get a clean read of that invocation's outcome. */
void aer_clear_error(void);

/* Embedding: custom native functions. A host registers a C function under a module name; AER scripts reach it like the built-in math/random/string modules, via `import` then dot-call — `import game; game.spawn_enemy(x, y)`. Registration is process-global and must happen before parsing/running any script that references it. */

/* `args` points at a contiguous block of arg_count values, built fresh for this one call — converted from the VM's internal representation, valid for the call's duration (not a live pointer into the calling VM's own stack). Return the call's result directly; to report an error, call error() (source/utilities/error.h), the same recoverable error path every other AER error goes through. */
typedef Value (*AerNativeFn)(VM* vm, int arg_count, Value* args, void* userdata);

/* Registers `fn` as `module.name`. Returns false if the registry is full (MAX_HOST_FUNCTIONS, source/core/aer_host.h). */
bool aer_register_function(const char* module, const char* name, AerNativeFn fn, void* userdata);

/* The number of assert() calls that have failed (reported and continued — see the Language Reference's assert() entry) since the last aer_clear_error() or process start. A script can finish with zero parse/runtime errors (aer_had_error() false) and still have failed assertions; check both when deciding whether a run actually succeeded. */
unsigned int aer_assert_failure_count(void);

/* Embedding: garbage collector introspection and tuning. See the README's Memory and Security section for the generational design these report on and configure. */

/* Any of the three out-params may be NULL if that figure isn't wanted. live_cells is a snapshot from the pools' own bookkeeping (not a fresh trace), so it can include cells that are actually garbage but haven't been swept yet — same caveat any generational collector's "live" figure has between collections. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections,
                  unsigned int* major_collections);

/* Overrides the collector's tuning constants (defaults: 2048 minor threshold, every 10th minor triggers a major). 0 for either argument leaves that one unchanged, so a host can override just one knob without needing to repeat the other's current value. Safe to call at any time; takes effect on the next collection check. */
void aer_gc_configure(unsigned int minor_threshold, unsigned int major_every_n_minor);

/* Caps total live cells across all five pools — a -Xmx-style ceiling; 0 (the default) means unlimited. If a script is still over the ceiling immediately after a forced extra collection, it aborts with a normal, recoverable runtime error (aer_last_error()), the same non-fatal path every other runtime fault takes, not a process exit. */
void aer_gc_set_ceiling(unsigned int max_live_cells);

#endif
