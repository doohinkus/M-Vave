/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "capture.h"
#include "fm1.h"
#include "fm1_block.h"
#include "instrument.h"
#include "capture_sin.h"
#include "sea_encode.h"
#undef FM1_DSP_DATA                 /* the quantizer table serves the control
                                     * task: keep it in flash rodata instead of
                                     * the tight audio SRAM region */
#define FM1_DSP_DATA
#include "sample_dqt.inc"
#include <stdlib.h>
#include <string.h>

/* Live Bluetooth capture. The audio interrupt copies its A2DP block into a
 * small ring; the control task downmixes to mono, resamples 44.1 -> 32 kHz,
 * streams the take through the SEA5 encoder into a RAM staging buffer and, on
 * stop, hands it to the sample bank for an append commit. Capture-what-you-
 * hear: a Bluetooth underrun records silence, matching the monitor output. */

enum { RING_BLOCKS = 16 };             /* ~46 ms of jitter absorption */
enum { CAPTURE_PHASES = 320, CAPTURE_TAPS = 32 };
enum { CAPTURE_CHUNK_FRAMES = 320, CAPTURE_CHUNK_BYTES = 228 };
enum { CAPTURE_MIN_CHUNKS = 8 };       /* 160 ms minimum take */
enum { MONO_HISTORY = CAPTURE_TAPS - 1 };

/* Polyphase resampler coefficients, generated at boot. 320 phases x 32 taps
 * (the 441/320 ratio), Blackman-Harris windowed sinc, cutoff 16 kHz, Q15 with
 * per-phase DC gain of exactly 32768. BSS keeps the 20 KiB table out of the
 * flash image; RAM has far more room than flash on this board. */
static int16_t resample[CAPTURE_PHASES][CAPTURE_TAPS];

/* Quarter-wave sine in Q15: phase 0..65535 spans 0..2pi, wraps freely. */
static int32_t sin_q15(uint32_t phase)
{
    unsigned quadrant = phase >> 14 & 3u;
    unsigned index = (phase & 16383u) >> CAPTURE_SIN_SHIFT;
    unsigned frac = (phase & 16383u) & ((1u << CAPTURE_SIN_SHIFT) - 1u);
    int32_t a = capture_sin[index], b = capture_sin[index + 1];
    int32_t v = a + ((b - a) * (int32_t)frac >> CAPTURE_SIN_SHIFT);
    switch (quadrant) {
    case 0: return v;
    case 1: return capture_sin[CAPTURE_SIN_N - index];
    case 2: return -v;
    default: return -capture_sin[CAPTURE_SIN_N - index];
    }
}

#define BH_A0 11757                            /* Blackman-Harris in Q15 */
#define BH_A1 16002
#define BH_A2 4630
#define BH_A3 383
#define SINC_ARG_Q26 76084                     /* turns per T unit, Q26 */
#define PI_T_Q20 10289                         /* pi/320 rad per T unit, Q20 */
#define TAP_SCALE 32                           /* Q15 correction, see below */

/* Coefficient for tap j of phase phi: h(t) = sin(pi*fc*t)*window(t)/(pi*t)
 * with t = (j - 15 - phi/320) input samples, T = 320*t. h is even in t. */
static int32_t generate_tap(unsigned phi, int32_t j)
{
    int32_t t = (int32_t)(j * CAPTURE_PHASES) - (int32_t)phi
        - (int32_t)((CAPTURE_TAPS / 2 - 1) * CAPTURE_PHASES);
    int32_t magnitude = t < 0 ? -t : t;
    if (magnitude >= (int32_t)(CAPTURE_PHASES * (CAPTURE_TAPS / 2))) return 0;
    if (!magnitude) return 23779;              /* fc in Q15: the sinc peak */
    uint32_t x = (uint32_t)magnitude * 65536u
        / (CAPTURE_PHASES * (CAPTURE_TAPS / 2) / 2u) >> 1;   /* |t|/16 in Q16 */
    int32_t window = BH_A0
        + (BH_A1 * sin_q15(16384u + (x >> 1)) >> 15)
        + (BH_A2 * sin_q15(16384u + x) >> 15)
        + (BH_A3 * sin_q15(16384u + x + (x >> 1)) >> 15);
    int32_t sine = sin_q15((uint32_t)SINC_ARG_Q26 * (uint32_t)magnitude >> 10);
    /* h_q15 = 32768*h = sine*window*2^20 / (2^15 * PI_T_Q20 * T) */
    return (int32_t)((int64_t)sine * window * (int64_t)TAP_SCALE
        / ((int64_t)PI_T_Q20 * magnitude));
}

