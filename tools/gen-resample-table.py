#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Regenerate app/capture_sin.h for the live-capture resampler.

The 44100->32000 Hz polyphase coefficients (320 phases x 32 taps,
Blackman-Harris windowed sinc, cutoff 16 kHz) are computed at boot into BSS by
app/capture.c from this 1025-entry quarter-wave sine table (Q15, linear
interpolation between entries). Flash stays small; RAM absorbs the table.

    python3 tools/gen-resample-table.py   # from the repository root
"""
import math

N = 512                        # quarter-wave entries


def main():
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */",
        "/* Generated quarter-wave sine, Q15: entry k = round(32767*sin(pi/2*k/N)).",
        " * app/capture.c builds its polyphase coefficients from this at boot so the",
        " * Regenerate with tools/gen-resample-table.py. */",
        "#ifndef FM1_CAPTURE_SIN_H",
        "#define FM1_CAPTURE_SIN_H",
        "#include <stdint.h>",
        f"enum {{ CAPTURE_SIN_N = {N} }};",
        f"static const int16_t capture_sin[{N} + 1] = {{",
    ]
    for k in range(N + 1):
        v = round(32767 * math.sin(math.pi / 2 * k / N))
        if k and k % 8 == 0:
            lines.append("    ")
        lines[-1] += f"{v},"
    lines += ["};", "#endif"]
    lines = [line.replace(f"enum {{ CAPTURE_SIN_N = {N} }};",
                          f"enum {{ CAPTURE_SIN_N = {N}, CAPTURE_SIN_SHIFT = {N.bit_length() - 1} }};")
             for line in lines]
    with open("app/capture_sin.h", "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote app/capture_sin.h: {N}+1 entries")


if __name__ == "__main__":
    main()
