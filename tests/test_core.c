#include "internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fake {
    uint8_t reply[256], request[256];
    size_t length, read, written;
};
static int read_fake(void *p, uint8_t *b, size_t n, uint32_t ms) {
    (void)ms;
    struct fake *f = p;
    if (f->read == f->length)
        return 0;
    if (n > 1)
        n = 1;
    memcpy(b, f->reply + f->read, n);
    f->read += n;
    return (int)n;
}
static int write_fake(void *p, const uint8_t *b, size_t n, uint32_t ms) {
    (void)ms;
    struct fake *f = p;
    if (n > 2)
        n = 2;
    memcpy(f->request + f->written, b, n);
    f->written += n;
    return (int)n;
}
static void checksum(struct fake *f, size_t length) {
    uint16_t c = rc_crc(f->reply, length);
    f->reply[length] = (uint8_t)c;
    f->reply[length + 1] = (uint8_t)(c >> 8);
    f->length = length + 2;
    f->read = f->written = 0;
}
int main(void) {
    uint8_t known[] = {1, 3, 0, 0, 0, 10};
    assert(rc_crc(known, sizeof(known)) == 0xcdc5);
    const uint16_t model_ids[] = {6006, 60060, 60064, 60065, 60066, 60067, 6012, 60124,
                                  6018, 60189, 6024,  60249, 6030,  60309, 8009};
    for (unsigned i = 0; i < sizeof(model_ids) / sizeof(model_ids[0]); i++) {
        rc_model_info model;
        assert(rc_get_model_info(model_ids[i], &model) == RC_OK);
        assert(model.voltage_scale && model.current_scale);
        rc_register_info reg;
        assert(rc_get_register_info(model_ids[i], 8, &reg) == RC_OK);
        assert(reg.known && reg.writable &&
               reg.raw_max == (uint16_t)(model.max_voltage * model.voltage_scale));
        assert(rc_get_register_info(model_ids[i], 9, &reg) == RC_OK);
        assert(reg.known && reg.writable &&
               reg.raw_max == (uint16_t)(model.max_current * model.current_scale));
        assert(rc_get_register_info(model_ids[i], 82, &reg) == RC_OK);
        assert(reg.writable && reg.raw_max == (uint16_t)(model.max_ovp * model.voltage_scale));
        assert(rc_get_register_info(model_ids[i], 83, &reg) == RC_OK);
        assert(reg.writable && reg.raw_max == (uint16_t)(model.max_ocp * model.current_scale));
    }
    rc_model_info unknown;
    assert(rc_get_model_info(60125, &unknown) == RC_UNSUPPORTED);
    rc_register_info reg;
    rc_model_info custom = {12345, NULL, 1000, 10000, 61, 6.1, 62, 6.2};
    assert(rc_get_register_info(12345, 8, &reg) == RC_OK && !reg.writable);
    assert(rc_register_model(&custom) == RC_OK);
    assert(rc_get_model_info(12345, &unknown) == RC_OK && !strcmp(unknown.name, "Custom"));
    assert(unknown.voltage_scale == 1000 && unknown.current_scale == 10000);
    assert(rc_get_register_info(12345, 8, &reg) == RC_OK && reg.writable && reg.scale == 1000 &&
           reg.raw_max == 61000);
    assert(rc_get_register_info(12345, 9, &reg) == RC_OK && reg.writable && reg.raw_max == 61000);
    assert(rc_get_register_info(12345, 83, &reg) == RC_OK && reg.writable && reg.raw_max == 62000);
    custom.max_voltage = 30;
    assert(rc_register_model(&custom) == RC_OK);
    assert(rc_get_register_info(12345, 8, &reg) == RC_OK && reg.raw_max == 30000);
    custom.max_voltage = 70;
    assert(rc_register_model(&custom) == RC_RANGE);
    custom.max_voltage = 30;
    custom.voltage_scale = 50;
    assert(rc_register_model(&custom) == RC_RANGE);
    custom.voltage_scale = 100;
    custom.model = 60067;
    assert(rc_register_model(&custom) == RC_BUSY);
    assert(rc_get_model_info(60067, &unknown) == RC_OK && !strcmp(unknown.name, "RD6006"));
    for (uint16_t id = 20000; id < 20007; id++) {
        custom.model = id;
        assert(rc_register_model(&custom) == RC_OK);
    }
    custom.model = 20007;
    assert(rc_register_model(&custom) == RC_QUEUE_FULL);
    assert(rc_get_register_info(60067, 10, &reg) == RC_OK && !reg.writable);
    assert(rc_get_register_info(60067, 120, &reg) == RC_OK && !reg.known);
    assert(rc_get_register_info(60067, 72, &reg) == RC_OK && reg.known && reg.writable &&
           !reg.inferred && reg.raw_min == 0 && reg.raw_max == 5);
    const char *known_names[] = {"Model",
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
                                 "Capacity",
                                 "Energy"};
    const unsigned known_addresses[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                        11, 12, 13, 14, 15, 16, 17, 18, 19, 39, 41};
    for (unsigned i = 0; i < sizeof(known_addresses) / sizeof(known_addresses[0]); i++) {
        assert(rc_get_register_info(60067, (uint16_t)known_addresses[i], &reg) == RC_OK);
        assert(reg.known && strcmp(reg.name, known_names[i]) == 0);
    }
    rc_config cfg = rc_default_config();
    cfg.frame_gap_us = 0;
    struct fake f = {0};
    rc_transport t = {&f, read_fake, write_fake};
    uint64_t last = 0, sent = 0, done = 0;
    uint16_t v[4] = {0};
    uint8_t response[] = {1, 3, 8, 0, 1, 0, 2, 0, 3, 0, 4};
    memcpy(f.reply, response, sizeof(response));
    checksum(&f, sizeof(response));
    assert(rc_exchange(&t, &cfg, &last, 3, 10, 4, v, &sent, &done) == 0);
    assert(v[0] == 1 && v[3] == 4);
    assert(f.written == 8);
    assert(f.request[2] == 0 && f.request[3] == 10);
    assert(rc_crc(f.request, 6) == (uint16_t)(f.request[6] | f.request[7] << 8));
    f.read = f.written = 0;
    f.reply[f.length - 1] ^= 1;
    assert(rc_exchange(&t, &cfg, &last, 3, 10, 4, v, &sent, &done) == RC_CRC);
    memcpy(f.reply, response, sizeof(response));
    f.reply[0] = 2;
    checksum(&f, sizeof(response));
    assert(rc_exchange(&t, &cfg, &last, 3, 10, 4, v, &sent, &done) == RC_PROTOCOL);
    uint8_t exception[] = {1, 0x83, 2};
    memcpy(f.reply, exception, 3);
    checksum(&f, 3);
    assert(rc_exchange(&t, &cfg, &last, 3, 10, 4, v, &sent, &done) == RC_UNSUPPORTED);
    uint8_t echo[] = {1, 6, 0, 18, 0, 0};
    memcpy(f.reply, echo, 6);
    checksum(&f, 6);
    v[0] = 0;
    assert(rc_exchange(&t, &cfg, &last, 6, 18, 1, v, &sent, &done) == 0);
    f.reply[3] = 19;
    checksum(&f, 6);
    assert(rc_exchange(&t, &cfg, &last, 6, 18, 1, v, &sent, &done) == RC_PROTOCOL);
    f.length = f.read = f.written = 0;
    assert(rc_exchange(&t, &cfg, &last, 3, 10, 4, v, &sent, &done) == RC_TIMEOUT);
    cfg.poll_count = 0;
    assert(rc_create(&t, &cfg) == NULL);
    puts("Known CRC, partial reads/writes, address validation, CRC rejection, exception framing, "
         "write echo and timeout passed");
    return 0;
}