static void generate_resample(void)
{
    for (unsigned phi = 0; phi < CAPTURE_PHASES; ++phi) {
        int32_t sum = 0, largest = 0;
        for (unsigned j = 0; j < CAPTURE_TAPS; ++j) {
            resample[phi][j] = (int16_t)generate_tap(phi, (int32_t)j);
            sum += resample[phi][j];
            if (abs(resample[phi][j]) > abs(resample[phi][largest])) largest = (int32_t)j;
        }
        resample[phi][largest] += (int16_t)(32768 - sum);   /* exact DC gain */
    }
}

static int16_t ring[RING_BLOCKS][FM1_BLOCK_WORDS];
static volatile uint8_t ring_head, ring_tail, capturing;
static volatile unsigned ring_drops;

static enum fm1_capture_state state;
static enum fm1_capture_error error_code;
static uint8_t *staging;
static unsigned staging_chunks;        /* staging capacity in chunks */
static unsigned staged_chunks;         /* chunks encoded so far */
static unsigned frames_out;            /* 32 kHz frames produced */
static unsigned done_at;               /* fm1_millis() when the take finished */
static struct fm1_sea_encoder encoder;
static int16_t chunk_buf[CAPTURE_CHUNK_FRAMES];
static unsigned chunk_fill;

/* Resampler carry-over: the last MONO_HISTORY mono samples plus the fractional
 * position of the next output on the 441/320 grid. */
static int16_t res_hist[MONO_HISTORY];
static unsigned res_base, res_frac;
static uint32_t stream_index;          /* absolute mono frames ingested */

static unsigned target_slot;
static unsigned capacity_ds;           /* take capacity in 0.1 s units */

static void cleanup(void)
{
    free(staging);
    staging = NULL;
    staged_chunks = 0;
    frames_out = 0;
    chunk_fill = 0;
    target_slot = 255;
    capacity_ds = 0;
    ring_head = ring_tail = 0;
    res_base = 0;
    res_frac = 0;
    stream_index = 0;
    memset(res_hist, 0, sizeof res_hist);
    memset(chunk_buf, 0, sizeof chunk_buf);
    fm1_sea_encoder_init(&encoder);
}

enum fm1_capture_state fm1_capture_state(void) { return state; }
enum fm1_capture_error fm1_capture_error(void) { return error_code; }
unsigned fm1_capture_slot(void) { return target_slot; }
unsigned fm1_capture_drops(void) { return ring_drops; }

unsigned fm1_capture_ms(void)
{
    return (unsigned)(((uint64_t)frames_out * 1000u) / 32000u);
}

unsigned fm1_capture_deciseconds(void) { return capacity_ds; }

/* Audio interrupt. Single-producer/single-consumer ring; the producer drops
 * its own block when the consumer stalls and never touches the tail. */
FM1_AUDIO_CODE void fm1_capture_feed(const int16_t *stereo, unsigned frames)
{
    if (!capturing || frames != FM1_BLOCK_FRAMES) return;
    unsigned head = ring_head, next = head + 1 == RING_BLOCKS ? 0 : head + 1;
    if (next == ring_tail) {           /* consumer stalled: lose the newest */
        ring_drops++;
        return;
    }
    int16_t *slot = ring[head];
    if (stereo) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) slot[i] = stereo[i];
    } else {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) slot[i] = 0;
    }
    ring_head = (uint8_t)next;
}

