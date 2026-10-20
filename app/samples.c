/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm1_engine_control.h"
#include "fm1_device.h"
#include "instrument.h"
#include "sampledrum.h"
#include <string.h>

/* Single bank, no hidden second copy. CRC and a last-written commit word make
 * an interrupted import visibly incomplete. Projects occupy separate sectors.
 * The browser retains/downloads a complete bank before replacing one slot. */
struct sample_entry {
    uint32_t offset, frames, bytes;
    char name[16];
};
static struct sample_header {
    uint32_t magic, version, bytes, crc, commit, rate, reserved[2];
    struct sample_entry entry[16];
    uint8_t padding[32];
} header;
_Static_assert(sizeof header == 512, "sample bank header");
static unsigned state, total, position;
static uint32_t checksum, changed_at;
static uint32_t crc32(uint32_t crc, unsigned offset, const uint8_t *p, unsigned bytes)
{
    /* Treat the stored CRC and commit word as zero, including split writes. */
    static const uint8_t zero[8] = {0};
    if (offset < 12) {
        unsigned n = bytes < 12 - offset ? bytes : 12 - offset;
        crc = groove_crc32(crc, p, n);
        p += n; offset += n; bytes -= n;
    }
    if (offset < 20 && bytes) {
        unsigned n = bytes < 20 - offset ? bytes : 20 - offset;
        crc = groove_crc32(crc, zero, n);
        p += n; bytes -= n;
    }
    return groove_crc32(crc, p, bytes);
}
static void unbind(void)
{
    fm1_engine_pause();
    groove_samples_stop();
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i)
        (void)sampledrum_bind(i, NULL, 0, 0, NULL);
    fm1_engine_resume();
}
static int valid(void)
{
    if (header.magic != 0x53314d46U || header.version != 1 || header.commit ||
        header.rate != 32000 || header.bytes < sizeof header ||
        header.bytes > GROOVE_SAMPLE_BYTES || header.reserved[0] || header.reserved[1])
        return 0;
    for (unsigned i = 0; i < sizeof header.padding; ++i)
        if (header.padding[i])
            return 0;
    unsigned next = sizeof header;
    for (unsigned i = 0; i < 16; ++i) {
        const struct sample_entry *s = &header.entry[i];
        if (s->offset != next || s->frames > SAMPLE_DRUM_MAX_FRAMES ||
            (i >= SAMPLE_DRUM_PADS && (s->frames || s->name[0])) ||
            s->bytes != ((s->frames + 319U) / 320U) * 228U || s->bytes > header.bytes - next ||
            s->name[15])
            return 0;
        for (unsigned n = 0; n < 15; ++n)
            if ((uint8_t)s->name[n] && (s->name[n] < 32 || s->name[n] > 126))
                return 0;
        next += s->bytes;
    }
    return next == header.bytes;
}
static const uint8_t *mapped_data(void)
{
    const uint8_t *mapped = fm1_flash_map(GROOVE_SAMPLE_OFFSET, total);
    if (!mapped)
        return NULL;
    for (unsigned p = sizeof header; p < total; p += 228U)
        if (mapped[p] != 1 || mapped[p + 1] != 0x45 || mapped[p + 2] != 20 || mapped[p + 3] != 0x5a)
            return NULL;
    return mapped;
}
static int bind(void)
{
    const uint8_t *mapped = mapped_data();
    if (!mapped)
        return GROOVE_PROJECT_INVALID;
    fm1_engine_pause();
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i) {
        const struct sample_entry *s = &header.entry[i];
        (void)sampledrum_bind(i, s->frames ? mapped + s->offset : NULL, s->bytes, s->frames,
                              s->name);
    }
    fm1_engine_resume();
    state = 2;
    return 0;
}
void groove_samples_init(void)
{
    unbind();
    state = total = position = 0;
    checksum = ~0U;
    state = 1;
}
void groove_samples_poll(void)
{
    if (state == 3 && fm1_millis() - changed_at > 10000U)
        groove_samples_cancel();
    if (state != 1)
        return;
    if (!total) {
        if (fm1_flash_size() != GROOVE_FLASH_BYTES) {
            state = 4;
            return;
        }
        int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET, &header, sizeof header);
        if (r == FM1_FLASH_BUSY)
            return;
        if (r) {
            state = 4;
            return;
        }
        if (!valid()) {
            state = 0;
            return;
        }
        total = header.bytes;
    }
    uint8_t bytes[512];
    for (unsigned budget = 0; budget < 8 && position < total; ++budget) {
        unsigned n = total - position;
        if (n > sizeof bytes)
            n = sizeof bytes;
        int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET + position, bytes, n);
        if (r == FM1_FLASH_BUSY) return;
        if (r) {
            state = 4;
            return;
        }
        checksum = crc32(checksum, position, bytes, n);
        position += n;
    }
    if (position == total && (~checksum != header.crc || bind()))
        state = 4;
}
unsigned groove_samples_status(void)
{
    return state;
}
unsigned groove_samples_size(void)
{
    return state == 2 ? total : 0;
}
int groove_samples_begin(unsigned bytes)
{
    if (bytes < sizeof header || bytes > GROOVE_SAMPLE_LIMIT ||
        fm1_flash_size() != GROOVE_FLASH_BYTES)
        return GROOVE_PROJECT_INVALID;
    if (groove_samples_capture_active())
        return GROOVE_PROJECT_INVALID;   /* the commit machine owns the bank */
    unbind();
    /* Erase the commit first, before any old sample bytes can be overwritten. */
    groove_edit_record(0, 0, 0);
    fm1_engine_pause();
    fm1_engine_stop();
    fm1_engine_resume();
    int r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET);
    if (r) {
        state = 4;
        return r;
    }
    state = 3;
    total = bytes;
    position = 0;
    checksum = ~0U;
    changed_at = fm1_millis();
    memset(&header, 0, sizeof header);
    return 0;
}
int groove_samples_write(unsigned offset, const uint8_t *data, unsigned bytes)
{
    if (state != 3 || !data || !bytes || bytes > 128 || offset != position ||
        bytes > total - position)
        return GROOVE_PROJECT_INVALID;
    uint8_t copy[128];
    memcpy(copy, data, bytes);
    if (offset < sizeof header) {
        unsigned n = sizeof header - offset;
        if (n > bytes)
            n = bytes;
        memcpy((uint8_t *)&header + offset, data, n);
        for (unsigned i = 0; i < bytes; ++i)
            if (offset + i >= 16 && offset + i < 20)
                copy[i] = 255;
    }
    /* Requests may straddle a sector. Erase only the as-yet-unwritten sector. */
    unsigned first = offset / 4096U, last = (offset + bytes - 1U) / 4096U;
    int r = 0;
    if (offset && !(offset % 4096U))
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + first * 4096U);
    if (!r && last != first)
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + last * 4096U);
    if (!r)
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + offset, copy, bytes);
    if (r) {
        state = 4;
        return r;
    }
    checksum = crc32(checksum, offset, data, bytes);
    position += bytes;
    changed_at = fm1_millis();
    return 0;
}
int groove_samples_finish(void)
{
    if (state != 3 || position != total || header.bytes != total || !valid() ||
        ~checksum != header.crc)
        return GROOVE_PROJECT_INVALID;
    /* Validate codec headers before committing, including data just written
     * through the raw Flash API and not yet visible in the mapped cache. */
    int r = fm1_flash_invalidate(GROOVE_SAMPLE_OFFSET, total);
    if (!r && !mapped_data())
        r = GROOVE_PROJECT_INVALID;
    uint32_t commit = 0;
    if (!r)
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + 16, &commit, sizeof commit);
    if (!r)
        r = bind();
    if (r)
        state = 4;
    return r;
}
void groove_samples_cancel(void)
{
    if (state == 3)
        state = 4;
}
int groove_samples_read(unsigned offset, uint8_t *data, unsigned bytes)
{
    if (state != 2 || !data || !bytes || bytes > 64 || offset > total || bytes > total - offset)
        return GROOVE_PROJECT_INVALID;
    return fm1_flash_read(GROOVE_SAMPLE_OFFSET + offset, data, bytes);
}

