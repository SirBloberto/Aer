#ifndef LEXER_H
#define LEXER_H

#include <stdbool.h>
#include "value.h"

typedef enum TokenType {
    /* Binary ops — precedence is looked up by binary_op_info()'s switch in parser.c, not by enum
       order, so nothing here needs to stay contiguous. */
    TOKEN_OR,               /* or */
    TOKEN_PIPE,             /* |> — x |> f(a) desugars to f(x, a) */
    TOKEN_AND,              /* and */
    TOKEN_BITWISE_OR,       /* |  */
    TOKEN_BITWISE_XOR,      /* ^  */
    TOKEN_BITWISE_AND,      /* &  */
    TOKEN_EQUAL,            /* == */
    TOKEN_NOT_EQUAL,        /* != */
    TOKEN_LESS,             /* <  */
    TOKEN_GREATER,          /* >  */
    TOKEN_LESS_EQUAL,       /* <= */
    TOKEN_GREATER_EQUAL,    /* >= */
    TOKEN_IN,               /* in — membership test: `key in dict`, `value in array` */
    TOKEN_LEFT_SHIFT,       /* << */
    TOKEN_RIGHT_SHIFT,      /* >> */
    TOKEN_ADD,              /* +  */
    TOKEN_SUBTRACT,         /* -  */
    TOKEN_MULTIPLY,         /* *  */
    TOKEN_DIVIDE,           /* /  */
    TOKEN_MODULO,           /* %  */
    TOKEN_FLOOR_DIVIDE,     /* // */
    TOKEN_AS,               /* as — type cast: x as integer/float/string/boolean */

    /* Assignment operators */
    TOKEN_ASSIGN,           /* =   */
    TOKEN_ADD_ASSIGN,       /* +=  */
    TOKEN_SUBTRACT_ASSIGN,  /* -=  */
    TOKEN_MULTIPLY_ASSIGN,  /* *=  */
    TOKEN_DIVIDE_ASSIGN,    /* /=  */
    TOKEN_MODULO_ASSIGN,    /* %=  */
    TOKEN_FLOOR_DIVIDE_ASSIGN, /* //= */

    /* Unary operators */
    TOKEN_NOT,              /* not — see binary_op_info's comment: binds tighter than and/or, looser than everything else */
    TOKEN_BITWISE_NOT,      /* ~  */

    /* Punctuation */
    TOKEN_OPEN_PARENTHESE,  /* (  */
    TOKEN_CLOSE_PARENTHESE, /* )  */
    TOKEN_OPEN_BRACKET,     /* [  */
    TOKEN_CLOSE_BRACKET,    /* ]  */
    TOKEN_OPEN_BRACE,       /* {  */
    TOKEN_CLOSE_BRACE,      /* }  */
    TOKEN_COLON,            /* :  */
    TOKEN_DOT,              /* .  — struct field access: p.x */
    TOKEN_DOT_DOT,          /* .. */

    /* Literals */
    TOKEN_IDENTIFIER,
    TOKEN_INTEGER,
    TOKEN_REAL,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_STRING,

    /* Keywords */
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_STRUCT,
    TOKEN_FUNCTION,
    TOKEN_RETURN,
    TOKEN_RAISE,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_NULL,
    TOKEN_IMPORT,

    /* Reserved type names, unusable as any identifier. `string` is excluded -- it collides with the stdlib `string` module. */
    TOKEN_TYPE_INTEGER,
    TOKEN_TYPE_FLOAT,
    TOKEN_TYPE_BOOLEAN,
    TOKEN_TYPE_ARRAY,
    TOKEN_TYPE_HASHTABLE,

    TOKEN_COMMA,

    TOKEN_INDENT,
    TOKEN_DEDENT,
    TOKEN_NEW_LINE,
    TOKEN_END_OF_FILE,

    /* Emitted for a byte lex() cannot make a token from (already reported via error_at()); always consumes exactly one byte so callers (parser.c's error-recovery skip loop) are guaranteed forward progress. */
    TOKEN_ERROR,
} TokenType;

typedef struct Token {
    TokenType type;
    AerVal    value;
} Token;

void read_file(char* name);
void shell(char* line);
void lex();

bool equal(TokenType match);
void require(TokenType match, const char* msg);
bool consume(TokenType match);

extern Token token;

/* Exposed so error.c can print source context */
const char* current_source_start();
const char* current_source_cursor();

/* Report a deferred error at a SAVED cursor position: save current_source_cursor() first,
   override, then restore. Mutates the current File's cursor; does not switch files. */
void lexer_set_cursor(const char* pos);

/* Exposed so parser.c can tag each statement's bytecode with its source line (see Chunk.line_mark_offsets, vm.h). */
unsigned int current_source_line();

/* Exposed so a file-based `import` can resolve a sibling .aer path relative to the file currently being lexed. */
const char* current_source_name();

/* Captures everything read_file()/indent tracking need to resume the CURRENT file's lexing where it left off after a nested read_file()+lex()+parse() cycle for an imported file completes; opaque outside lexer.c — only ever saved and restored, never inspected. */
typedef struct LexerState LexerState;
LexerState* lexer_save_state(void);
void        lexer_restore_state(LexerState* state);

/* Begins lexing a fresh, independent text span (e.g. a string interpolation's `{expr}` body) —
   see this function's own comment in lexer.c for the save/restore contract callers must follow. */
void lexer_begin_span(const char* text, unsigned int len);

#endif
