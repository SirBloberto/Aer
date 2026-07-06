#ifndef AER_IO_H
#define AER_IO_H

/* Registers the "io" module (open/read/write/close) via aer_register_function — deliberately not wired into aer_stdlib.c; file access is opt-in per host (main.c calls this after vm_init(); embed_smoke_test.c doesn't, so its scripts get none). See aer_io.c. */
void aer_io_register(void);

#endif