/* ---- Live capture commit ------------------------------------------------
 * Appends a staged SEA5 take as the first free pad slot. Data lands beyond
 * the current bank image while audio keeps playing; the header (sector 0,
 * shared with entry 0's first bytes) is rewritten last under an unbind, so an
 * interrupted commit leaves the previous bank untouched and valid. */

static struct {
    uint8_t phase;                 /* 0 idle, 1 data, 2 crc, 3 header, 4 done */
    uint32_t step;                 /* progress inside a phase */
    int result;                    /* 1 saved, negative error, 0 while busy */
    const uint8_t *data;           /* staged SEA5 chunks, owned by app/capture.c */
    unsigned chunks, at;           /* staged chunk count; bytes already flashed */
    unsigned data_start, new_total, slot;
    uint32_t crc;
    uint8_t sector[4096];          /* read-modify-write buffer for one sector */
    uint8_t newhdr[512];           /* the header this commit will store */
    uint8_t fresh;
} cap;

static void cap_fail(int error, int broke)
{
    cap.result = error;
    cap.phase = 4;
    if (broke) state = 4;          /* header already touched: bank is suspect */
}

int groove_samples_capture_target(unsigned *slot, unsigned *room)
{
    if (cap.phase && cap.phase < 4) return -1;
    if (fm1_flash_size() != GROOVE_FLASH_BYTES) return -1;
    if (state == 1 || state == 3) return -1;
    /* Appends after the last filled pad. A full bank replaces the last pad:
     * nothing behind it shifts, so the take lands with the same machinery. */
    unsigned first = 0;
    if (state == 2) {
        unsigned last = 0;
        for (unsigned i = SAMPLE_DRUM_PADS; i-- > 0;)
            if (header.entry[i].frames) { last = i; break; }
        first = last + 1 <= SAMPLE_DRUM_PADS - 1 ? last + 1 : last;
    }
    unsigned start = state == 2 ? header.entry[first].offset : sizeof header;
    if (slot) *slot = first;
    if (room) *room = start < GROOVE_SAMPLE_LIMIT ? GROOVE_SAMPLE_LIMIT - start : 0;
    return 0;
}

