// Run the Calcmesh link against a Meshtastic node on a local serial port, e.g.
// the Heltec's USB port or the PicoCalc's node port in desktop mode: the same
// handshake, tables and messaging the Pico firmware uses, without the Pico in
// the loop.
//
//   meshcat [-1] [-x COMMAND]... /dev/ttyACM0
//
// Once the handshake is done, lines typed on stdin are console commands, the
// same ones the firmware takes (`help` lists them): `send 0 hello`,
// `dm !a1b2c3d4 hi`, `msgs`. Received messages print as they arrive.
//
// -x runs a command after the first handshake (repeatable). -1 exits after
// the first dump, or with -x, once every message sent has been acked or
// has failed (or after a minute), with status 1 if any failed.
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "link.h"
#include "msgs.h"
#include "nodedb.h"

static int fd = -1;
static int done;
static int once;
static int ready;
static const char *cmds[16];
static int ncmds;

#define ONCE_TIMEOUT_MS 60000

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

static void run(const char *cmd) {
    char line[512];
    snprintf(line, sizeof line, "%s", cmd);
    if (!mesh_cmd_run(line)) {
        if (strcmp(line, "help") != 0)
            printf("unknown command '%s'\n", line);
        mesh_cmd_help();
    }
}

static void on_ready(void) {
    mesh_db_dump();
    if (!ready) {
        for (int i = 0; i < ncmds; i++)
            run(cmds[i]);
        printf("ready; type `help` for commands\n");
    }
    ready = 1;
    fflush(stdout);
    if (once && !ncmds)
        done = 1;
}

// For -1 -x: 0 while anything we sent is still waiting, else 1 + failures.
static int sends_settled(void) {
    int failed = 0;
    for (int c = 0; c < mesh_msgs_conv_count(); c++) {
        mesh_conv_t conv = mesh_msgs_conv_at(c)->conv;
        for (int i = 0; i < mesh_msgs_count(conv); i++) {
            const mesh_msg_t *m = mesh_msgs_recent(conv, i);
            if (m->status == MESH_MSG_QUEUED || m->status == MESH_MSG_SENT)
                return 0;
            failed += m->status == MESH_MSG_FAILED;
        }
    }
    return 1 + failed;
}

// Collect stdin into lines and run them once the link is up.
static void stdin_task(int *open_) {
    static char line[512];
    static size_t len;
    char buf[256];
    ssize_t n = read(0, buf, sizeof buf);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR))
            *open_ = 0;
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            line[len] = '\0';
            len = 0;
            if (!line[0])
                continue;
            if (ready)
                run(line);
            else
                printf("not connected yet; try again in a moment\n");
        } else if (len < sizeof line - 1) {
            line[len++] = buf[i];
        }
    }
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
    for (; argi < argc - 1; argi++) {
        if (strcmp(argv[argi], "-1") == 0) {
            once = 1;
        } else if (strcmp(argv[argi], "-x") == 0 && argi + 1 < argc - 1 &&
                   ncmds < (int)(sizeof cmds / sizeof *cmds)) {
            cmds[ncmds++] = argv[++argi];
        } else {
            break;
        }
    }
    if (argi != argc - 1) {
        fprintf(stderr, "usage: %s [-1] [-x COMMAND]... /dev/ttyXXX\n", argv[0]);
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

    int stdin_open = 1, status = 0;
    uint32_t ready_ms = 0;
    uint8_t buf[256];
    while (!done) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {0, POLLIN, 0}};
        if (poll(p, stdin_open ? 2 : 1, 10) > 0) {
            if (p[0].revents) {
                ssize_t n = read(fd, buf, sizeof buf);
                if (n > 0) {
                    mesh_link_rx(buf, (size_t)n);
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    fprintf(stderr, "port closed\n");
                    status = 1;
                    break;
                }
            }
            if (stdin_open && p[1].revents)
                stdin_task(&stdin_open);
        }
        mesh_link_poll();

        if (once && ncmds && ready) {
            if (!ready_ms)
                ready_ms = now_ms(NULL) | 1;
            int settled = sends_settled();
            if (settled) {
                status = settled > 1;
                done = 1;
            } else if (now_ms(NULL) - ready_ms > ONCE_TIMEOUT_MS) {
                printf("gave up waiting for acks\n");
                status = 1;
                done = 1;
            }
        }
    }
    close(fd);
    return status;
}
