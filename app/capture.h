/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_CAPTURE_H
#define FM1_CAPTURE_H
#include <stdint.h>
#include "fm1.h"
#include "fm1_block.h"

/* Live sample capture: Bluetooth A2DP audio is recorded into RAM (SEA5
 * compressed, 32 kHz mono) and committed to the first free sample-bank slot
 * when the take stops. The resampler and encoder run on the control task;
 * only the block-sized feed below belongs to the audio interrupt. */

enum fm1_capture_state {
    FM1_CAPTURE_IDLE,       /* nothing armed; results of the last take are gone */
    FM1_CAPTURE_RECORDING,  /* feeding from the audio interrupt */
    FM1_CAPTURE_COMMIT,     /* stopped; writing the bank, engine briefly muted */
    FM1_CAPTURE_SAVED,      /* take landed in fm1_capture_slot(); poll again to idle */
    FM1_CAPTURE_ERROR       /* fm1_capture_error() says why; poll again to idle */
};

enum fm1_capture_error {
    FM1_CAPTURE_NONE = 0,
    FM1_CAPTURE_NO_BT,      /* no Bluetooth audio link when arming */
    FM1_CAPTURE_BANK_BUSY,  /* sample bank validating or receiving an import */
    FM1_CAPTURE_BANK_FULL,  /* all eight pads hold samples */
    FM1_CAPTURE_NO_RAM,     /* staging buffer could not be allocated */
    FM1_CAPTURE_TOO_SHORT,  /* under 160 ms - not worth a slot */
    FM1_CAPTURE_FLASH       /* Flash write/read failed; bank may be marked bad */
};

void fm1_capture_arm(void);      /* control task: start recording */
void fm1_capture_stop(void);     /* control task: stop and commit */
void fm1_capture_cancel(void);   /* control task: discard the take */
void fm1_capture_poll(void);     /* control task: drain, resample, encode, commit */
enum fm1_capture_state fm1_capture_state(void);
enum fm1_capture_error fm1_capture_error(void);
unsigned fm1_capture_slot(void);         /* destination pad 0..7; 255 unknown */
unsigned fm1_capture_ms(void);           /* captured audio so far */
unsigned fm1_capture_deciseconds(void);  /* capacity of this take, 0.1 s units */
unsigned fm1_capture_drops(void);        /* blocks lost to a stalled control task */

/* Audio interrupt only, once per block. stereo holds FM1_BLOCK_FRAMES
 * interleaved L/R frames at the board rate; NULL records silence (source
 * underrun) so the take stays aligned with what was heard. Cheap no-op unless
 * recording. Must stay in audio SRAM together with its callees. */
FM1_AUDIO_CODE void fm1_capture_feed(const int16_t *stereo, unsigned frames);
#endif
