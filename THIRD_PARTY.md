# Third-party code

## Drum voices (`poly_drums.c`) – MIT

Taken from the 808 kit of the FM1 Open SDK (`third_party/synth/drum808.c`): DaisySP / Plaits bass and snare
excitation, metallic noise, and the FM1 supplementary percussion. Changed for Polycercle: one kit per circle, direct
trigger of any of the 16 sounds with per-circle tune / decay / tone / drive, step transposition and gate, mono render,
and every per-sample constant recomputed for the host's sample rate.

Copyright (c) 2020 Electrosmith, Corp. (DaisySP).
Copyright (c) 2016 Emilie Gillet (Plaits).
FM1 modifications: Copyright (c) 2026 珠海笙科智能科技有限公司 (M-VAVE).
Polycercle changes: Copyright (c) 2026 Polycercle.

```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## VIBE FM SDK – Apache-2.0

The firmware is compiled together with `sdk/src/vibe_sdk.c` and `sdk/src/vibe_font.c` of the VIBE FM SDK,
Copyright 2026 Sven Trogus (zvenson), under the Apache License 2.0 (see the SDK's `sdk/LICENSE` and `sdk/NOTICE`).
