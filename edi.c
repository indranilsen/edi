// ******** INCLUDES ********

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// ******** DEFINES ********

#define CTRL_KEY(k) ((k) & 0x1F)

// ******** DATA ********

struct editorConfig {
    int screen_rows;
    int screen_cols;
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

char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    return c;
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

// ******** OUTPUT ********

void editorDrawRows() {
    for (int y = 0; y < E.screen_rows; y++) {
        write(STDOUT_FILENO, "~", 1);

        if (y < E.screen_rows - 1) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    // Write a 4-byte escape sequence to the terminal to clear the screen.
    // The first byte is \x1b is the escape character (decimal 27),
    // followed by [2J, the next three bytes.
    // The J escape sequence command takes a parameter, 2, which clears the
    // entire screen. [0J is the default argument and clears the screen from
    // the cursor to the end. [1J clears the screen up to the cursor.
    write(STDOUT_FILENO, "\x1b[2J", 4); // J: Erase In Display

    // H takes 2 parameters (row and col numbers). Default arguments are 1
    // and 1, which places the cursor at the top of the screen.
    write(STDOUT_FILENO, "\x1b[H", 3);  // H: Cursor Position

    editorDrawRows();

    write(STDOUT_FILENO, "\x1b[H", 3);
}

// ******** INPUT ********

void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

// ******** INIT ********

void initEditor() {
    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("getWindowSize");
    }
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}