int groove_samples_capture_commit(const uint8_t *data, unsigned chunks)
{
    unsigned slot = 0, room = 0;
    int r = groove_samples_capture_target(&slot, &room);
    if (r) return r;
    if (!data || !chunks || chunks > SAMPLE_DRUM_MAX_FRAMES / SAMPLE_DRUM_CHUNK_FRAMES ||
        chunks * SAMPLE_DRUM_CHUNK_BYTES > room)
        return -1;
    cap.fresh = state != 2;
    cap.data = data;
    cap.chunks = chunks;
    cap.at = 0;
    cap.slot = slot;
    cap.data_start = cap.fresh ? (unsigned)sizeof header : header.entry[slot].offset;
    cap.new_total = cap.data_start + chunks * SAMPLE_DRUM_CHUNK_BYTES;
    cap.crc = ~0U;
    cap.phase = 1;
    cap.step = 0;
    cap.result = 0;
    groove_edit_record(0, 0, 0);   /* sample writes pause playback: leave REC */
    return 0;
}

int groove_samples_capture_active(void) { return cap.phase && cap.phase < 4; }

int groove_samples_capture_result(void) { return cap.result; }

/* Erase the next not-yet-written sector for a sequential write, mirroring
 * groove_samples_write's boundary/straddle pattern. */
static int cap_erase_for(unsigned offset, unsigned bytes)
{
    unsigned first = offset / 4096U, last = (offset + bytes - 1U) / 4096U;
    int r = 0;
    if (offset && !(offset % 4096U))
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + first * 4096U);
    if (!r && last != first)
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + last * 4096U);
    return r;
}

