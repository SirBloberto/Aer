#include "terminal.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN   /* reduce windows.h's macro surface / collision risk */
    #include <windows.h>
    #include <conio.h>
#else
    #include <termios.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
#endif
#include "error.h"

/* ANSI / VT100 control character codes */
#define END_OF_TEXT  3      /* Ctrl-C */
#define END_OF_TRANS 4      /* Ctrl-D */
#define NEW_LINE     10     /* \n     */
#define BACKSPACE    127

#define ESCAPE_SEQUENCE '\x1b'
#define ARROW_UP    'A'     /* Esc[A — POSIX-only; see read_key() */
#define ARROW_DOWN  'B'     /* Esc[B */
#define ARROW_RIGHT 'C'     /* Esc[C */
#define ARROW_LEFT  'D'     /* Esc[D */
#define HOME        'H'     /* Esc[H */
#define END         'F'     /* Esc[F */
#define DELETE_SEQ  '3'     /* Esc[3~ — named DELETE_SEQ, not DELETE: windows.h
                                already defines DELETE as a file-access-rights
                                constant */

/* Cross-platform special-key codes read_key() normalizes both platforms'
   raw input conventions down to — deliberately outside any possible
   char/EOF value, so a literal typed character can never collide with one
   (unlike ARROW_UP/etc. above, which reuse printable-letter byte values
   safely only because they're compared solely against the third byte of
   an already-confirmed POSIX escape sequence, never a top-level key). */
#define KEY_ARROW_UP    1000
#define KEY_ARROW_DOWN  1001
#define KEY_ARROW_RIGHT 1002
#define KEY_ARROW_LEFT  1003
#define KEY_HOME        1004
#define KEY_END         1005
#define KEY_DELETE      1006

#define COMMAND_SIZE   1024
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static FILE*  history;
static char   history_buffer[COMMAND_SIZE];
static unsigned long history_position;
static unsigned long history_length;

#ifdef _WIN32
    static HANDLE hStdin, hStdout;
    static DWORD  original_in_mode, original_out_mode;
#else
    static struct termios original;
#endif
static unsigned short screen_row;
static unsigned short screen_position;
static unsigned short screen_rows;
static unsigned short screen_columns;

static const char* terminal_name;
static const char* prompt     = ">>> ";
static unsigned int prompt_len = 4;

static char* buffer;
static int   position;
static int   length;
static int   buffer_length = 1024;

#ifndef _WIN32
/* Set when SIGWINCH fires; main loop checks and calls refresh(). No Windows
   equivalent signal exists — handle_terminal() polls check_resize() instead. */
    static volatile sig_atomic_t resize_pending = 0;
#endif

static void refresh();
#ifdef _WIN32
    static void check_resize();
#else
    static void handle_resize(int sig);
#endif
static int  read_key(void);
static void clear();
static void reset();
static void die(const char* message);

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void set_terminal_prompt(const char* p) {
    prompt     = p;
    prompt_len = (unsigned int)strlen(p);
}

void start_terminal(char* name) {
    terminal_name = name;

#ifdef _WIN32
    hStdin  = GetStdHandle(STD_INPUT_HANDLE);
    hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(hStdin, &original_in_mode))   die("GetConsoleMode (stdin)");
    if (!GetConsoleMode(hStdout, &original_out_mode)) die("GetConsoleMode (stdout)");
    atexit(end_terminal);

    /* No line buffering | no auto-echo | no Ctrl-C-as-signal handling —
       the direct equivalents of clearing ICANON|ECHO|ISIG in termios. */
    SetConsoleMode(hStdin, original_in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
    /* refresh() already writes plain ANSI/VT100 escapes (cursor movement,
       clear-to-end, hide/show cursor) — Windows 10+ renders them natively
       once this is set, so none of that code needs to change per platform. */
    SetConsoleMode(hStdout, original_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hStdout, &info))
        die("Could not retrieve window size");
    screen_columns = (unsigned short)(info.srWindow.Right - info.srWindow.Left + 1);
