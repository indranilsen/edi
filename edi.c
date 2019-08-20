// ******** FEATURE TEST MACROS ********

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

// ******** INCLUDES ********

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ******** DEFINES ********
#define EDI_VERSION "0.0.1"
#define EDI_TAB_STOP 8
#define EDI_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1F)

enum editorKey {
    BACKSPACE = 127,
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
    int rsize;
    char* chars;
    char* render;
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int row_offset;
    int col_offset;
    int screen_rows;
    int screen_cols;
    int num_rows;
    erow* row;
    int dirty;
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

// ******** PROTOTYPES ********

void editorSetStatusMessage(const char* fmt, ...);

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

int editorRowCxToRx(erow* row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (EDI_TAB_STOP - 1) - (rx % EDI_TAB_STOP);
        }
        rx++;
    }

    return rx;
}

void editorUpdateRow(erow* row) {
    int j = 0;
    int tabs = 0;

    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + (tabs * (EDI_TAB_STOP - 1)) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while ( idx % EDI_TAB_STOP != 0 ) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char* s, size_t len) {
    if (at < 0 || at > E.num_rows) {
        return;
    }

    E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.num_rows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.num_rows++;
    E.dirty++;
}

void editorFreeRow(erow* row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.num_rows) {
        return;
    }
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.num_rows - at - 1));
    E.num_rows--;
    E.dirty++;
}

void editorRowInsertChar(erow* row, int at, int c) {
    // Validate insertion index. It can go one character past the
    // end of the string, in which case 'c' is appended at the end
    // of the string.
    if (at < 0 || at > row->size) {
        at = row->size;
    }

    // Allocate an extra byte for the new character + one byte for NULL byte
    row->chars = realloc(row->chars, row->size + 2);

    // "Split" the contiguous block so there is space for one character between them
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);

    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow* row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow* row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

// ******** EDITOR OPERATIONS ********

void editorInsertChar(int c) {
    // If the cursor is at the tilde line at the EOF, then append a new row to the file
    // before inserting a character.
    if (E.cy == E.num_rows) {
        editorInsertRow(E.num_rows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow* row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);

        // Reassign the row pointer in case the realloc() in editorInsertRow() invalidates the pointer
        row = &E.row[E.cy];

        // Truncate the current row and call editorUpdateRow() on it
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    // If the cursor is past the end of the file, there is nothing to delete
    if (E.cy == E.num_rows) {
        return;
    }

    // If the cursor is at the beginning of the file, there is nothing to delete
    if (E.cx == 0 && E.cy == 0) {
        return;
    }

    erow* row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        // This is the special case where the beginning of a line is deleted
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

// ******** FILE I/O ********

char* editorRowsToString(int* buff_len) {
    int total_len = 0;
    int j;
    for (j = 0; j < E.num_rows; j++) {
        // Plus 1 for the newline character
        total_len += E.row[j].size + 1;
    }

    *buff_len = total_len;

    char* buff = malloc(total_len);
    char* ptr = buff;
    for (j = 0; j < E.num_rows; j++) {
        memcpy(ptr, E.row[j].chars, E.row[j].size);
        ptr += E.row[j].size;
        *ptr = '\n';
        ptr++;
    }

    return buff;
}

void editorOpen(char* filename) {
    free(E.filename);
    E.filename = strdup(filename);

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
        editorInsertRow(E.num_rows, line, line_len);
    }

    free(line);
    fclose(fp);

    // editorAppendRow() above increments the dirty bit so clear it
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        return;
    }

    int len;
    char* buff = editorRowsToString(&len);

    // O_CREAT flag creates the file if it does not exist
    // O_CREAT requires an extra argument 0644 which is the
    // standard permission for text files, giving the owner
    // read/write permissions; all other users get read permissions.
    // O_RDWR flag opens the file to read and write
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    if (fd != -1) {
        // Set the file's size to the specified size, cutting off data
        // that is larger than 'len'
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buff, len) == len) {
                close(fd);
                free(buff);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buff);
    editorSetStatusMessage("Could not save. I/O errors: %s", strerror(errno));
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
    E.rx = 0;
    if (E.cy < E.num_rows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.row_offset) {
        E.row_offset = E.cy;
    }

    if (E.cy >= E.row_offset + E.screen_rows) {
        E.row_offset = E.cy - E.screen_rows + 1;
    }

    if (E.rx < E.col_offset) {
        E.col_offset = E.rx;
    }

    if (E.rx >= E.col_offset + E.screen_cols) {
        E.col_offset = E.rx - E.screen_cols + 1;
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
            int len = E.row[file_row].rsize - E.col_offset;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screen_cols) {
                len = E.screen_cols;
            }
            abuffAppend(ab, &E.row[file_row].render[E.col_offset], len);
        }

        // Write a 3-byte escape sequence to the terminal to clear the screen.
        // The first byte is \x1b is the escape character (decimal 27),
        // followed by [K, the next two bytes.
        // The K escape sequence command takes a parameter, 2, which clears the
        // entire line. [0K is the default argument and clears the line to
        // the right of the cursor. [1K clears the line to the left of the cursor.
        abuffAppend(ab, "\x1b[K", 3); // K: Erase in line

        abuffAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuff* ab) {
    // m command: Select Graphic Rendition
    abuffAppend(ab, "\x1b[7m", 4); // Switch to inverted terminal colors
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
            E.filename ? E.filename : "[No Name]",
            E.num_rows,
            E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.num_rows);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }
    abuffAppend(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            abuffAppend(ab, rstatus, rlen);
            break;
        } else {
            abuffAppend(ab, " ", 1);
            len++;
        }
    }
    abuffAppend(ab, "\x1b[m", 3); // Switch to normal terminal colors
    abuffAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuff* ab) {
    abuffAppend(ab, "\x1b[K", 3);
    int msg_len = strlen(E.statusmsg);
    if (msg_len > E.screen_cols) {
        msg_len = E.screen_cols;
    }
    if (msg_len && time(NULL) - E.statusmsg_time < 5) {
        abuffAppend(ab, E.statusmsg, msg_len);
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
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // Create a H command escape sequence to place the cursor at
    // the desired location stored in the editorConfig, using the
    // snprintf function to append to \xb[%d;%d ==> \xb[10;16 (for example)
    char buff[32];
    snprintf(buff, sizeof(buff), "\x1b[%d;%dH", (E.cy - E.row_offset) + 1, (E.rx - E.col_offset) + 1);
    abuffAppend(&ab, buff, strlen(buff));

    abuffAppend(&ab, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abuffFree(&ab);
}

void editorSetStatusMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
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
    static int quit_times = EDI_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING! File has unsaved changes. "
                                       "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.num_rows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) {
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { // Creating a code block, {...}, to allow declaration of rows variable
                if (c == PAGE_UP) {
                    E.cy = E.row_offset;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.row_offset + E.screen_rows - 1;
                    if (E.cy > E.num_rows) {
                        E.cy = E.num_rows;
                    }
                }

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

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = EDI_QUIT_TIMES;
}

// ******** INIT ********

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.row_offset = 0;
    E.col_offset = 0;
    E.num_rows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("getWindowSize");
    }

    // Use the last 2 rows for the status bar and the message bar
    E.screen_rows -= 2;
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}