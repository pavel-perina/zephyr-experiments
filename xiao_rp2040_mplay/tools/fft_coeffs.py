#!/usr/bin/env python3
"""Generate fixed-point tables for a 64-point Q1.15 radix-2 DIT FFT, for
the OLED spectrum analyzer's replacement of the IIR filter bank.

Stdlib-only, run directly with plain python3 - no coloraide/venv needed
(same as spectrum_coeffs.py).

Precomputes everything that doesn't depend on the audio signal: twiddle
factors (cos/sin), the bit-reversal permutation, and which of the 33
useful FFT bins (0..N/2 at the decimated rate) belongs to which of the 16
display bands. None of this needs to happen at runtime - the FFT itself
(spectrum.c) is pure fixed-point multiply/add/shift.

Also emits a second, "wide" (Winamp-style) band layout: fewer bands with
log-spaced edges, each covering a whole range of bins (fft_wide_lo/hi[]) so
spectrum.c can take the max over it - see wide_band_ranges().

Usage:
    python3 fft_coeffs.py [--n 64] [--fs 16000] [--bands 16]
                           [--fmin 80] [--fmax 8000]
                           [--wide-bands 32] [--wide-fmin 60]
                           [--out ../src/fft_coeffs.h]

The current src/fft_coeffs.h was generated with:
    python3 fft_coeffs.py --n 512 --bands 128 --wide-bands 32 --wide-fmin 60
"""
import argparse
import math
from pathlib import Path

Q15 = 1 << 15


def to_q15(x):
    v = round(x * Q15)
    # clamp: cos/sin never exceed +-1, but +1.0 rounds to exactly Q15 =
    # 32768, one past int16_t's max (32767) - clamp that single edge case.
    return max(-32768, min(32767, v))


def bit_reverse_table(n):
    bits = n.bit_length() - 1
    return [int(f"{i:0{bits}b}"[::-1], 2) for i in range(n)]