#else
    if (tcgetattr(STDIN_FILENO, &original) == -1)
        die("tcgetattr");
    atexit(end_terminal);

    struct termios raw = original;
    /* No echo | character-at-a-time | don't auto-handle Ctrl-C/Z */
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");

    /* Safe signal handler — just sets a flag */
    signal(SIGWINCH, handle_resize);

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        die("Could not retrieve window size");
    screen_columns = ws.ws_col;
#endif

    /* calloc guarantees zero-initialisation so clear() is safe on first call */
    buffer = xcalloc(buffer_length, sizeof(char));

    char filename[strlen(".") + strlen(name) + strlen("_history") + 1];
    snprintf(filename, sizeof(filename), ".%s_history", name);
    if ((history = fopen(filename, "a+")) == NULL) {
        fprintf(stderr, "Cannot open %s history file\n", name);
        exit(1);
    }
    fseek(history, 0, SEEK_END);
    history_length = (unsigned long)ftell(history);
    unsigned int read_size = MIN(history_length, COMMAND_SIZE);
    fseek(history, -(long)read_size, SEEK_END);
    /* Read into a temp buffer, then place each byte at its true logical slot
       (logical_start + k) % COMMAND_SIZE — a plain fread would misalign
       against the modulo-indexed scheme once history exceeds COMMAND_SIZE
       bytes across sessions. */
    char temp[COMMAND_SIZE];
    size_t got = fread(temp, 1, read_size, history);
    read_size = (unsigned int)got;
    unsigned long logical_start = history_length - read_size;
    for (unsigned int k = 0; k < read_size; k++)
        history_buffer[(logical_start + k) % COMMAND_SIZE] = temp[k];
    history_position = history_length;
}

