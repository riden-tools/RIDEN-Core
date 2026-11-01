#include "internal.h"
#include <string.h>
uint16_t rc_crc(const uint8_t *p, size_t n) {
    uint16_t c = 0xffff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (uint16_t)((c >> 1) ^ ((c & 1) ? 0xa001 : 0));
    }
    return c;
}
static unsigned remaining(uint64_t end) {
    uint64_t n = rc_monotonic_ns();
    return n >= end ? 0 : (unsigned)((end - n + 999999) / 1000000);
}
static int get(const rc_transport *t, uint8_t *b, size_t n, uint64_t end, uint64_t *last) {
    size_t done = 0;
    while (done < n) {
        unsigned ms = remaining(end);
        if (!ms)
            return RC_TIMEOUT;
        int k = t->read(t->context, b + done, n - done, ms);
        if (k < 0)
            return RC_IO;
        if (k == 0)
            return RC_TIMEOUT;
        if ((size_t)k > n - done)
            return RC_PROTOCOL;
        done += (size_t)k;
        *last = rc_monotonic_ns();
    }
    return RC_OK;
}
int rc_exchange(const rc_transport *t, const rc_config *cfg, uint64_t *last, uint8_t fn,
                uint16_t start, uint16_t count, uint16_t *v, uint64_t *sent, uint64_t *completed) {
    uint8_t req[256], resp[256];
    size_t len = 6;
    uint64_t gap = (uint64_t)cfg->frame_gap_us * 1000ULL, now = rc_monotonic_ns();
    if (*last && now - *last < gap)
        rc_sleep_ns(gap - (now - *last));
    req[0] = cfg->address;
    req[1] = fn;
    req[2] = (uint8_t)(start >> 8);
    req[3] = (uint8_t)start;
    if (fn == 6) {
        req[4] = (uint8_t)(v[0] >> 8);
        req[5] = (uint8_t)v[0];
    } else {
        req[4] = (uint8_t)(count >> 8);
        req[5] = (uint8_t)count;
        if (fn == 16) {
            req[len++] = (uint8_t)(count * 2);
            for (unsigned i = 0; i < count; i++) {
                req[len++] = (uint8_t)(v[i] >> 8);
                req[len++] = (uint8_t)v[i];
            }
        }
    }
    uint16_t crc = rc_crc(req, len);
    req[len++] = (uint8_t)crc;
    req[len++] = (uint8_t)(crc >> 8);
    *sent = rc_monotonic_ns();
    uint64_t end = *sent + (uint64_t)cfg->timeout_ms * 1000000ULL;
    for (size_t n = 0; n < len;) {
        unsigned ms = remaining(end);
        if (!ms)
            return RC_TIMEOUT;
        int k = t->write(t->context, req + n, len - n, ms);
        if (k <= 0 || (size_t)k > len - n)
            return RC_IO;
        n += (size_t)k;
        *last = rc_monotonic_ns();
    }
    int e = get(t, resp, 3, end, last);
    if (e)
        return e;
    size_t expected = (resp[1] & 128) ? 5 : (fn == 3 ? (size_t)5 + resp[2] : 8);
    if (expected > sizeof(resp))
        return RC_PROTOCOL;
    e = get(t, resp + 3, expected - 3, end, last);
    if (e)
        return e;
    *completed = rc_monotonic_ns();
    crc = rc_crc(resp, expected - 2);
    if (resp[expected - 2] != (uint8_t)crc || resp[expected - 1] != (uint8_t)(crc >> 8))
        return RC_CRC;
    if (resp[0] != cfg->address)
        return RC_PROTOCOL;
    if (resp[1] == (uint8_t)(fn | 128))
        return RC_UNSUPPORTED;
    if (resp[1] != fn)
        return RC_PROTOCOL;
    if (fn == 3) {
        if (resp[2] != count * 2)
            return RC_PROTOCOL;
        for (unsigned i = 0; i < count; i++)
            v[i] = (uint16_t)((resp[3 + i * 2] << 8) | resp[4 + i * 2]);
    } else if (memcmp(req, resp, fn == 6 ? 6 : 6))
        return RC_PROTOCOL;
    return RC_OK;
}