def twiddle_tables(n):
    # W_N^k = e^{-j*2*pi*k/N}, k = 0..N/2-1 (only half needed by symmetry
    # in the DIT butterfly's per-stage twiddle stride).
    re = [to_q15(math.cos(2 * math.pi * k / n)) for k in range(n // 2)]
    im = [to_q15(-math.sin(2 * math.pi * k / n)) for k in range(n // 2)]
    return re, im


def band_to_bin_table(n, fs, bands, fmin, fmax):
    # Inverse of the natural "which band does each bin belong to" mapping:
    # with only n/2+1 linearly-spaced bins (fs/n apart) covering a log-
    # spaced band range, several low bands can fall entirely between two
    # bin frequencies and never get a bin assigned - confirmed empirically
    # (host harness against real playback: several low bands sat at
    # exactly zero the entire time, not just quiet). Going the other way -
    # each band picks its single *nearest* bin by centre frequency - can't
    # have that gap: every band always has some nearest bin.
    #
    # At a high enough band count (128 was tried against a 512-point FFT's
    # 257 bins) plain nearest-bin produces the opposite problem instead:
    # many low bands collide on the *same* bin - confirmed empirically,
    # 128 requested bands only used 87 distinct bins, the low ~30 bands
    # clumped into blocks of 5-9 identical values. Forcing each band to a
    # bin strictly greater than the previous band's - bump forward past
    # whatever's already claimed instead of colliding - fixes that: where
    # log-spaced targets are packed tighter than the bin resolution (the
    # low end), every band claims the next unclaimed bin in sequence,
    # which is exactly linear spacing; where targets are naturally spread
    # further apart than one bin (the high end), nothing needs bumping and
    # it stays log-spaced. Net effect: log where the FFT has the
    # resolution to support it, linear exactly where it doesn't - "semi-
    # linear" without needing a separate linear/log split point to tune.
    ratio = fmax / fmin
    table = []
    prev_bin = 0   # 0 is DC, never a valid assignment - see the floor below
    for i in range(bands):
        freq = fmin * (ratio ** (i / (bands - 1)))
        bin_idx = round(freq * n / fs)
        # Never bin 0 (DC, not a frequency) and never <= the previous
        # band's bin (would duplicate it) - both a floor of prev_bin + 1.
        bin_idx = max(prev_bin + 1, bin_idx)
        bin_idx = min(n // 2, bin_idx)
        table.append(bin_idx)
        prev_bin = bin_idx
    return table


def wide_band_ranges(n, fs, bands, fmin, fmax):
    # Winamp-style layout: `bands` bands with edges evenly spaced in log
    # frequency (equal share of octaves each), every band owning the whole
    # [lo, hi] bin range between its edges instead of one bin - noise-like
    # content (hi-hats, cymbals) is spread across many bins, so a single
    # bin catches only a fraction of it. Same bump-forward rule as
    # band_to_bin_table() at the low end, where a band is narrower than one
    # bin: each band starts strictly after the previous one ends, so low
    # bands degrade to one bin each (linear) instead of sharing bins.
    ratio = fmax / fmin
    edges = [fmin * (ratio ** (i / bands)) for i in range(bands + 1)]
    lo_tab, hi_tab = [], []
    prev_hi = 0   # bin 0 is DC - never used
    for i in range(bands):
        lo = max(prev_hi + 1, round(edges[i] * n / fs))
        hi = max(lo, round(edges[i + 1] * n / fs) - 1)
        lo = min(n // 2, lo)
        hi = min(n // 2, hi)
        lo_tab.append(lo)
        hi_tab.append(hi)
        prev_hi = hi
    assert all(l <= h for l, h in zip(lo_tab, hi_tab))
    # Octaves above fmin of each band's centre (geometric mean of its
    # actual bin range's edges), Q8 - for spectrum.c's tilt compensation.
    oct_tab = []
    for lo, hi in zip(lo_tab, hi_tab):
        f_lo = lo * fs / n
        f_hi = (hi + 1) * fs / n
        centre = math.sqrt(f_lo * f_hi)
        oct_tab.append(max(0, round(math.log2(centre / fmin) * 256)))
    assert all(lo_tab[i] > hi_tab[i - 1] for i in range(1, bands)), \
        "too many wide bands for this FFT size - bands ran out of bins"
    return lo_tab, hi_tab, oct_tab


def emit_header(n, twiddle_re, twiddle_im, bitrev, band_to_bin, wide_lo, wide_hi, wide_oct,
                out_path):
    bands = len(band_to_bin)
    lines = [
        "/* Generated by tools/fft_coeffs.py - do not edit by hand. */",
        "#ifndef FFT_COEFFS_H",
        "#define FFT_COEFFS_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define FFT_N {n}",
        f"#define FFT_STAGES {n.bit_length() - 1}",
        f"/* Must match spectrum.h's SPECTRUM_BANDS. */",
        f"#define FFT_BANDS {bands}",
        "",
        "/* Q1.15 fixed-point: cos/-sin of 2*pi*k/N, k = 0..N/2-1. */",
    ]
    lines.append(f"static const int16_t fft_twiddle_re[FFT_N / 2] = {{")
    lines.append("\t" + ", ".join(str(v) for v in twiddle_re) + ",")
    lines.append("};")
    lines.append(f"static const int16_t fft_twiddle_im[FFT_N / 2] = {{")
    lines.append("\t" + ", ".join(str(v) for v in twiddle_im) + ",")
    lines.append("};")
    lines.append("")
    lines.append("/* Bit-reversal permutation for the in-place DIT FFT. */")
    lines.append(f"static const uint16_t fft_bitrev[FFT_N] = {{")
    lines.append("\t" + ", ".join(str(v) for v in bitrev) + ",")
    lines.append("};")
    lines.append("")
    lines.append("/* Each display band's single nearest FFT bin, by centre frequency -")
    lines.append(" * see band_to_bin_table()'s docstring for why this is the direction")
    lines.append(" * that can't leave a band with no bin (the reverse mapping can). */")
    lines.append(f"static const uint16_t fft_band_bin[FFT_BANDS] = {{")
    lines.append("\t" + ", ".join(str(v) for v in band_to_bin) + ",")
    lines.append("};")
    lines.append("")
    lines.append("/* Wide (Winamp-style) layout: band i covers bins fft_wide_lo[i]..")
    lines.append(" * fft_wide_hi[i] inclusive, log-spaced edges - see wide_band_ranges(). */")
    lines.append(f"#define FFT_WIDE_BANDS {len(wide_lo)}")
    lines.append(f"static const uint16_t fft_wide_lo[FFT_WIDE_BANDS] = {{")
    lines.append("\t" + ", ".join(str(v) for v in wide_lo) + ",")
    lines.append("};")
    lines.append(f"static const uint16_t fft_wide_hi[FFT_WIDE_BANDS] = {{")
    lines.append("\t" + ", ".join(str(v) for v in wide_hi) + ",")
    lines.append("};")
    lines.append("/* Octaves above --wide-fmin of each band's centre, Q8 (tilt compensation). */")
    lines.append(f"static const uint16_t fft_wide_oct_q8[FFT_WIDE_BANDS] = {{")
    lines.append("\t" + ", ".join(str(v) for v in wide_oct) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    out_path.write_text("\n".join(lines) + "\n")
    print("wrote", out_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--n", type=int, default=64)
    ap.add_argument("--fs", type=float, default=16000.0)
    ap.add_argument("--bands", type=int, default=16)
    ap.add_argument("--fmin", type=float, default=80.0)
    ap.add_argument("--fmax", type=float, default=8000.0)
    ap.add_argument("--wide-bands", type=int, default=32)
    ap.add_argument("--wide-fmin", type=float, default=60.0)
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "../src/fft_coeffs.h")
    args = ap.parse_args()

    twiddle_re, twiddle_im = twiddle_tables(args.n)
    bitrev = bit_reverse_table(args.n)
    band_to_bin = band_to_bin_table(args.n, args.fs, args.bands, args.fmin, args.fmax)
    wide_lo, wide_hi, wide_oct = wide_band_ranges(args.n, args.fs, args.wide_bands,
                                                   args.wide_fmin, args.fmax)
    emit_header(args.n, twiddle_re, twiddle_im, bitrev, band_to_bin, wide_lo, wide_hi, wide_oct,
                args.out)


if __name__ == "__main__":
    main()
