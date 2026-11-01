#define _DEFAULT_SOURCE
#include "riden_core.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
struct serial {
    int fd;
    char *path;
    unsigned baud;
};
static int open_port(struct serial *s) {
    int fd = open(s->path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }
#ifdef TIOCEXCL
    if (ioctl(fd, TIOCEXCL) < 0) {
        close(fd);
        return -1;
    }
#endif
    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    speed_t b;
    switch (s->baud) {
    case 9600:
        b = B9600;
        break;
    case 19200:
        b = B19200;
        break;
    case 38400:
        b = B38400;
        break;
    case 57600:
        b = B57600;
        break;
    case 115200:
        b = B115200;
        break;
    default:
        close(fd);
        return -1;
    }
    cfsetispeed(&t, b);
    cfsetospeed(&t, b);
    t.c_cflag = (t.c_cflag & ~(PARENB | CSTOPB | CSIZE | CRTSCTS)) | CS8 | CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    s->fd = fd;
    return 0;
}
static int io(struct serial *s, uint8_t *b, size_t n, unsigned timeout, int writing) {
    if (s->fd < 0 && (!writing || open_port(s) < 0))
        return -1;
    struct pollfd p = {s->fd, writing ? POLLOUT : POLLIN, 0};
    int ready;
    do {
        ready = poll(&p, 1, (int)timeout);
    } while (ready < 0 && errno == EINTR);
    if (!ready)
        return 0;
    if (ready < 0 || p.revents & (POLLERR | POLLHUP | POLLNVAL))
        goto fail;
    ssize_t k = writing ? write(s->fd, b, n) : read(s->fd, b, n);
    if (k < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    if (k <= 0)
        goto fail;
    return (int)k;
fail:
    close(s->fd);
    s->fd = -1;
    return -1;
}
static int sr(void *p, uint8_t *b, size_t n, uint32_t ms) {
    return io(p, b, n, ms, 0);
}
static int sw(void *p, const uint8_t *b, size_t n, uint32_t ms) {
    return io(p, (uint8_t *)b, n, ms, 1);
}
int rc_serial_open(const char *path, unsigned baud, rc_transport *out) {
    if (!path || !out)
        return RC_RANGE;
    struct serial *s = calloc(1, sizeof(*s));
    if (!s)
        return RC_NOMEM;
    s->fd = -1;
    s->path = strdup(path);
    s->baud = baud;
    if (!s->path || open_port(s)) {
        free(s->path);
        free(s);
        return RC_IO;
    }
    out->context = s;
    out->read = sr;
    out->write = sw;
    return 0;
}
void rc_serial_close(rc_transport *t) {
    if (!t || !t->context)
        return;
    struct serial *s = t->context;
    if (s->fd >= 0) {
#ifdef TIOCNXCL
        ioctl(s->fd, TIOCNXCL);
#endif
        close(s->fd);
    }
    free(s->path);
    free(s);
    memset(t, 0, sizeof(*t));
}
