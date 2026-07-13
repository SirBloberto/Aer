#include <stdbool.h>
#include <stdio.h>
#include "aer.h"
#include "error.h"
#include "lexer.h"
#include "parser_v3.h"
#include "vm.h"

Token token;
Mode  mode;

int main(void) {
    mode = MODE_RUN;
    Chunk c;
    chunk_init(&c);
    const char* src =
        "function fib(n):\n"
        "    if n <= 1:\n"
        "        return n\n"
        "    return fib(n - 1)\n"
        "y = fib(5)\n";
    shell((char*)src);
    lex();
    v3_parse(&c);
    chunk_emit(&c, OP_HALT);
    VM vm;
    vm_init(&vm, &c);
    runtime_had_error = false;
    bool ok = vm_run(&vm);
    printf("ok=%d y=%lld\n", ok, ok ? (long long)aer_as_int(v3_register_get(0)) : -1);
    chunk_free(&c);
    return 0;
}