void fm1_capture_cancel(void)
{
    if (state == FM1_CAPTURE_COMMIT) return;   /* flash surgery must finish */
    if (state == FM1_CAPTURE_RECORDING) capturing = 0;
    cleanup();
    error_code = FM1_CAPTURE_NONE;
    state = FM1_CAPTURE_IDLE;
}

void fm1_capture_arm(void)
{
    if (state == FM1_CAPTURE_RECORDING || state == FM1_CAPTURE_COMMIT) return;
    if (resample[0][CAPTURE_TAPS / 2] == 0) generate_resample();
    cleanup();
    error_code = FM1_CAPTURE_NONE;
    state = FM1_CAPTURE_IDLE;

    struct fm1_bluetooth_status radio;
    fm1_bluetooth_get_status(&radio);
    if (!radio.audio_connected) {
        error_code = FM1_CAPTURE_NO_BT;
        state = FM1_CAPTURE_ERROR;
        done_at = fm1_millis();
        return;
    }
    unsigned slot = 0, room = 0;
    int r = groove_samples_capture_target(&slot, &room);
    if (r) {
        error_code = r == -2 ? FM1_CAPTURE_BANK_FULL : FM1_CAPTURE_BANK_BUSY;
        state = FM1_CAPTURE_ERROR;
        done_at = fm1_millis();
        return;
    }
    unsigned chunks_room = room / CAPTURE_CHUNK_BYTES;
    if (chunks_room < CAPTURE_MIN_CHUNKS) {
        error_code = FM1_CAPTURE_BANK_FULL;
        state = FM1_CAPTURE_ERROR;
        done_at = fm1_millis();
        return;
    }
    /* Heap is shared with the Bluetooth stack: take the largest staging that
     * fits today instead of assuming a fixed budget. */
    static const unsigned budgets[] = {262144, 196608, 163840, 131072, 98304,
                                       65536, 49152, 32768, 16384};
    for (unsigned i = 0; i < sizeof budgets / sizeof budgets[0]; ++i) {
        unsigned want = budgets[i] / CAPTURE_CHUNK_BYTES * CAPTURE_CHUNK_BYTES;
        if (want > chunks_room * CAPTURE_CHUNK_BYTES) want = chunks_room * CAPTURE_CHUNK_BYTES;
        if (want < CAPTURE_MIN_CHUNKS * CAPTURE_CHUNK_BYTES) break;
        staging = malloc(want);
        if (staging) {
            staging_chunks = want / CAPTURE_CHUNK_BYTES;
            break;
        }
    }
    if (!staging) {
        error_code = FM1_CAPTURE_NO_RAM;
        state = FM1_CAPTURE_ERROR;
        done_at = fm1_millis();
        return;
    }
    target_slot = slot;
    capacity_ds = staging_chunks / 10u;
    ring_drops = 0;
    ring_head = ring_tail = 0;
    capturing = 1;                     /* ISR feeds from here on */
    state = FM1_CAPTURE_RECORDING;
}

/* Downmix one ring block to mono and push it through the resampler into the
 * chunk assembler. Control task only. scratch[i] holds absolute mono frame
 * stream_index - MONO_HISTORY + i; output n sits at input position
 * res_base + res_frac/320 and weights CAPTURE_TAPS samples centred there. */
