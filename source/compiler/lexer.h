#ifndef LEXER_H
#define LEXER_H

#include <stdbool.h>
#include "value.h"

/* Binary-operator tokens must be contiguous and in the same order as BinaryOperation in parser.h — the parser's precedence table relies on this. */
typedef enum TokenType {
    /* Binary ops (contiguous block — do not reorder) */
    TOKEN_OR,               /* || */
    TOKEN_PIPE,             /* |> — x |> f(a) desugars to f(x, a) */
    TOKEN_AND,              /* && */
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
    TOKEN_LEFT_SHIFT_ASSIGN,/* <<= */
    TOKEN_RIGHT_SHIFT_ASSIGN,/* >>= */
    TOKEN_AND_ASSIGN,       /* &=  */
    TOKEN_OR_ASSIGN,        /* |=  */
    TOKEN_XOR_ASSIGN,       /* ^=  */

    /* Unary operators */
    TOKEN_NOT,              /* !  */
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
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_NULL,
    TOKEN_IMPORT,
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

/* Exposed so parser.c's forward-reference resolution can report a deferred "genuinely never
   defined" error at the ORIGINAL call site instead of wherever the cursor ends up once that's
   detected (end of compile). Callers must save current_source_cursor()'s return value before
   overriding it with this, then restore it via a second call — this mutates the current File's
   cursor field in place; it does not switch files like lexer_save_state/lexer_restore_state do. */
void lexer_set_cursor(const char* pos);

/* Exposed so parser.c can tag each statement's bytecode with its source line (see Chunk.line_mark_offsets, vm.h). */
unsigned int current_source_line();

/* Exposed so a file-based `import` can resolve a sibling .aer path relative to the file currently being lexed. */
const char* current_source_name();

/* Captures everything read_file()/indent tracking need to resume the CURRENT file's lexing where it left off after a nested read_file()+lex()+parse() cycle for an imported file completes; opaque outside lexer.c — only ever saved and restored, never inspected. */
typedef struct LexerState LexerState;
LexerState* lexer_save_state(void);
void        lexer_restore_state(LexerState* state);

#endif
