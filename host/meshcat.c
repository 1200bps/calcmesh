// Run the Calcmesh link against a Meshtastic node on a local serial port, e.g.
// the Heltec's USB port: the same handshake and tables the Pico firmware uses,
// without the Pico in the loop.
//
//   meshcat [-1] /dev/ttyACM0
//
// -1 exits after the first node/channel dump; otherwise it keeps printing
// node logs and packets until interrupted.
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "link.h"
#include "nodedb.h"

static int fd = -1;
static int done;
static int once;

static void port_write(const uint8_t *d, size_t n, void *ctx) {
    (void)ctx;
    while (n) {
        ssize_t w = write(fd, d, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                struct pollfd p = {fd, POLLOUT, 0};
                poll(&p, 1, 100);
                continue;
            }
            perror("write");
            done = 1;
            return;
        }
        d += w;
        n -= (size_t)w;
    }
}

static uint32_t now_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void on_ready(void) {
    mesh_db_dump();
    fflush(stdout);
    if (once)
        done = 1;
}

static int open_port(const char *path) {
    int f = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (f < 0) {
        perror(path);
        return -1;
    }
    struct termios t;
    if (tcgetattr(f, &t) < 0) {
        perror("tcgetattr");
        close(f);
        return -1;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    // Don't drop DTR on close: on some boards that resets the node, as the
    // Meshtastic Python client also avoids.
    t.c_cflag &= ~HUPCL;
    if (tcsetattr(f, TCSANOW, &t) < 0) {
        perror("tcsetattr");
        close(f);
        return -1;
    }
    tcflush(f, TCIOFLUSH);
    return f;
}

int main(int argc, char **argv) {
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-1") == 0) {
        once = 1;
        argi++;
    }
    if (argi != argc - 1) {
        fprintf(stderr, "usage: %s [-1] /dev/ttyXXX\n", argv[0]);
        return 2;
    }
    fd = open_port(argv[argi]);
    if (fd < 0)
        return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    mesh_link_io_t io = {
        .write = port_write, .now_ms = now_ms, .seed = (uint32_t)getpid()};
    mesh_link_init(&io);
    mesh_link_on_ready(on_ready);
    mesh_link_request_config();

    uint8_t buf[256];
    while (!done) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 10) > 0) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
                mesh_link_rx(buf, (size_t)n);
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "port closed\n");
                break;
            }
        }
        mesh_link_poll();
    }
    close(fd);
    return 0;
}