void groove_samples_capture_poll(void)
{
    if (!cap.phase || cap.phase == 4) return;
    unsigned at = cap.data_start + cap.at;
    unsigned bytes = cap.chunks * SAMPLE_DRUM_CHUNK_BYTES;
    switch (cap.phase) {
    case 1: {                      /* staged data into the bank tail */
        if (cap.step == 0) {       /* sector-aligned takes stream directly */
            cap.step = at % 4096U ? 1 : 35;
        }
        if (cap.step == 1) {       /* read the sector the take starts inside */
            int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET + at - at % 4096U,
                                   cap.sector, sizeof cap.sector);
            if (r == FM1_FLASH_BUSY) return;
            if (r) { cap_fail(GROOVE_PROJECT_INVALID, 0); return; }
            cap.step = 2;
        }
        if (cap.step == 2) {       /* patch staged bytes over the old tail */
            unsigned n = 4096U - at % 4096U;
            if (n > bytes) n = bytes;
            memcpy(cap.sector + at % 4096U, cap.data, n);
            int r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + at - at % 4096U);
            if (r == FM1_FLASH_BUSY) return;
            if (r) { cap_fail(GROOVE_PROJECT_INVALID, 0); return; }
            cap.step = 3;
        }
        if (cap.step >= 3 && cap.step < 35) {   /* sector rewrite, 128 B at a time */
            unsigned base = at - at % 4096U;
            for (unsigned budget = 0; budget < 8 && cap.step < 35; ++budget) {
                unsigned part = (cap.step - 3) * 128;
                int r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + base + part,
                                        cap.sector + part, 128);
                if (r == FM1_FLASH_BUSY) return;
                if (r) { cap_fail(GROOVE_PROJECT_INVALID, 0); return; }
                cap.step++;
            }
            if (cap.step < 35) return;
            unsigned n = 4096U - at % 4096U;
            cap.at = n > bytes ? bytes : n;
            cap.step = cap.at == bytes ? 0 : 35;
            if (cap.step == 0) cap.phase = 2;
            return;
        }
        if (cap.step == 35) {      /* stream the rest at sector boundaries */
            for (unsigned budget = 0; budget < 4 && cap.at < bytes; ++budget) {
                unsigned n = bytes - cap.at;
                if (n > 128) n = 128;
                int r = cap_erase_for(at, n);
                if (!r) r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + at, cap.data + cap.at, n);
                if (r == FM1_FLASH_BUSY) return;
                if (r) { cap_fail(GROOVE_PROJECT_INVALID, 0); return; }
                cap.at += n;
                at += n;
            }
            if (cap.at == bytes) { cap.phase = 2; cap.step = 0; }
            return;
        }
        return;
    }
    case 2: {                      /* CRC the final image: new header then data */
        if (!cap.step) {
            struct sample_header next;
            memset(&next, 0, sizeof next);
            if (!cap.fresh) memcpy(&next, &header, sizeof next);
            next.magic = 0x53314d46U;
            next.version = 1;
            next.bytes = cap.new_total;
            next.rate = 32000;
            next.entry[cap.slot].offset = cap.data_start;
            next.entry[cap.slot].frames = cap.chunks * SAMPLE_DRUM_CHUNK_FRAMES;
            next.entry[cap.slot].bytes = cap.chunks * SAMPLE_DRUM_CHUNK_BYTES;
            static const char *const names[8] = {"GRAB 1", "GRAB 2", "GRAB 3", "GRAB 4",
                                                 "GRAB 5", "GRAB 6", "GRAB 7", "GRAB 8"};
            memcpy(next.entry[cap.slot].name, names[cap.slot], strlen(names[cap.slot]) + 1);
            memcpy(cap.newhdr, &next, sizeof next);
        }
        uint8_t bytes_buf[512];
        for (unsigned budget = 0; budget < 8; ++budget) {
            unsigned off = cap.step;
            unsigned n = cap.new_total - off;
            if (n > sizeof bytes_buf) n = sizeof bytes_buf;
            if (off < sizeof header) {
                cap.crc = crc32(cap.crc, off, cap.newhdr + off, n);
            } else {
                int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET + off, bytes_buf, n);
                if (r == FM1_FLASH_BUSY) return;
                if (r) { cap_fail(GROOVE_PROJECT_INVALID, 0); return; }
                cap.crc = crc32(cap.crc, off, bytes_buf, n);
            }
            cap.step += n;
            if (cap.step == cap.new_total) {
                cap.phase = 3;
                cap.step = 0;
                return;
            }
        }
        return;
    }
    case 3: {                      /* rewrite sector 0: preserve data, new header */
        if (cap.step == 0) {
            int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET, cap.sector, sizeof cap.sector);
            if (r == FM1_FLASH_BUSY) return;
            if (r) { cap_fail(GROOVE_PROJECT_INVALID, 1); return; }
            memcpy(cap.sector, cap.newhdr, sizeof cap.newhdr);
            cap.step = 1;
            unbind();              /* pads lose Flash backing until re-bind */
        }
        if (cap.step == 1) {
            int r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET);
            if (r == FM1_FLASH_BUSY) return;
            if (r) { cap_fail(GROOVE_PROJECT_INVALID, 1); return; }
            cap.step = 2;
        }
        for (unsigned budget = 0; budget < 8 && cap.step < 2 + 32; ++budget) {
            unsigned part = (cap.step - 2) * 128;
            int r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + part, cap.sector + part, 128);
            if (r == FM1_FLASH_BUSY) return;
            if (r) { cap_fail(GROOVE_PROJECT_INVALID, 1); return; }
            cap.step++;
        }
        if (cap.step < 34) return;
        {
            int r = fm1_flash_invalidate(GROOVE_SAMPLE_OFFSET, cap.new_total);
            if (r == FM1_FLASH_BUSY) return;
            if (!r) {
                memcpy(&header, cap.newhdr, sizeof header);
                total = cap.new_total;
                r = bind();
            }
            if (r) { cap_fail(GROOVE_PROJECT_INVALID, 1); return; }
            cap.phase = 4;
            cap.result = 1;
        }
        return;
    }
    }
}
