// ******** FEATURE TEST MACROS ********

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

// ******** INCLUDES ********

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

// ******** DEFINES ********
#define EDI_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1F)

enum editorKey {
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

// ******** DATA ********

typedef struct erow {
    int size;
    char* chars;

} erow;

struct editorConfig {
    int cx, cy;
    int row_offset;
    int col_offset;
    int screen_rows;
    int screen_cols;
    int num_rows;
    erow* row;
    struct termios orig_termios;
};

struct editorConfig E;

// ******** TERMINAL ********

void die(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // Flip lflag bits
    //     ECHO is a bit flag 00000001000
    //     ICANON is bit flag 00100000000
    //     ISIG is bit flag   00010000000
    //     IEXTEN is bit flag 10000000000
    //     ~(ECHO | ICANON | ISIG) gives 10011110111
    //     Bitwise AND with c_lflag turns ECHO, ICANON and ISIG flags off
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editorReadKey() {
    // Note: HOME and END keys have multiple escape sequences
    // and need to be handled according.
    // HOME: <esc>[1~, <esc>[7~, <esc>[H, <esc>OH
    //  END: <esc>[4~, <esc>[8~, <esc>[F, <esc>OF
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }

        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            // Capture escape sequences of the form [<Number>~
            // For example: [5~ ==> page up
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }

                if (seq[2] == '~') {
                    switch(seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                // Capture escape sequences of the form [<Letter>
                // For example: [A ==> up arrow
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) {
    // The n method reports the terminal status information, including
    // cursor position (parameter: 6), to standard input. So read from
    // STDIN into a buffer and parse the result
    // Example result: \x1b[10;20R] ==> cursor at row 10, col 20

    char buff[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < (sizeof(buff) - 1)) {
        if (read(STDIN_FILENO, &buff[i], 1) != 1) {
            break;
        }

        if (buff[i] == 'R') {
            break;
        }

        i++;
    }

    buff[i] = '\0';

    if (buff[0] != '\x1b' || buff[1] != '[') {
        return -1;
    }

    if (sscanf(&buff[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int* rows, int* cols) {
    struct winsize ws;

    // ioctl can return erroneous column size value of 0 possible, so check ws_col
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // If ioctl fails, we use the following fall-back method:
        // 1. Place the cursor at the bottom-right corner. Methods C and B (with
        // sufficiently large position parameters, '999')push the cursor right and
        // down, respectively, and do not go past the terminal view.
        // 2. Query the cursor position using the n method. This tells us what the
        // row and col sizes are.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

// ******** ROW OPERATIONS ********

void editorAppendRow(char* s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

    int i = E.num_rows;
    E.row[i].size = len;
    E.row[i].chars = malloc(len + 1);
    memcpy(E.row[i].chars, s, len);
    E.row[i].chars[len] = '\0';
    E.num_rows++;
}

// ******** FILE I/O ********

void editorOpen(char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char* line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    // getline return -1 at EOF
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line_len--;
        }
        editorAppendRow(line, line_len);
    }

    free(line);
    fclose(fp);
}

// ******** APPEND BUFFER ********

struct abuff {
    char* b;
    int len;
};

#define ABUFF_INIT {NULL, 0};

void abuffAppend(struct abuff* ab, const char* s, int len) {
    char* new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abuffFree(struct abuff* ab) {
    free(ab->b);
}

// ******** OUTPUT ********

void editorScroll() {
    if (E.cy < E.row_offset) {
        E.row_offset = E.cy;
    }

    if (E.cy >= E.row_offset + E.screen_rows) {
        E.row_offset = E.cy - E.screen_rows + 1;
    }

    if (E.cx < E.col_offset) {
        E.col_offset = E.cx;
    }

    if (E.cx >= E.col_offset + E.screen_cols) {
        E.col_offset = E.cx - E.screen_cols + 1;
    }
}

void editorDrawRows(struct abuff* ab) {
    for (int y = 0; y < E.screen_rows; y++) {
        int file_row = y + E.row_offset;
        if (file_row >= E.num_rows) {
            // Print welcome message
            if (E.num_rows == 0 && y == E.screen_rows/3) {
                char welcome[80];
                const char* message = "EDItor -- version %s";
                int welcome_len = snprintf(welcome, sizeof(welcome), message, EDI_VERSION);
                // Truncate message if the terminal view is too small
                if (welcome_len > E.screen_cols) {
                    welcome_len = E.screen_cols;
                }
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding) {
                    abuffAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    abuffAppend(ab, " ", 1);
                }
                abuffAppend(ab, welcome, welcome_len);
            } else {
                abuffAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[file_row].size - E.col_offset;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screen_cols) {
                len = E.screen_cols;
            }
            abuffAppend(ab, &E.row[file_row].chars[E.col_offset], len);
        }

        // Write a 3-byte escape sequence to the terminal to clear the screen.
        // The first byte is \x1b is the escape character (decimal 27),
        // followed by [K, the next two bytes.
        // The K escape sequence command takes a parameter, 2, which clears the
        // entire line. [0K is the default argument and clears the line to
        // the right of the cursor. [1K clears the line to the left of the cursor.
        abuffAppend(ab, "\x1b[K", 3); // K: Erase in line

        if (y < E.screen_rows - 1) {
            abuffAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuff ab = ABUFF_INIT;

    // l and h commands (Reset Mode, Set Mode) are used to enable/disable
    // various terminal features.
    abuffAppend(&ab, "\x1b[?25l", 6); // Hide cursor

    // H takes 2 parameters (row and col numbers). Default arguments are 1
    // and 1, which places the cursor at the top of the screen.
    abuffAppend(&ab, "\x1b[H", 3);  // H: Cursor Position

    editorDrawRows(&ab);

    // Create a H command escape sequence to place the cursor at
    // the desired location stored in the editorConfig, using the
    // snprintf function to append to \xb[%d;%d ==> \xb[10;16 (for example)
    char buff[32];
    snprintf(buff, sizeof(buff), "\x1b[%d;%dH", (E.cy - E.row_offset) + 1, (E.cx - E.col_offset) + 1);
    abuffAppend(&ab, buff, strlen(buff));

    abuffAppend(&ab, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abuffFree(&ab);
}

// ******** INPUT ********

void editorMoveCursor(int key) {
    erow* row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.num_rows) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
    int row_len = row ? row->size : 0;
    if (E.cx > row_len) {
        E.cx = row_len;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screen_cols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { // Creating a code block, {...}, to allow declaration of rows variable
                int rows = E.screen_rows;
                while (rows--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

// ******** INIT ********

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.row_offset = 0;
    E.col_offset = 0;
    E.num_rows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("getWindowSize");
    }
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}