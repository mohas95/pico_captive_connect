#include "creds_store.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdint.h>

#ifndef CREDS_FLASH_OFFSET
// reserve last 64KB sector (adjust if you already use it)
#define CREDS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 64*1024)
#endif

#define CREDS_MAGIC 0x43525749u  // 'I','W','R','C' (just a tag)

struct Blob {
    uint32_t magic;
    uint32_t crc;
    DeviceCreds creds;
};

static uint32_t crc32(const void *data, size_t len){
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t*)data;
    while (len--) {
        c ^= *p++;
        for (int i=0;i<8;i++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return ~c;
}

bool creds_load(DeviceCreds &out) {
    const Blob *b = (const Blob*)(XIP_BASE + CREDS_FLASH_OFFSET);
    if (b->magic != CREDS_MAGIC) return false;
    uint32_t calc = crc32(&b->creds, sizeof(DeviceCreds));
    if (calc != b->crc) return false;
    out = b->creds;

    if (out.dirty) {
        // clear dirty immediately so we don't reboot repeatedly
        DeviceCreds cleared = out;
        cleared.dirty = false;
        creds_save(cleared);
    }

    return out.valid;
}

bool creds_save(const DeviceCreds &in, bool mark_dirty) {
    Blob b{};
    b.magic = CREDS_MAGIC;
    b.creds = in;
    b.creds.valid = true;
    if (mark_dirty) {
        b.creds.dirty = true;
    }
    b.crc = crc32(&b.creds, sizeof(DeviceCreds));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CREDS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CREDS_FLASH_OFFSET, (const uint8_t*)&b, sizeof(b));
    restore_interrupts(ints);
    return true;
}

void creds_clear() {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CREDS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}