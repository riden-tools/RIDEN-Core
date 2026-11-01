#include "internal.h"
#include <stdlib.h>
#include <string.h>
#define QUEUE_LIMIT 32
struct task {
    struct task *next;
    uint8_t fn;
    uint16_t start, count, values[RC_REGISTERS];
    uint64_t created;
    unsigned refs;
    int done, error;
};
struct rc_core {
    rc_transport transport;
    rc_config config;
    rc_mutex mutex;
    rc_cond changed;
    rc_thread worker;
    int stop;
    struct task *reads, *writes, *active;
    rc_snapshot cache;
    int hot_active;
    uint16_t hot_start, hot_count;
    uint64_t hot_generation;
};
rc_config rc_default_config(void) {
    rc_config c = {0};
    c.abi_version = RC_ABI_VERSION;
    c.address = 1;
    c.poll_start = 10;
    c.poll_count = 4;
    c.timeout_ms = 300;
    c.frame_gap_us = 1750;
    return c;
}
static int builtin_profile(uint16_t model, rc_model_info *out) {
    static const rc_model_info rd6006 = {0, "RD6006", 100, 1000, 61, 6.1, 62, 6.2};
    static const rc_model_info rd6012 = {0, "RD6012", 100, 100, 61, 12.1, 62, 12.2};
    static const rc_model_info rd6018 = {0, "RD6018", 100, 100, 61, 18.1, 62, 18.2};
    static const rc_model_info rd6024 = {0, "RD6024", 100, 100, 61, 24.1, 62, 24.2};
    static const rc_model_info rd6030 = {0, "RD6030", 100, 100, 61, 30.1, 62, 30.2};
    static const rc_model_info rk8009 = {0, "RK8009", 100, 1000, 81, 9.1, 82, 9.2};
    rc_model_info p;
    if (model == 60065) {
        p = rd6006;
        p.name = "RD6006P";
        p.voltage_scale = 1000;
        p.current_scale = 10000;
    } else if (model == 60066) {
        p = rd6006;
        p.name = "RK6006";
    } else if (model == 6006 || (model >= 60060 && model <= 60064) || model == 60067) {
        p = rd6006;
    } else if (model == 6012 || (model >= 60120 && model <= 60124)) {
        p = rd6012;
    } else if (model == 6018 || (model >= 60180 && model <= 60189)) {
        p = rd6018;
    } else if (model == 6024 || (model >= 60241 && model <= 60249)) {
        p = rd6024;
    } else if (model == 6030 || (model >= 60300 && model <= 60309)) {
        p = rd6030;
    } else if (model == 8009) {
        p = rk8009;
    } else {
        return RC_UNSUPPORTED;
    }
    p.model = model;
    if (out)
        *out = p;
    return RC_OK;
}
/* User-registered profiles only cover IDs without a built-in profile. */
#define CUSTOM_MODELS 8
static rc_model_info custom_models[CUSTOM_MODELS];
static unsigned custom_count;
static int model_profile(uint16_t model, rc_model_info *out) {
    if (builtin_profile(model, out) == RC_OK)
        return RC_OK;
    for (unsigned i = 0; i < custom_count; i++)
        if (custom_models[i].model == model) {
            if (out)
                *out = custom_models[i];
            return RC_OK;
        }
    return RC_UNSUPPORTED;
}
int rc_get_model_info(uint16_t model, rc_model_info *info) {
    return model_profile(model, info);
}
static int valid_scale(uint16_t s) {
    return s == 10 || s == 100 || s == 1000 || s == 10000;
}
static int fits(double limit, uint16_t scale) {
    return limit > 0 && limit * scale < 65535.5;
}
int rc_register_model(const rc_model_info *info) {
    if (!info || !valid_scale(info->voltage_scale) || !valid_scale(info->current_scale) ||
        !fits(info->max_voltage, info->voltage_scale) || !fits(info->max_ovp, info->voltage_scale) ||
        !fits(info->max_current, info->current_scale) || !fits(info->max_ocp, info->current_scale))
        return RC_RANGE;
    if (builtin_profile(info->model, NULL) == RC_OK)
        return RC_BUSY;
    unsigned i = 0;
    while (i < custom_count && custom_models[i].model != info->model)
        i++;
    if (i == CUSTOM_MODELS)
        return RC_QUEUE_FULL;
    custom_models[i] = *info;
    custom_models[i].name = "Custom";
    if (i == custom_count)
        custom_count++;
    return RC_OK;
}
int rc_get_register_info(uint16_t model, uint16_t address, rc_register_info *info) {
    static const char *const names[] = {"Model",
                                        "Serial number high",
                                        "Serial number low",
                                        "Firmware",
                                        "Temperature sign C",
                                        "Temperature C",
                                        "Temperature sign F",
                                        "Temperature F",
                                        "Set voltage",
                                        "Set current",
                                        "Output voltage",
                                        "Output current",
                                        "Power high",
                                        "Power low",
                                        "Input voltage",
                                        "Key lock",
                                        "Protection status",
                                        "CC/CV mode",
                                        "Output enable",
                                        "Quick preset",
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        "Capacity",
                                        NULL,
                                        "Energy"};
    static const char *const units[] = {"", "V", "A"};
    if (!info || address >= RC_REGISTERS)
        return RC_RANGE;
    memset(info, 0, sizeof(*info));
    info->address = address;
    if (address < sizeof(names) / sizeof(names[0]) && names[address]) {
        info->known = 1;
        info->name = names[address];
    } else if (address == 72) {
        info->known = info->writable = 1;
        info->name = "Backlight";
        info->raw_max = 5;
    } else if (address >= 80 && address <= 119) {
        static const char *const fields[] = {"Preset voltage", "Preset current", "Preset OVP",
                                             "Preset OCP"};
        unsigned field = (address - 80) % 4;
        info->known = info->writable = 1;
        info->name = fields[field];
        info->unit = units[field == 1 || field == 3 ? 2 : 1];
    } else {
        info->name = "Unknown";
        return RC_OK;
    }
    switch (address) {
    case 8:
        info->writable = 1;
        info->name = "Set voltage";
        break;
    case 9:
        info->writable = 1;
        info->name = "Set current";
        break;
    case 15:
        info->writable = 1;
        info->raw_max = 1;
        break;
    case 18:
        info->writable = 1;
        info->raw_max = 1;
        break;
    case 19:
        info->writable = 1;
        info->raw_max = 9;
        break;
    default:
        break;
    }
    rc_model_info known_model;
    if (model_profile(model, &known_model) != RC_OK)
        info->writable = 0;
    if (address == 72 && model == 8009)
        info->writable = 0;
    if (address == 8 || address == 10 ||
        (address >= 80 && address <= 119 && ((address - 80) % 4 == 0 || (address - 80) % 4 == 2))) {
        info->unit = units[1];
        info->scale = 100;
    } else if (address == 9 || address == 11 ||
               (address >= 80 && address <= 119 &&
                ((address - 80) % 4 == 1 || (address - 80) % 4 == 3))) {
        info->unit = units[2];
        info->scale = 1000;
    } else if (address == 14) {
        info->unit = units[1];
        info->scale = 100;
    }
    if (address == 8 || address == 9 || (address >= 80 && address <= 119)) {
        rc_model_info profile;
        if (model_profile(model, &profile) != RC_OK)
            info->writable = 0;
        else {
            unsigned field = address == 8 ? 0 : address == 9 ? 1 : (address - 80) % 4;
            info->scale = field == 1 || field == 3 ? profile.current_scale : profile.voltage_scale;
            double limit = field == 0   ? profile.max_voltage
                           : field == 1 ? profile.max_current
                           : field == 2 ? profile.max_ovp
                                        : profile.max_ocp;
            info->raw_max = (uint16_t)(limit * info->scale + 0.000001);
            if (field == 0 || field == 1 || field >= 2)
                info->writable = address == 8 || address == 9 || address >= 80;
        }
    }
    return RC_OK;
}
const char *rc_error_string(int e) {
    switch (e) {
    case 0:
        return "ok";
    case RC_IO:
        return "transport I/O";
    case RC_TIMEOUT:
        return "response timeout";
    case RC_PROTOCOL:
        return "invalid response";
    case RC_CRC:
        return "CRC mismatch";
    case RC_CLOSED:
        return "core closed";
    case RC_RANGE:
        return "invalid range/value";
    case RC_QUEUE_FULL:
        return "queue full";
    case RC_READ_ONLY:
        return "register is read-only";
    case RC_UNSUPPORTED:
        return "unsupported operation/model";
    case RC_NOMEM:
        return "out of memory";
    case RC_BUSY:
        return "device busy";
    default:
        return "unknown error";
    }
}
static int range(uint16_t s, uint16_t n) {
    return n && n <= RC_REGISTERS && s < RC_REGISTERS && (unsigned)s + n <= RC_REGISTERS;
}
static void append(struct task **q, struct task *t) {
    while (*q)
        q = &(*q)->next;
    *q = t;
}
static struct task *pop(struct task **q) {
    struct task *t = *q;
    if (t)
        *q = t->next;
    return t;
}
static int covers(struct task *t, uint16_t s, uint16_t n) {
    return t && t->fn == 3 && t->start <= s && (unsigned)t->start + t->count >= (unsigned)s + n;
}
static void release(struct task *t) {
    if (--t->refs == 0)
        free(t);
}
static void merge(rc_core *c, uint16_t start, uint16_t n, const uint16_t *v, uint64_t sent,
                  uint64_t done) {
    for (unsigned i = 0; i < n; i++) {
        c->cache.registers[start + i] = v[i];
        c->cache.valid[start + i] = 1;
        c->cache.updated_ns[start + i] = done;
    }
    if (start <= 10 && (unsigned)start + n > 11) {
        c->cache.sequence++;
        c->cache.request_ns = sent;
        c->cache.completed_ns = done;
    }
}
static void recover(rc_core *c, uint64_t *last) {
    uint8_t b[256];
    uint64_t end = rc_monotonic_ns() + 600000000ULL, quiet = rc_monotonic_ns();
    while (rc_monotonic_ns() < end && rc_monotonic_ns() - quiet < 100000000ULL) {
        int n = c->transport.read(c->transport.context, b, sizeof(b), 10);
        if (n < 0)
            break;
        if (n > 0) {
            quiet = rc_monotonic_ns();
            *last = quiet;
        }
    }
}
static int write_policy(unsigned model, uint16_t s, uint16_t n, const uint16_t *v) {
    rc_model_info profile;
    int supported = model_profile((uint16_t)model, &profile) == RC_OK;
    if (!supported)
        return RC_UNSUPPORTED;
    for (unsigned i = 0; i < n; i++) {
        unsigned a = s + i;
        rc_register_info reg;
        int e = rc_get_register_info((uint16_t)model, (uint16_t)a, &reg);
        if (e)
            return e;
        if (!reg.writable)
            return supported ? RC_READ_ONLY : RC_UNSUPPORTED;
        if ((a == 8 || a == 9 || (a >= 80 && a <= 119)) && !supported)
            return RC_UNSUPPORTED;
        if (a == 8 || a == 9 || (a >= 80 && a <= 119)) {
            if (v[i] > reg.raw_max)
                return RC_RANGE;
        } else if ((a == 15 || a == 18) && v[i] > 1)
            return RC_RANGE;
        else if (a == 72 && v[i] > reg.raw_max)
            return RC_RANGE;
    }
    return 0;
}
static void *worker(void *arg) {
    rc_core *c = arg;
    uint64_t last = 0, next_poll = 0;
    unsigned urgent = 0, failures = 0;
    int identify = 1;
    for (;;) {
        struct task *t = NULL;
        uint16_t values[RC_REGISTERS] = {0};
        uint16_t start, count;
        uint8_t fn = 3;
        uint64_t sent = 0, done = 0;
        rc_lock(&c->mutex);
        if (c->stop) {
            rc_unlock(&c->mutex);
            break;
        }
        uint64_t now = rc_monotonic_ns();
        int due = now >= next_poll;
        if (identify) {
            start = 0;
            count = 20;
        } else {
            if (c->reads && urgent == 0 && now - c->reads->created >= 500000000ULL) {
                t = pop(&c->reads);
                urgent++;
            } else if (c->writes && (urgent < 1 || !due)) {
                t = pop(&c->writes);
                urgent++;
            } else if (c->reads &&
                       (!due || (urgent < 1 && now - c->reads->created >= 500000000ULL))) {
                t = pop(&c->reads);
                urgent++;
            } else if (due) {
                urgent = 0;
            } else if (c->writes) {
                t = pop(&c->writes);
                urgent++;
            } else {
                unsigned ms = (unsigned)((next_poll - now + 999999) / 1000000);
                rc_timedwait(&c->changed, &c->mutex, ms);
                rc_unlock(&c->mutex);
                continue;
            }
            if (t) {
                c->cache.queued--;
                c->active = t;
                fn = t->fn;
                start = t->start;
                count = t->count;
                memcpy(values, t->values, count * sizeof(uint16_t));
            } else {
                start = c->config.poll_start;
                count = c->config.poll_count;
            }
        }
        if (!t) {
            c->hot_active = 1;
            c->hot_start = start;
            c->hot_count = count;
        }
        rc_unlock(&c->mutex);
        int error = 0;
        if (fn != 3) {
            rc_lock(&c->mutex);
            unsigned model = c->cache.valid[0] ? c->cache.registers[0] : 0;
            error = write_policy(model, start, count, values);
            rc_unlock(&c->mutex);
        }
        if (!error)
            error = rc_exchange(&c->transport, &c->config, &last, fn, start, count, values, &sent,
                                &done);
        rc_lock(&c->mutex);
        if (sent)
            c->cache.transactions++;
        c->cache.last_error = error;
        if (error && sent) {
            c->cache.failures++;
            memset(c->cache.valid, 0, sizeof(c->cache.valid));
            identify = 1;
            failures++;
        } else if (!error) {
            failures = 0;
            if (fn == 3)
                merge(c, start, count, values, sent, done);
            else
                memset(c->cache.valid + start, 0, count);
            if (identify)
                identify = 0;
        }
        if (t) {
            t->error = error;
            t->done = 1;
            if (!error && fn == 3)
                memcpy(t->values, values, count * sizeof(uint16_t));
            c->active = NULL;
            release(t);
        } else
            next_poll = sent + (uint64_t)c->config.period_ms * 1000000ULL;
        if (!t) {
            c->hot_active = 0;
            c->hot_generation++;
        }
        rc_broadcast(&c->changed);
        rc_unlock(&c->mutex);
        if (error && sent) {
            recover(c, &last);
            unsigned backoff = 50U << (failures > 4 ? 4 : failures - 1);
            rc_lock(&c->mutex);
            if (!c->stop)
                rc_timedwait(&c->changed, &c->mutex, backoff);
            rc_unlock(&c->mutex);
        }
    }
    rc_lock(&c->mutex);
    struct task *t;
    while ((t = pop(&c->reads)) || (t = pop(&c->writes))) {
        t->error = RC_CLOSED;
        t->done = 1;
        release(t);
    }
    c->cache.queued = 0;
    rc_broadcast(&c->changed);
    rc_unlock(&c->mutex);
    return NULL;
}
rc_core *rc_create(const rc_transport *t, const rc_config *cfg) {
    if (!t || !t->read || !t->write || !cfg || cfg->abi_version != RC_ABI_VERSION ||
        !range(cfg->poll_start, cfg->poll_count) || !cfg->address || cfg->address > 247 ||
        cfg->timeout_ms < 10 || cfg->timeout_ms > 10000 || cfg->frame_gap_us > 1000000)
        return NULL;
    rc_core *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->transport = *t;
    c->config = *cfg;
    rc_mutex_init(&c->mutex);
    rc_cond_init(&c->changed);
    if (rc_thread_start(&c->worker, worker, c)) {
        rc_mutex_destroy(&c->mutex);
        rc_cond_destroy(&c->changed);
        free(c);
        return NULL;
    }
    return c;
}
void rc_destroy(rc_core *c) {
    if (!c)
        return;
    rc_lock(&c->mutex);
    c->stop = 1;
    rc_broadcast(&c->changed);
    rc_unlock(&c->mutex);
    rc_thread_join(c->worker);
    rc_cond_destroy(&c->changed);
    rc_mutex_destroy(&c->mutex);
    free(c);
}
int rc_snapshot_get(rc_core *c, rc_snapshot *out) {
    if (!c || !out)
        return RC_RANGE;
    rc_lock(&c->mutex);
    *out = c->cache;
    int e = c->stop ? RC_CLOSED : 0;
    rc_unlock(&c->mutex);
    return e;
}
int rc_snapshot_wait(rc_core *c, uint64_t after, uint32_t timeout, rc_snapshot *out) {
    if (!c || !out)
        return RC_RANGE;
    uint64_t deadline = rc_monotonic_ns() + (uint64_t)timeout * 1000000ULL;
    rc_lock(&c->mutex);
    while (!c->stop && c->cache.sequence <= after) {
        uint64_t now = rc_monotonic_ns();
        if (now >= deadline) {
            rc_unlock(&c->mutex);
            return RC_TIMEOUT;
        }
        rc_timedwait(&c->changed, &c->mutex, (unsigned)((deadline - now + 999999) / 1000000));
    }
    *out = c->cache;
    int e = c->stop ? RC_CLOSED : 0;
    rc_unlock(&c->mutex);
    return e;
}
static int submit(rc_core *c, uint8_t fn, uint16_t s, uint16_t n, uint32_t age, uint16_t *v) {
    if (!c || !v || !range(s, n))
        return RC_RANGE;
    rc_lock(&c->mutex);
    if (c->stop) {
        rc_unlock(&c->mutex);
        return RC_CLOSED;
    }
    if (fn == 3 && age) {
        uint64_t now = rc_monotonic_ns();
        int fresh = 1;
        for (unsigned i = s; i < (unsigned)s + n; i++)
            if (!c->cache.valid[i] || now - c->cache.updated_ns[i] > (uint64_t)age * 1000000ULL)
                fresh = 0;
        if (fresh) {
            memcpy(v, c->cache.registers + s, n * sizeof(*v));
            c->cache.cache_hits++;
            rc_unlock(&c->mutex);
            return 0;
        }
    }
    if (fn == 3 && c->hot_active && c->hot_start <= s &&
        (unsigned)c->hot_start + c->hot_count >= (unsigned)s + n) {
        uint64_t generation = c->hot_generation;
        c->cache.coalesced++;
        while (!c->stop && generation == c->hot_generation)
            rc_wait(&c->changed, &c->mutex);
        int error = c->stop ? RC_CLOSED : 0;
        for (unsigned i = s; i < (unsigned)s + n; i++)
            if (!c->cache.valid[i])
                error = c->cache.last_error ? c->cache.last_error : RC_IO;
        if (!error)
            memcpy(v, c->cache.registers + s, n * sizeof(*v));
        rc_unlock(&c->mutex);
        return error;
    }
    struct task *t = NULL;
    if (fn == 3) {
        if (covers(c->active, s, n))
            t = c->active;
        else
            for (t = c->reads; t && !covers(t, s, n); t = t->next) {
            }
        if (t) {
            t->refs++;
            c->cache.coalesced++;
        }
    }
    if (!t) {
        if (c->cache.queued >= QUEUE_LIMIT) {
            c->cache.queue_rejected++;
            rc_unlock(&c->mutex);
            return RC_QUEUE_FULL;
        }
        t = calloc(1, sizeof(*t));
        if (!t) {
            rc_unlock(&c->mutex);
            return RC_NOMEM;
        }
        t->fn = fn;
        t->start = s;
        t->count = n;
        t->created = rc_monotonic_ns();
        t->refs = 2;
        if (fn != 3)
            memcpy(t->values, v, n * sizeof(*v));
        append(fn == 3 ? &c->reads : &c->writes, t);
        c->cache.queued++;
        rc_broadcast(&c->changed);
    }
    while (!t->done)
        rc_wait(&c->changed, &c->mutex);
    int e = t->error;
    if (!e && fn == 3)
        memcpy(v, t->values + (s - t->start), n * sizeof(*v));
    release(t);
    rc_unlock(&c->mutex);
    return e;
}
int rc_read(rc_core *c, uint16_t s, uint16_t n, uint32_t age, uint16_t *v) {
    return submit(c, 3, s, n, age, v);
}
int rc_write(rc_core *c, uint16_t s, uint16_t n, const uint16_t *v) {
    if (!c || !v || !range(s, n))
        return RC_RANGE;
    if (!c->config.allow_writes)
        return RC_READ_ONLY;
    if (n > 1 && !c->config.allow_multiwrite)
        return RC_UNSUPPORTED;
    uint16_t copy[RC_REGISTERS];
    memcpy(copy, v, n * sizeof(*v));
    return submit(c, n == 1 ? 6 : 16, s, n, 0, copy);
}
