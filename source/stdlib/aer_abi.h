#ifndef AER_ABI_H
#define AER_ABI_H

/* The standard library's wire identities: which module, which function within it, which builtin.
   These are what OP_CALL_MODULE and OP_CALL_BUILTIN carry in their trailing words, so the parser
   emits them and the VM dispatches on them -- but they describe the LIBRARY, not the machine, and
   they outnumbered every other kind of constant in vm.h put together. Adding a stdlib function
   should not mean editing the VM's core header. */

/* The native modules. The CALL_MODULE_* ids, aer_stdlib_is_native_module's name check, the parser's
   name lookup and the VM's dispatch switch are all generated from this one list -- stating the set
   in four places made a missing entry a silent misroute rather than a compile error. The third
   column is the call itself, so json needing the chunk costs no special case; it expands only where
   those names are in scope. */
#define AER_NATIVE_MODULES(X)                                                                                \
    X(MATH, "math", aer_math_call(vm, fn_id, arg_count))                                                     \
    X(RANDOM, "random", aer_random_call(vm, fn_id, arg_count))                                               \
    X(STRING, "string", aer_string_call(vm, fn_id, arg_count))                                               \
    X(TIME, "time", aer_time_call(vm, fn_id, arg_count))                                                     \
    X(JSON, "json", aer_json_call(vm, c, fn_id, arg_count))                                                  \
    X(COLLECTION, "collection", aer_collection_call(vm, fn_id, arg_count))                                   \
    X(NET, "net", aer_net_call(vm, fn_id, arg_count))                                                        \
    X(REGEX, "regex", aer_regex_call(vm, fn_id, arg_count))                                                  \
    X(ACTOR, "actor", aer_actor_module_call(vm, fn_id, arg_count))                                           \
    X(SCHEDULER, "scheduler", aer_scheduler_module_call(vm, fn_id, arg_count))

/* CALL_MODULE_DYNAMIC (a host or file module, resolved by name at runtime) must stay last -- it is
   the one id not in the list above. */
#define AER_MODULE_ID(name, str, call) CALL_MODULE_##name,
enum { AER_NATIVE_MODULES(AER_MODULE_ID) CALL_MODULE_DYNAMIC };
#undef AER_MODULE_ID

/* Second trailing word: fn_id within the module (each module owns a flat id space);
   FN_ID_UNKNOWN still errors by name, never misroutes to id 0. */
#define FN_ID_UNKNOWN (-1)

#define FN_MATH_SQRT 0
#define FN_MATH_POW 1
#define FN_MATH_FLOOR 2
#define FN_MATH_CEIL 3
#define FN_MATH_ABS 4
#define FN_MATH_MIN 5
#define FN_MATH_MAX 6
#define FN_MATH_SIN 7
#define FN_MATH_COS 8
#define FN_MATH_LOG 9
#define FN_MATH_LOG2 10
#define FN_MATH_LOG10 11
#define FN_MATH_PI 12
#define FN_MATH_ROUND 13
#define FN_MATH_TAN 14
#define FN_MATH_EXP 15
#define FN_MATH_ASIN 16
#define FN_MATH_ACOS 17
#define FN_MATH_ATAN 18
#define FN_MATH_ATAN2 19

#define FN_RANDOM_RANDOM 0
#define FN_RANDOM_RANDINT 1
#define FN_RANDOM_SEED 2
#define FN_RANDOM_CHOICE 3
#define FN_RANDOM_SHUFFLE 4

#define FN_STRING_UPPER 0
#define FN_STRING_LOWER 1
#define FN_STRING_TRIM 2
#define FN_STRING_CONTAINS 3
#define FN_STRING_SPLIT 4
#define FN_STRING_STARTS_WITH 5
#define FN_STRING_ENDS_WITH 6
#define FN_STRING_REPEAT 7
#define FN_STRING_REPLACE 8
#define FN_STRING_JOIN 9
#define FN_STRING_INDEX_OF 10
#define FN_STRING_TO_INTEGER 11
#define FN_STRING_TO_FLOAT 12

#define FN_TIME_NOW 0
#define FN_TIME_STRFTIME 1
#define FN_TIME_SLEEP 2
#define FN_TIME_PARSE 3
#define FN_TIME_TO_PARTS 4
#define FN_TIME_FROM_PARTS 5

#define FN_JSON_ENCODE 0
#define FN_JSON_DECODE 1

#define FN_COLLECTION_APPEND 0
#define FN_COLLECTION_DELETE 1
#define FN_COLLECTION_COPY 2
#define FN_COLLECTION_INSERT 3
#define FN_COLLECTION_INDEX_OF 4
#define FN_COLLECTION_KEYS 5
#define FN_COLLECTION_SORT 6
#define FN_COLLECTION_RESERVE 7
#define FN_COLLECTION_SUM 8
#define FN_COLLECTION_MIN 9
#define FN_COLLECTION_MAX 10
#define FN_COLLECTION_GROUP_SUM 11
/* Compiler-emitted only, so absent from the parser's name lookup: a fused array expression under
   sum() or group_sum(), and the group_sum that ADDS into an array the caller already has (returning
   a fresh one would rebind the name and change what every other reference to it sees). */
#define FN_COLLECTION_SUM_CHAIN 12
#define FN_COLLECTION_GROUP_SUM_INTO 13
#define FN_COLLECTION_GROUP_SUM_CHAIN 14

#define FN_NET_CONNECT 0
#define FN_NET_SEND 1
#define FN_NET_RECV 2
#define FN_NET_CLOSE 3
#define FN_NET_LISTEN 4
#define FN_NET_ACCEPT 5

#define FN_REGEX_MATCH 0
#define FN_REGEX_FIND 1
#define FN_REGEX_REPLACE 2
#define FN_REGEX_FIND_ALL 3

#define FN_ACTOR_SPAWN 0
#define FN_ACTOR_SEND 1
#define FN_ACTOR_RECEIVE 2
#define FN_ACTOR_CALL 3
#define FN_ACTOR_KEEP 4
#define FN_ACTOR_KEPT 5
#define FN_ACTOR_GIVE 6

#define FN_SCHEDULER_ADD 0
#define FN_SCHEDULER_RUN 1

/* OP_CALL_BUILTIN's trailing builtin_id -- no DYNAMIC case; is_builtin_name gates every site. */
#define CALL_BUILTIN_LENGTH 0
#define CALL_BUILTIN_PRINT 1
#define CALL_BUILTIN_TYPE 2
#define CALL_BUILTIN_ASSERT 3
#define CALL_BUILTIN_PANIC 4
/* The only way AER source constructs a Result -- lets user functions join |>'s short-circuit. */
#define CALL_BUILTIN_RESULT 5

#endif /* AER_ABI_H */