char* handle_terminal() {
    clear();
    refresh();

    while (true) {
        /* Process a pending resize before blocking on input */
#ifdef _WIN32
        check_resize();
#else
        if (resize_pending) {
            resize_pending = 0;
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col != 0)
                screen_columns = ws.ws_col;
            refresh();
        }
#endif

        int key = read_key();  /* int — preserves high-bit chars, EOF, and the
                                   out-of-band KEY_* special-key sentinels */

        if (key == KEY_ARROW_UP) {
            if (history_position < 2) continue;  /* underflow guard */
            reset();
            clear();
            /* Step back past the trailing newline of the previous entry */
            unsigned long pos = history_position - 2;
            unsigned long start = pos;
            while (pos > 0 && history_buffer[pos % COMMAND_SIZE] != '\n')
                pos--;
            /* If we stopped on a newline, move past it */
            unsigned long entry_start = (history_buffer[pos % COMMAND_SIZE] == '\n') ? pos + 1 : pos;
            unsigned long entry_len   = start - entry_start + 1;
            for (unsigned long i = 0; i < entry_len; i++)
                buffer[i] = history_buffer[(entry_start + i) % COMMAND_SIZE];
            position = length = (int)entry_len;
            history_position = entry_start;

        } else if (key == KEY_ARROW_DOWN) {
            if (history_position >= history_length) continue;
            reset();
            clear();
            /* Skip to after the current newline */
            unsigned long pos = history_position;
            while (pos < history_length && history_buffer[pos % COMMAND_SIZE] != '\n')
                pos++;
            pos++;  /* step past the newline */
            unsigned long entry_start = pos;
            while (pos < history_length && history_buffer[pos % COMMAND_SIZE] != '\n')
                pos++;
            unsigned long entry_len = pos - entry_start;
            for (unsigned long i = 0; i < entry_len; i++)
                buffer[i] = history_buffer[(entry_start + i) % COMMAND_SIZE];
            position = length = (int)entry_len;
            history_position = entry_start;

        } else if (key == KEY_ARROW_RIGHT) {
            position = MIN(position + 1, length);
        } else if (key == KEY_ARROW_LEFT) {
            position = MAX(position - 1, 0);
        } else if (key == KEY_HOME) {
            position = 0;
        } else if (key == KEY_END) {
            position = length;
        } else if (key == KEY_DELETE) {
            if (position < length) {
                memmove(&buffer[position], &buffer[position + 1], length - position);
                length--;
            }

        } else if (key == BACKSPACE) {
            if (position == 0) continue;
            memmove(&buffer[position - 1], &buffer[position], length - position);
            position--;
            length--;

        } else if (key == NEW_LINE) {
            buffer[length] = '\n';
            if (length != 0) {
                /* Persist to history file */
                fwrite(buffer, 1, length + 1, history);
                fflush(history);
                /* Append to ring buffer */
                for (int i = 0; i <= length; i++)
                    history_buffer[history_position++ % COMMAND_SIZE] = buffer[i];
                history_length += (unsigned long)(length + 1);
            }
            /* Move cursor to last row of input then advance to next line */
            if (screen_rows > screen_row)
                printf("\x1b[%dB", screen_rows - screen_row);
            printf("\r\n");
            fflush(stdout);
            screen_row = 0;
            return buffer;

        } else if (key == END_OF_TEXT) {   /* Ctrl-C */
            /* Cancels the current line only, like Python's REPL — it does
               NOT exit the shell, which surprises people used to a
               terminal where Ctrl-C kills the process. Ctrl-D (EOF) is
               the actual exit; say so every time, since there's no other
               way to discover it (no exit()/quit() builtin exists). */
            printf("\nKeyboardInterrupt (press Ctrl-D to exit)\n");
            clear();

        } else if (key == END_OF_TRANS) {  /* Ctrl-D */
            printf("\n");
            end_terminal();
            exit(0);

        } else if (key == EOF) {
            /* stdin closed/exhausted (not interactive Ctrl-D — that's
               END_OF_TRANS above). Without this check, EOF falls into the
               "insert character" branch below and loops forever. */
            printf("\n");
            end_terminal();
            exit(0);

        } else if (key >= 0 && key < 256 && !iscntrl(key)) {
            if (length >= buffer_length) {
                buffer_length *= 2;
                buffer = xrealloc(buffer, buffer_length);
            }
            memmove(&buffer[position + 1], &buffer[position], length - position);
            buffer[position++] = (char)key;
            length++;
        }

        refresh();
    }
}

void end_terminal() {
    if (history != NULL) {
        fclose(history);
        history = NULL;
    }
#ifdef _WIN32
    SetConsoleMode(hStdin, original_in_mode);
    SetConsoleMode(hStdout, original_out_mode);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
#endif
}

/* ------------------------------------------------------------------ */
/* Internal                                                             */
/* ------------------------------------------------------------------ */

static void refresh() {
    printf("\x1b[?25l");  /* hide cursor while drawing */

    /* Move cursor back to the start row of our input */
    reset();

    /* Count how many terminal rows the current text occupies and where the
       cursor sits within them, accounting for the prompt width. */
    int text_cols = prompt_len;
    for (int c = text_cols + position; c >= (int)screen_columns; c -= screen_columns)
        screen_row++;
    for (int c = (int)prompt_len + length; c > (int)screen_columns; c -= screen_columns)
        screen_rows++;

    /* Handle the edge case where text fills exactly to the right margin:
       the terminal won't auto-scroll so we emit a newline manually. */
    if (((int)prompt_len + length) % (int)screen_columns == 0
            && position == length
            && (int)prompt_len + position > screen_position) {
        for (int r = 0; r < screen_rows; r++) printf("\x1b[1E");
        printf("\x1b[%dG\n", screen_columns);
        for (int r = 0; r <= screen_rows; r++) printf("\x1b[1F");
    }

    /* Redraw: go to column 0, clear to end of screen, print prompt + buffer */
    printf("\x1b[0G\x1b[0J%s%.*s", prompt, length, buffer);

    if (((int)prompt_len + length) % (int)screen_columns == 0 && position == length)
        printf("\x1b[1E");

    /* Move up from the last row to the cursor row */
    for (int r = screen_rows; r > screen_row; r--)
        printf("\x1b[1F");

    /* Position cursor and show it */
    printf("\x1b[%dG\x1b[?25h",
           ((int)(prompt_len + position) % (int)screen_columns) + 1);

    screen_position = (unsigned short)(prompt_len + position);
    fflush(stdout);  /* ensure all escape sequences reach the terminal */
}

