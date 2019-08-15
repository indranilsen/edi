#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#define QUITCHR 'q'

struct termios orig_termios;

void die(const char* s) {
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = orig_termios;

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

int main() {
    enableRawMode();

    while (1) {
        char c = '\0';

        // read() timeout in Cygwin return -1 (instead of 0, as it is supposed to)
        // and sets errno to EAGAIN, so don't treat EAGAIN as error to make it work
        // in Cygwin
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
            die("read");
        }

        if (iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            printf("%d ('%c')\r\n", c, c);
        }
        if (c == QUITCHR) {
            break;
        }
    }

    return 0;
}