#ifndef AER_ABI_H
#define AER_ABI_H

/* The standard library's wire identities: which module, which function within it, which builtin.
   These are what OP_CALL_MODULE and OP_CALL_BUILTIN carry in their trailing words, so the parser
   emits them and the VM dispatches on them -- but they describe the LIBRARY, not the machine, and
   they outnumbered every other kind of constant in vm.h put together. Adding a stdlib function
   should not mean editing the VM's core header. */

/* OP_CALL_MODULE's trailing module_id word, resolved at parse time; CALL_MODULE_DYNAMIC =
   host/file module, resolved by name at runtime. */
#define CALL_MODULE_MATH 0
#define CALL_MODULE_RANDOM 1
#define CALL_MODULE_STRING 2
#define CALL_MODULE_TIME 3
#define CALL_MODULE_JSON 4
#define CALL_MODULE_COLLECTION 5
#define CALL_MODULE_NET 6
#define CALL_MODULE_REGEX 7
#define CALL_MODULE_ACTOR 8
#define CALL_MODULE_SCHEDULER 9
#define CALL_MODULE_DYNAMIC 10

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
