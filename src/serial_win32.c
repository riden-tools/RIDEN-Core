#include "riden_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
struct serial {
    HANDLE handle;
    char path[256];
    unsigned baud;
};
static int open_port(struct serial *s) {
    s->handle = CreateFileA(s->path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s->handle == INVALID_HANDLE_VALUE)
        return -1;
    DCB d = {0};
    d.DCBlength = sizeof(d);
    if (!GetCommState(s->handle, &d))
        goto fail;
    d.BaudRate = s->baud;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fParity = FALSE;
    d.fOutxCtsFlow = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fDsrSensitivity = FALSE;
    d.fOutX = FALSE;
    d.fInX = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE;
    d.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(s->handle, &d))
        goto fail;
    SetupComm(s->handle, 4096, 4096);
    PurgeComm(s->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 0;
fail:
    CloseHandle(s->handle);
    s->handle = INVALID_HANDLE_VALUE;
    return -1;
}
static int io(struct serial *s, uint8_t *b, size_t n, unsigned ms, int writing) {
    if (s->handle == INVALID_HANDLE_VALUE && (!writing || open_port(s)))
        return -1;
    COMMTIMEOUTS tm = {0};
    tm.ReadIntervalTimeout = MAXDWORD;
    tm.ReadTotalTimeoutMultiplier = MAXDWORD;
    tm.ReadTotalTimeoutConstant = ms;
    tm.WriteTotalTimeoutConstant = ms;
    if (!SetCommTimeouts(s->handle, &tm))
        return -1;
    DWORD got = 0;
    BOOL ok = writing ? WriteFile(s->handle, b, (DWORD)n, &got, NULL)
                      : ReadFile(s->handle, b, (DWORD)n, &got, NULL);
    if (!ok) {
        CloseHandle(s->handle);
        s->handle = INVALID_HANDLE_VALUE;
        return -1;
    }
    return (int)got;
}
static int sr(void *p, uint8_t *b, size_t n, uint32_t ms) {
    return io(p, b, n, ms, 0);
}
static int sw(void *p, const uint8_t *b, size_t n, uint32_t ms) {
    return io(p, (uint8_t *)b, n, ms, 1);
}
int rc_serial_open(const char *path, unsigned baud, rc_transport *out) {
    if (!path || !out || strlen(path) > 240)
        return RC_RANGE;
    struct serial *s = calloc(1, sizeof(*s));
    if (!s)
        return RC_NOMEM;
    s->handle = INVALID_HANDLE_VALUE;
    s->baud = baud;
    if (strncmp(path, "\\\\.\\", 4) == 0)
        snprintf(s->path, sizeof(s->path), "%s", path);
    else
        snprintf(s->path, sizeof(s->path), "\\\\.\\%s", path);
    if (open_port(s)) {
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
    if (s->handle != INVALID_HANDLE_VALUE)
        CloseHandle(s->handle);
    free(s);
    memset(t, 0, sizeof(*t));
}
