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
#define DELETE_SEQ  '3'     /* named DELETE_SEQ: windows.h already defines DELETE */

/* read_key()'s normalized special-key codes — outside any char/EOF value so a typed character
   can never collide with one. */
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
/* True when stdin isn't a real console (redirected from a file/pipe) -- raw-mode editing and
   history are meaningless without a terminal to render them on, so handle_terminal() falls back
   to plain line reads instead (see handle_terminal_piped). */
static bool piped_stdin = false;
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
/* Set by SIGWINCH; Windows has no equivalent signal, so handle_terminal polls check_resize(). */
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
    /* A redirected/piped stdin is a valid handle but not a console, so GetConsoleMode fails --
       that used to hit die() unconditionally, crashing on `aer.exe < script.txt`. Same fallback
       applied on POSIX (isatty) below in case the equivalent gap exists there too, just
       unexercised. Raw mode, history, and screen-size queries are all meaningless without a real
       console to render them on, so the piped path skips straight to allocating the line buffer
       handle_terminal_piped() needs and returns. */
    piped_stdin = !GetConsoleMode(hStdin, &original_in_mode);
#else
    piped_stdin = !isatty(STDIN_FILENO);
#endif
    if (piped_stdin) {
        buffer = xcalloc(buffer_length, sizeof(char));
        return;
    }

#ifdef _WIN32
    if (!GetConsoleMode(hStdout, &original_out_mode)) die("GetConsoleMode (stdout)");
    atexit(end_terminal);

    /* Equivalent of clearing ICANON|ECHO|ISIG in termios. */
    SetConsoleMode(hStdin, original_in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
    /* Lets Windows 10+ render the ANSI escapes refresh() writes, no per-platform output code. */
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
    /* Place each byte at (logical_start + k) % COMMAND_SIZE — a plain fread misaligns once
       history exceeds COMMAND_SIZE bytes across sessions. */
    char temp[COMMAND_SIZE];
    size_t got = fread(temp, 1, read_size, history);
    read_size = (unsigned int)got;
    unsigned long logical_start = history_length - read_size;
    for (unsigned int k = 0; k < read_size; k++)
        history_buffer[(logical_start + k) % COMMAND_SIZE] = temp[k];
    history_position = history_length;
}

/* Plain line reads, no raw-mode editing/history -- used when stdin is redirected from a file or
   pipe (see start_terminal). Mirrors the interactive loop's own EOF contract (end_terminal() +
   exit(0)) rather than returning NULL, so run_shell()'s existing NULL-means-Ctrl-C handling in
   main.c needs no changes for this path. A final line with no trailing newline is still handed
   over as one real line; only a completely empty read at EOF exits. */
static char* handle_terminal_piped(void) {
    length = 0;
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            if (length == 0) { end_terminal(); exit(0); }
            break;
        }
        if (length + 2 > buffer_length) {
            buffer_length *= 2;
            buffer = xrealloc(buffer, buffer_length);
        }
        buffer[length++] = (char)ch;
        if (ch == '\n') break;
    }
    buffer[length] = '\0';
    return buffer;
}

char* handle_terminal() {
    if (piped_stdin) return handle_terminal_piped();

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

        int key = read_key();  /* int: preserves high-bit chars, EOF, and KEY_* sentinels */

        if (key == KEY_ARROW_UP) {
            if (history_position < 2) continue;  /* underflow guard */
            reset();
            clear();
            /* Bytes before this floor were overwritten by ring wraparound — scanning past it aliases. */
            unsigned long floor_pos = (history_length > COMMAND_SIZE) ? (history_length - COMMAND_SIZE) : 0;
            /* Step back past the trailing newline of the previous entry */
            unsigned long pos = history_position - 2;
            unsigned long start = pos;
            while (pos > floor_pos && history_buffer[pos % COMMAND_SIZE] != '\n')
                pos--;
            /* If we stopped on a newline, move past it */
            unsigned long entry_start = (pos > floor_pos && history_buffer[pos % COMMAND_SIZE] == '\n') ? pos + 1 : pos;
            unsigned long entry_len   = start - entry_start + 1;
            if (entry_len > (unsigned long)(buffer_length - 1)) entry_len = (unsigned long)(buffer_length - 1);
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
            if (entry_len > (unsigned long)(buffer_length - 1)) entry_len = (unsigned long)(buffer_length - 1);
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
            /* length can equal buffer_length exactly — reserve room for newline AND NUL before writing. */
            if (length + 2 > buffer_length) {
                buffer_length *= 2;
                buffer = xrealloc(buffer, buffer_length);
            }
            buffer[length] = '\n';
            /* Backspace leaves stale bytes past `length` — NUL-terminate or strlen reads a longer edit's tail. */
            buffer[length + 1] = '\0';
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
            /* Ctrl-C cancels the whole in-progress input (including a multi-line block), not the shell;
               Ctrl-D exits — say so, there's no other way to discover it. */
            if (screen_rows > screen_row)
                printf("\x1b[%dB", screen_rows - screen_row);
            printf("\nKeyboardInterrupt (press Ctrl-D to exit)\n");
            clear();
            screen_row = 0;
            return NULL;

        } else if (key == END_OF_TRANS) {  /* Ctrl-D */
            printf("\n");
            end_terminal();
            exit(0);

        } else if (key == EOF) {
            /* stdin closed/exhausted — without this, EOF loops forever in the insert branch. */
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
    if (piped_stdin) return;   /* raw mode was never entered -- nothing to restore */
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

    /* Rows occupied by the current text and where the cursor sits, prompt width included. */
    int text_cols = prompt_len;
    for (int c = text_cols + position; c >= (int)screen_columns; c -= screen_columns)
        screen_row++;
    for (int c = (int)prompt_len + length; c > (int)screen_columns; c -= screen_columns)
        screen_rows++;

    /* Text ending exactly at the right margin doesn't auto-scroll — emit the newline manually. */
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
/* Polled resize check for Windows (no SIGWINCH); refresh() only on an actual change. */
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

/* Normalizes each platform's raw input to a char/control code or a KEY_* sentinel. */
#ifdef _WIN32
static int read_key(void) {
    int c = _getch();
    if (c == '\r') return NEW_LINE;   /* Windows Enter is CR, not LF */
    if (c == '\b') return BACKSPACE;  /* Windows Backspace is BS (0x08), not DEL (127) */
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
    /* GetLastError(), not errno — Win32 failures; likely cause is a redirected (non-console) stdin. */
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
