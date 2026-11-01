#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define CLOSE closesocket
#define BAD INVALID_SOCKET
#else
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int socket_t;
#define CLOSE close
#define BAD (-1)
#endif
#include "ipc.h"
#include "platform.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CLIENTS 32
struct server;
struct client {
    struct server *server;
    socket_t socket;
    rc_thread thread;
    atomic_int done;
    int used;
};
struct server {
    rc_core *core;
    atomic_int running;
    int write_enabled, multiwrite_enabled;
    struct client clients[CLIENTS];
};
struct request {
    char cmd[24];
    uint64_t id, after;
    unsigned start, count, age;
    uint16_t values[121];
    unsigned nvalues;
};
static void ws(const char **p) {
    while (isspace((unsigned char)**p))
        (*p)++;
}
static int str(const char **p, char *out, size_t cap) {
    ws(p);
    if (*(*p)++ != '"')
        return -1;
    size_t n = 0;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (c < 32 || c == '\\' || n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    if (**p != '"')
        return -1;
    (*p)++;
    out[n] = 0;
    return 0;
}
static int num(const char **p, uint64_t *v) {
    ws(p);
    if (!isdigit((unsigned char)**p))
        return -1;
    *v = 0;
    if (**p == '0' && isdigit((unsigned char)(*p)[1]))
        return -1;
    while (isdigit((unsigned char)**p)) {
        unsigned n = (unsigned)(*(*p)++ - '0');
        if (*v > (UINT64_MAX - n) / 10)
            return -1;
        *v = *v * 10 + n;
    }
    return 0;
}
static int parse(const char *p, struct request *r) {
    memset(r, 0, sizeof(*r));
    r->count = 4;
    ws(&p);
    if (*p++ != '{')
        return -1;
    unsigned seen = 0;
    for (;;) {
        ws(&p);
        if (*p == '}') {
            p++;
            break;
        }
        char key[24];
        if (str(&p, key, sizeof(key)))
            return -1;
        ws(&p);
        if (*p++ != ':')
            return -1;
        unsigned bit = 0;
        uint64_t v = 0;
        if (!strcmp(key, "cmd")) {
            bit = 1;
            if (str(&p, r->cmd, sizeof(r->cmd)))
                return -1;
        } else if (!strcmp(key, "values")) {
            bit = 2;
            ws(&p);
            if (*p++ != '[')
                return -1;
            for (;;) {
                ws(&p);
                if (*p == ']') {
                    p++;
                    break;
                }
                if (r->nvalues >= 121 || num(&p, &v) || v > 65535)
                    return -1;
                r->values[r->nvalues++] = (uint16_t)v;
                ws(&p);
                if (*p == ']') {
                    p++;
                    break;
                }
                if (*p++ != ',')
                    return -1;
                ws(&p);
                if (*p == ']')
                    return -1;
            }
        } else {
            if (num(&p, &v))
                return -1;
            if (!strcmp(key, "id")) {
                bit = 4;
                r->id = v;
            } else if (!strcmp(key, "after")) {
                bit = 8;
                r->after = v;
            } else if (!strcmp(key, "start")) {
                bit = 16;
                if (v > 65535)
                    return -1;
                r->start = (unsigned)v;
            } else if (!strcmp(key, "count")) {
                bit = 32;
                if (v > 121 || v == 0)
                    return -1;
                r->count = (unsigned)v;
            } else if (!strcmp(key, "max_age_ms")) {
                bit = 64;
                if (v > 3600000)
                    return -1;
                r->age = (unsigned)v;
            } else
                return -1;
        }
        if (seen & bit)
            return -1;
        seen |= bit;
        ws(&p);
        if (*p == '}') {
            p++;
            break;
        }
        if (*p++ != ',')
            return -1;
        ws(&p);
        if (*p == '}')
            return -1;
    }
    ws(&p);
    if (*p || !(seen & 1))
        return -1;
    return 0;
}
static int send_all(socket_t s, const char *p) {
    size_t n = strlen(p);
    while (n) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}
static int error(socket_t s, uint64_t id, int code) {
    char b[192];
    snprintf(b, sizeof(b), "{\"id\":%" PRIu64 ",\"ok\":false,\"error\":%d,\"message\":\"%s\"}\n",
             id, code, rc_error_string(code));
    return send_all(s, b);
}
static int snapshot(socket_t s, uint64_t id, const rc_snapshot *v) {
    char b[4096];
    int n = snprintf(
        b, sizeof(b),
        "{\"id\":%" PRIu64 ",\"ok\":true,\"type\":\"snapshot\",\"sequence\":%" PRIu64
        ",\"completed_ns\":%" PRIu64 ",\"request_ns\":%" PRIu64 ",\"transactions\":%" PRIu64
        ",\"failures\":%" PRIu64 ",\"cache_hits\":%" PRIu64 ",\"coalesced\":%" PRIu64
        ",\"queue_rejected\":%" PRIu64 ",\"queued\":%u,\"last_error\":%d,\"registers\":[",
        id, v->sequence, v->completed_ns, v->request_ns, v->transactions, v->failures,
        v->cache_hits, v->coalesced, v->queue_rejected, v->queued, v->last_error);
    for (unsigned i = 0; i < 121; i++) {
        if (v->valid[i])
            n += snprintf(b + n, sizeof(b) - (size_t)n, "%s%u", i ? "," : "", v->registers[i]);
        else
            n += snprintf(b + n, sizeof(b) - (size_t)n, "%snull", i ? "," : "");
    }
    n += snprintf(b + n, sizeof(b) - (size_t)n, "],\"updated_ns\":[");
    for (unsigned i = 0; i < 121; i++)
        n += snprintf(b + n, sizeof(b) - (size_t)n, "%s%" PRIu64, i ? "," : "", v->updated_ns[i]);
    snprintf(b + n, sizeof(b) - (size_t)n, "]}\n");
    return send_all(s, b);
}
struct input {
    char data[4096];
    size_t begin, end;
};
static int read_line(socket_t fd, struct input *in, char *out) {
    for (;;) {
        char *nl = memchr(in->data + in->begin, '\n', in->end - in->begin);
        if (nl) {
            size_t n = (size_t)(nl - (in->data + in->begin));
            memcpy(out, in->data + in->begin, n);
            out[n] = 0;
            in->begin += n + 1;
            return 1;
        }
        if (in->begin) {
            memmove(in->data, in->data + in->begin, in->end - in->begin);
            in->end -= in->begin;
            in->begin = 0;
        }
        if (in->end == sizeof(in->data) - 1)
            return -1;
        int n = recv(fd, in->data + in->end, (int)(sizeof(in->data) - 1 - in->end), 0);
        if (n <= 0)
            return 0;
        in->end += (size_t)n;
    }
}
static int disconnected(socket_t fd) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval zero = {0, 0};
    if (select((int)fd + 1, &set, NULL, NULL, &zero) <= 0)
        return 0;
    char b;
    return recv(fd, &b, 1, MSG_PEEK) <= 0;
}
static void *serve(void *arg) {
    struct client *c = arg;
    char line[4096];
    struct input input = {0};
    while (atomic_load(&c->server->running)) {
        int got = read_line(c->socket, &input, line);
        if (got <= 0) {
            if (got < 0)
                error(c->socket, 0, RC_RANGE);
            break;
        }
        struct request r;
        if (parse(line, &r)) {
            if (error(c->socket, 0, RC_RANGE))
                break;
            continue;
        }
        if (!strcmp(r.cmd, "snapshot") || !strcmp(r.cmd, "info")) {
            rc_snapshot s;
            rc_snapshot_get(c->server->core, &s);
            if (snapshot(c->socket, r.id, &s))
                break;
        } else if (!strcmp(r.cmd, "registers")) {
            rc_snapshot s;
            rc_snapshot_get(c->server->core, &s);
            unsigned model = s.valid[0] ? s.registers[0] : 0;
            rc_model_info m;
            int model_ok = rc_get_model_info((uint16_t)model, &m) == RC_OK;
            char b[32768];
            int n = snprintf(b, sizeof(b),
                             "{\"id\":%" PRIu64
                             ",\"ok\":true,\"model\":%u,\"model_name\":\"%s\",\"registers\":[",
                             r.id, model, model_ok ? m.name : "Unknown");
            for (unsigned i = 0; i < RC_REGISTERS; i++) {
                rc_register_info reg;
                rc_get_register_info((uint16_t)model, (uint16_t)i, &reg);
                n += snprintf(
                    b + n, sizeof(b) - (size_t)n,
                    "%s{\"address\":%u,\"name\":\"%s\",\"unit\":\"%s\",\"known\":%s,\"writable\":%"
                    "s,\"inferred\":%s,\"scale\":%u,\"raw_min\":%u,\"raw_max\":%u,\"value\":",
                    i ? "," : "", i, reg.name, reg.unit ? reg.unit : "",
                    reg.known ? "true" : "false", reg.writable ? "true" : "false",
                    reg.inferred ? "true" : "false", reg.scale, reg.raw_min, reg.raw_max);
                if (s.valid[i])
                    n += snprintf(b + n, sizeof(b) - (size_t)n, "%u}", s.registers[i]);
                else
                    n += snprintf(b + n, sizeof(b) - (size_t)n, "null}");
                if (n < 0 || (size_t)n >= sizeof(b) - 128)
                    break;
            }
            n += snprintf(b + n, sizeof(b) - (size_t)n,
                          "],\"writes_enabled\":%s,\"multiwrite_enabled\":%s}\n",
                          c->server->write_enabled ? "true" : "false",
                          c->server->multiwrite_enabled ? "true" : "false");
            if (n < 0 || (size_t)n >= sizeof(b)) {
                if (error(c->socket, r.id, RC_PROTOCOL))
                    break;
            } else if (send_all(c->socket, b))
                break;
        } else if (!strcmp(r.cmd, "subscribe")) {
            uint64_t after = r.after;
            while (atomic_load(&c->server->running)) {
                rc_snapshot s;
                int e = rc_snapshot_wait(c->server->core, after, 200, &s);
                if (e == RC_TIMEOUT) {
                    if (disconnected(c->socket))
                        break;
                    continue;
                }
                if (e || snapshot(c->socket, r.id, &s))
                    break;
                after = s.sequence;
            }
            break;
        } else if (!strcmp(r.cmd, "read")) {
            uint16_t v[121];
            int e = rc_read(c->server->core, (uint16_t)r.start, (uint16_t)r.count, r.age, v);
            if (e) {
                if (error(c->socket, r.id, e))
                    break;
            } else {
                char b[2048];
                int k = snprintf(b, sizeof(b),
                                 "{\"id\":%" PRIu64 ",\"ok\":true,\"start\":%u,\"values\":[", r.id,
                                 r.start);
                for (unsigned i = 0; i < r.count; i++)
                    k += snprintf(b + k, sizeof(b) - (size_t)k, "%s%u", i ? "," : "", v[i]);
                snprintf(b + k, sizeof(b) - (size_t)k, "]}\n");
                if (send_all(c->socket, b))
                    break;
            }
        } else if (!strcmp(r.cmd, "write")) {
            int e = rc_write(c->server->core, (uint16_t)r.start, (uint16_t)r.nvalues, r.values);
            if (e) {
                if (error(c->socket, r.id, e))
                    break;
            } else {
                char b[128];
                snprintf(b, sizeof(b), "{\"id\":%" PRIu64 ",\"ok\":true,\"acknowledged\":true}\n",
                         r.id);
                if (send_all(c->socket, b))
                    break;
            }
        } else if (error(c->socket, r.id, RC_UNSUPPORTED))
            break;
    }
    /* Socket closure belongs to server: avoid descriptor reuse during shutdown. */
    atomic_store(&c->done, 1);
    return NULL;
}
int rc_ipc_run(rc_core *core, unsigned port, atomic_int *stop, int writes, int multiwrite) {
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w))
        return -1;