static void ingest(const int16_t *block)
{
    int16_t scratch[MONO_HISTORY + FM1_BLOCK_FRAMES];
    memcpy(scratch, res_hist, sizeof res_hist);
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        int32_t mono = ((int32_t)block[2 * i] + block[2 * i + 1]) >> 1;
        scratch[MONO_HISTORY + i] = (int16_t)mono;
    }
    unsigned first = res_base - (CAPTURE_TAPS / 2 - 1) - (stream_index - MONO_HISTORY);
    while (first + CAPTURE_TAPS <= (unsigned)(MONO_HISTORY + FM1_BLOCK_FRAMES)) {
        const int16_t *taps = resample[res_frac];
        int64_t acc = 0;
        for (unsigned j = 0; j < CAPTURE_TAPS; ++j)
            acc += (int32_t)taps[j] * scratch[first + j];
        acc >>= 15;
        chunk_buf[chunk_fill++] =
            (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
        if (chunk_fill == CAPTURE_CHUNK_FRAMES) {
            fm1_sea_encoder_chunk(&encoder, chunk_buf, CAPTURE_CHUNK_FRAMES, sample_dqt,
                                  staging + (unsigned)staged_chunks * CAPTURE_CHUNK_BYTES);
            staged_chunks++;
            chunk_fill = 0;
        }
        frames_out++;
        res_frac += 441u;          /* each output advances 441/320 inputs */
        res_base += res_frac / (unsigned)CAPTURE_PHASES;
        res_frac %= (unsigned)CAPTURE_PHASES;
        first = res_base - (CAPTURE_TAPS / 2 - 1) - (stream_index - MONO_HISTORY);
    }
    memcpy(res_hist, scratch + FM1_BLOCK_FRAMES, sizeof res_hist);
    stream_index += FM1_BLOCK_FRAMES;
}

static void drain_ring(void)
{
    for (unsigned budget = 0; budget < RING_BLOCKS && ring_tail != ring_head; ++budget) {
        ingest(ring[ring_tail]);
        unsigned next = ring_tail + 1 == RING_BLOCKS ? 0 : ring_tail + 1;
        ring_tail = (uint8_t)next;
    }
}

void fm1_capture_stop(void)
{
    if (state != FM1_CAPTURE_RECORDING) return;
    capturing = 0;                     /* ISR stops feeding */
    drain_ring();
    if (chunk_fill) {                  /* pad the take's tail with silence */
        memset(chunk_buf + chunk_fill, 0, (CAPTURE_CHUNK_FRAMES - chunk_fill) * sizeof chunk_buf[0]);
        fm1_sea_encoder_chunk(&encoder, chunk_buf, chunk_fill, sample_dqt,
                              staging + (unsigned)staged_chunks * CAPTURE_CHUNK_BYTES);
        staged_chunks++;
        chunk_fill = 0;
    }
    if (staged_chunks < CAPTURE_MIN_CHUNKS) {
        cleanup();
        error_code = FM1_CAPTURE_TOO_SHORT;
        state = FM1_CAPTURE_ERROR;
        done_at = fm1_millis();
        return;
    }
    if (groove_samples_capture_commit(staging, staged_chunks)) {
        cleanup();
        error_code = FM1_CAPTURE_BANK_BUSY;
        state = FM1_CAPTURE_ERROR;
        done_at = fm1_millis();
        return;
    }
    state = FM1_CAPTURE_COMMIT;
}

void fm1_capture_poll(void)
{
    if (state == FM1_CAPTURE_RECORDING) {
        drain_ring();
        return;
    }
    if (state == FM1_CAPTURE_COMMIT) {
        groove_samples_capture_poll();     /* drive the Flash machine */
        int r = groove_samples_capture_result();
        if (!r) return;
        unsigned slot = target_slot;
        cleanup();
        error_code = r > 0 ? FM1_CAPTURE_NONE : FM1_CAPTURE_FLASH;
        state = r > 0 ? FM1_CAPTURE_SAVED : FM1_CAPTURE_ERROR;
        target_slot = slot;            /* show where the take landed */
        done_at = fm1_millis();
        return;
    }
    if ((state == FM1_CAPTURE_SAVED || state == FM1_CAPTURE_ERROR) &&
        fm1_millis() - done_at > 3000U) {
        cleanup();
        error_code = FM1_CAPTURE_NONE;
        state = FM1_CAPTURE_IDLE;
    }
}
