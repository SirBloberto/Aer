#ifndef AER_OBJECTS_H
#define AER_OBJECTS_H

#include <stdbool.h>
#include <stddef.h>
#include "hashtable.h"
#include "value.h"

/* gc_state first -- see pool.h and AerArray's own comment (value.h) for why. Small fields ordered to
   fill gc_state's padding gap before map (which needs pointer alignment), not declaration-grouped
   by topic -- see value.h's own top comment. */
struct AerDict {
    unsigned char gc_state;
    /* Card marking for the O(n) minor-GC rescan fix -- same fields, same reasoning, as AerArray's
       own (value.h). dirty_cards indexes map.dense[] by its DENSE index (stable across ordinary
       insert/update; hashtable_remove's swap-compaction invalidates it, which is why
       collection.delete sets dirty_all rather than trying to shift the affected bit). */
    bool dirty_all;
    unsigned int dirty_cards_bytes;
    HashTable map;
    unsigned char* dirty_cards;
    /* Bounds the actual set-bit range since the last clear -- see AerArray's own comment (value.h)
       for why this is needed on top of dirty_cards itself. */
    unsigned int dirty_min_byte, dirty_max_byte;
};
_Static_assert(offsetof(struct AerDict, gc_state) == 0, "pool.c assumes gc_state is byte 0");

#define MAX_STRUCT_FIELDS 16

/* A struct type's blueprint (field names in order + default literals); individually heap-allocated
   and never moved/realloc'd, so AerStruct.shape pointers stay valid as the shape table grows. */
struct Shape {
    unsigned int name; /* pool index of the struct's type name */
    unsigned int field_count;
    unsigned int field_names[MAX_STRUCT_FIELDS]; /* pool indices, declaration order       */
    AerVal field_defaults[MAX_STRUCT_FIELDS];
    /* TYPE_ANY = no declared type. A declared type is enforced once at FIELD_SET/construction,
       then trusted -- the fused opcodes skip the runtime check on that side. */
    ValueType field_types[MAX_STRUCT_FIELDS];
    /* An int/real field whose default carried an `i`/`f` suffix: 4-byte storage instead of 8.
       Meaningless for any other field kind. */
    bool field_narrow[MAX_STRUCT_FIELDS];
    /* Offset into AerStruct.fields. A typed field is stored raw -- 8 bytes, or 4 if field_narrow,
       with the type known from this Shape rather than the instance. A TYPE_ANY field stays a boxed
       16-byte AerVal, since it can hold a reference the GC must trace. */
    unsigned int field_offsets[MAX_STRUCT_FIELDS];
    unsigned int instance_bytes; /* total size of the fields buffer -- sum of every field's width above */
};

/* Its own type rather than an AerArray with a shape: sharing one tag meant every TYPE_ARRAY site
   had to ask "but what if this is a struct", and one didn't -- for-in iteration walked a struct's
   fields unchecked. No count/capacity either; a struct's field count is always shape->field_count. */
struct AerStruct {
    unsigned char gc_state; /* byte 0, same pool.c convention as every other pool-managed type */
    Shape* shape;
    unsigned char* fields; /* set to (char*)a + sizeof(AerStruct) at construction -- inline in
                                   the same pool cell, matching AerArray's own items pointer trick */
};
_Static_assert(offsetof(struct AerStruct, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* One field at its own offset: raw for a typed field, a boxed AerVal for TYPE_ANY. Not file-static
   -- json.encode serializes structs through it too. */
AerVal vm_struct_field_read(AerStruct* s, unsigned int slot);

#endif
