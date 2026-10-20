/* SPDX-License-Identifier: MIT
 * Fixed SEA5 quantizer tables: Copyright (c) 2025 Dani Biro.
 * FM1 pad mapping: Copyright (c) 2026 M-VAVE.
 * See THIRD_PARTY.md for the SEA source and MIT terms. */
#include "sample_dqt.inc"
static FM1_DSP_DATA const uint8_t sample_notes[SAMPLE_DRUM_PADS] = {36,38,37,41,47,48,42,46};
/* MIDI notes 35..48; all other notes are silent for this eight-pad bank. */
static FM1_DSP_DATA const uint8_t sample_map[14] = {0,0,2,1,255,255,3,6,3,255,4,7,4,5};