#endif
    struct server state = {0};
    state.core = core;
    state.write_enabled = writes;
    state.multiwrite_enabled = multiwrite;
    atomic_init(&state.running, 1);
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == BAD)
        return -1;
    int yes = 1;
#ifndef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#else
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes));
#endif
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 16)) {
        CLOSE(listener);
        return -1;
    }
    while (!atomic_load(stop)) {
        for (int i = 0; i < CLIENTS; i++) {
            struct client *c = &state.clients[i];
            if (c->used && atomic_load(&c->done)) {
                rc_thread_join(c->thread);
                CLOSE(c->socket);
                c->used = 0;
            }
        }
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(listener, &reads);
        struct timeval tv = {0, 200000};
        int ready = select((int)listener + 1, &reads, NULL, NULL, &tv);
        if (ready <= 0)
            continue;
        socket_t fd = accept(listener, NULL, NULL);
        if (fd == BAD)
            continue;
        struct client *c = NULL;
        for (int i = 0; i < CLIENTS; i++)
            if (!state.clients[i].used) {
                c = &state.clients[i];
                break;
            }
        if (!c) {
            CLOSE(fd);
            continue;
        }
#ifdef _WIN32
        DWORD timeout = 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
        struct timeval timeout = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        c->server = &state;
        c->socket = fd;
        c->used = 1;
        atomic_init(&c->done, 0);
        if (rc_thread_start(&c->thread, serve, c)) {
            CLOSE(fd);
            c->used = 0;
        }
    }
    atomic_store(&state.running, 0);
    CLOSE(listener);
    for (int i = 0; i < CLIENTS; i++)
        if (state.clients[i].used) {
            shutdown(state.clients[i].socket, 2);
            rc_thread_join(state.clients[i].thread);
            CLOSE(state.clients[i].socket);
        }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