#ifdef _WIN32
/* No SIGWINCH on Windows — polled once per input-loop iteration instead
   (see handle_terminal). Re-queries the console size and only calls
   refresh() if it actually changed since last checked. */
static void check_resize() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hStdout, &info)) return;
    unsigned short cols = (unsigned short)(info.srWindow.Right - info.srWindow.Left + 1);
    if (cols != 0 && cols != screen_columns) {
        screen_columns = cols;
        refresh();
    }
}
#else
/* Signal handler — only sets a flag; async-signal-safe */
static void handle_resize(int sig) {
    (void)sig;
    resize_pending = 1;
}
#endif

/* Resolves one logical keypress, normalizing each platform's raw-input
   convention to a plain character/control code or one of the KEY_* special-
   key sentinels above — the only thing handle_terminal()'s dispatch chain
   ever looks at, so it's identical for both platforms. */
#ifdef _WIN32
static int read_key(void) {
    int c = _getch();
    if (c == '\r') return NEW_LINE;   /* Windows Enter is CR, not LF */
    if (c == '\b') return BACKSPACE;  /* Windows Backspace is BS (0x08), not DEL (127) —
                                          without this, the raw 0x08 byte falls through
                                          to the line-editing loop's default character-
                                          insert path instead of the BACKSPACE branch,
                                          and the key appears to do nothing */
    if (c != 0 && c != 0xE0) return c;
    switch (_getch()) {   /* extended-key scan code */
        case 72: return KEY_ARROW_UP;
        case 80: return KEY_ARROW_DOWN;
        case 77: return KEY_ARROW_RIGHT;
        case 75: return KEY_ARROW_LEFT;
        case 71: return KEY_HOME;
        case 79: return KEY_END;
        case 83: return KEY_DELETE;
        default: return 0;
    }
}
#else
static int read_key(void) {
    int c = getchar();
    if (c != ESCAPE_SEQUENCE) return c;
    if (getchar() != '[') return ESCAPE_SEQUENCE;
    switch (getchar()) {
        case ARROW_UP:    return KEY_ARROW_UP;
        case ARROW_DOWN:  return KEY_ARROW_DOWN;
        case ARROW_RIGHT: return KEY_ARROW_RIGHT;
        case ARROW_LEFT:  return KEY_ARROW_LEFT;
        case HOME:        return KEY_HOME;
        case END:         return KEY_END;
        case DELETE_SEQ:  return (getchar() == '~') ? KEY_DELETE : ESCAPE_SEQUENCE;
        default:          return ESCAPE_SEQUENCE;
    }
}
#endif

static void clear() {
    memset(buffer, 0, (size_t)length + 1);
    position = 0;
    length   = 0;
}

static void reset() {
    for (int r = screen_row; r > 0; r--)
        printf("\x1b[1F");
    screen_row  = 0;
    screen_rows = 0;
}

static void die(const char* message) {
#ifdef _WIN32
    /* perror()/errno describe C-runtime failures, not Win32 API ones (the
       only kind this function is ever called for) — GetLastError() is the
       one that actually has useful content here. Most likely cause: stdin
       isn't a real console (redirected/piped) — GetConsoleMode only works
       on an actual console handle. */
    DWORD err = GetLastError();
    char* msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, (LPSTR)&msg, 0, NULL);
    fprintf(stderr, "%s: %s\n", message, msg ? msg : "(unknown error)");
    if (msg) LocalFree(msg);
#else
    perror(message);
#endif
    exit(1);
}
