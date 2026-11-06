#!/usr/bin/env python3
"""Builds src/audio/hrtf_data.inc, the headphone HRTF table, from the SADIE II database.

    python -I tools/hrtf/build_hrtf.py D1.zip [--out src/audio/hrtf_data.inc]

D1.zip is subject D1 (Neumann KU100 dummy head) of the SADIE II database, University of York, Apache-2.0:
https://zenodo.org/records/12092466 (version 2-2, md5 468f0fce29c2f5880627c571b73e64c3). The measurements stay out of
the repo; only this reduction of them is committed. Needs numpy and scipy. docs/AUDIO.md has what the table is for.

What it does to the 48 kHz 24-bit impulse responses, in order:
  1. Keeps a grid of about 9 degrees: rings every 15 degrees of elevation, each with fewer points toward the poles.
  2. Equalises for the diffuse field: divides out what all directions share (the head, the microphones, the loudspeaker),
     leaving what changes with direction. Below 250 Hz, where a head barely affects sound, the response is flat.
     Each ear's mean power over the sphere becomes 1/2, so a sound carries the power X3DAudio's panning gives it.
  3. Turns each ear into a minimum-phase filter of 64 taps.
  4. Stores, per direction, the interaural time difference those two filters still lack: the measured interaural phase
     minus the filters' own, as a delay averaged over 150 Hz-1.5 kHz, where hearing uses it. The filters' share isn't
     small: the shadowed far ear's minimum-phase filter runs late at low frequencies. A difference taken from the
     impulse responses alone misses that, and rendered 135 us too much at the side on a bass-heavy sound.
"""

import argparse
import io
import re
import sys
import zipfile
from pathlib import Path

import numpy as np
from scipy.io import wavfile

RATE = 48000
TAPS = 64
FFT = 2048
ELEVATIONS = [-90, -75, -60, -45, -30, -15, 0, 15, 30, 45, 60, 75, 90]
HORIZON_POINTS = 40
FLAT_BELOW_HZ = 250.0
ITD_BAND_HZ = (150.0, 1500.0)
SMOOTHING_OCTAVES = 1 / 3
EQ_LIMIT_DB = 18.0
NAME = re.compile(r"D1_HRIR_WAV/48K_24bit/azi_(\d+,\d+)_ele_(-?\d+,\d+)\.wav$")


def load(zip_path):
    """{(azimuth, elevation): array (samples, 2)}, azimuth growing to the left as the files are labelled."""
    measured = {}
    with zipfile.ZipFile(zip_path) as z:
        for name in z.namelist():
            m = NAME.search(name)
            if m:
                rate, data = wavfile.read(io.BytesIO(z.read(name)))
                if rate != RATE or data.ndim != 2 or data.shape[1] != 2:
                    raise SystemExit(f"{name}: expected stereo at {RATE} Hz")
                key = float(m[1].replace(",", ".")), float(m[2].replace(",", "."))
                measured[key] = data.astype(np.float64) / 2**31
    if not measured:
        raise SystemExit(f"{zip_path}: no 48 kHz HRIRs (is this SADIE II subject D1?)")
    return measured


def grid(measured):
    """The kept rings: [(elevation, [impulse response per evenly spaced azimuth])], and the largest azimuth error."""
    rings, worst = [], 0.0
    for elevation in ELEVATIONS:
        available = np.array(sorted(a for a, e in measured if e == elevation))
        count = max(1, round(HORIZON_POINTS * np.cos(np.radians(elevation))))
        if abs(elevation) == 90:
            count = 1  # one point in space, however many labels it has
        points = []
        for i in range(count):
            azimuth = 360.0 * i / count
            distance = np.abs((available - azimuth + 180.0) % 360.0 - 180.0)
            nearest = available[np.argmin(distance)]
            worst = max(worst, float(distance.min()))
            points.append(measured[(float(nearest), float(elevation))])
        rings.append((elevation, points))
    return rings, worst


def interaural_delay_us(left, right, freqs):
    """Microseconds the left ear lags the right at each of `freqs`, from the phase between the ears."""
    bins = np.fft.rfftfreq(FFT, 1 / RATE)
    cross = np.fft.rfft(left, FFT) * np.conj(np.fft.rfft(right, FFT))
    phase = np.unwrap(np.angle(cross))  # slow enough at these bins: under 0.2 rad per bin for any head
    return -np.interp(freqs, bins, phase) / (2 * np.pi * freqs) * 1e6


def smooth(power, freqs):
    """Fractional-octave smoothing of a power spectrum."""
    out = np.empty_like(power)
    half = 2 ** (SMOOTHING_OCTAVES / 2)
    for i, f in enumerate(freqs):
        band = (freqs >= f / half) & (freqs <= f * half) if f > 0 else freqs == 0
        out[i] = power[band].mean() if band.any() else power[i]
    return out


def minimum_phase(magnitude):
    """The minimum-phase impulse response of a magnitude spectrum (FFT // 2 + 1 bins), by the real cepstrum."""
    cepstrum = np.fft.irfft(np.log(np.maximum(magnitude, 1e-9)), FFT)
    fold = np.zeros(FFT)
    fold[0] = cepstrum[0]
    fold[1 : FFT // 2] = 2 * cepstrum[1 : FFT // 2]
    fold[FFT // 2] = cepstrum[FFT // 2]
    return np.fft.irfft(np.exp(np.fft.rfft(fold)), FFT)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zip", type=Path)
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[2] / "src/audio/hrtf_data.inc")
    args = ap.parse_args()

    measured = load(args.zip)
    rings, azimuth_error = grid(measured)
    directions = [(e, 360.0 * i / len(p), h) for e, p in rings for i, h in enumerate(p)]
    print(f"{len(measured)} measurements, keeping {len(directions)}; azimuth off by at most {azimuth_error:.2f} deg")

    # Which channel is which ear, and which way azimuth runs: from the data, not the label.
    def level(key, ear):
        return float(np.sum(measured[key][:, ear] ** 2))

    if not level((90.0, 0.0), 0) > level((90.0, 0.0), 1) > 0:
        raise SystemExit("azimuth 90 should be loudest in channel 0, the left ear")

    freqs = np.fft.rfftfreq(FFT, 1 / RATE)
    spectra = np.abs(np.fft.rfft(np.stack([d[2] for d in directions]), FFT, axis=1))  # (direction, bin, ear)

    # Each ring stands for the band of the sphere around it, shared among its points.
    weights = np.empty(len(directions))
    at = 0
    for elevation, points in rings:
        lo, hi = np.radians(max(elevation - 7.5, -90)), np.radians(min(elevation + 7.5, 90))
        weights[at : at + len(points)] = (np.sin(hi) - np.sin(lo)) / len(points)
        at += len(points)
    weights /= weights.sum()
    diffuse = smooth(np.einsum("d,dbe->b", weights, spectra**2) / 2, freqs)

    # Half the power in each ear. Limited, so a dip in the measurement doesn't become a spike of gain.
    gain = 1 / np.sqrt(2 * diffuse)
    reference = np.median(gain[(freqs > 500) & (freqs < 2000)])
    gain = reference * np.clip(gain / reference, 10 ** (-EQ_LIMIT_DB / 20), 10 ** (EQ_LIMIT_DB / 20))

    # Under FLAT_BELOW_HZ both ears get the same plain level; a fade over an octave joins it to the measured part.
    fade = np.clip(np.log2(np.maximum(freqs, 1.0) / (FLAT_BELOW_HZ / 2)), 0.0, 1.0)
    flat = np.sqrt(0.5)

    filters = np.empty((len(directions), 2, TAPS))
    itds = np.empty(len(directions))
    stored = np.empty(len(directions))
    itd_error = np.empty(len(directions))
    window = np.ones(TAPS)
    itd_freqs = np.geomspace(*ITD_BAND_HZ, 48)  # even in octaves: the low end counts as much as the high
    window[-8:] = np.cos(np.linspace(0, np.pi / 2, 8)) ** 2
    errors = []
    band = (freqs >= 200) & (freqs <= 16000)
    for d, (elevation, azimuth, hrir) in enumerate(directions):
        for ear in (0, 1):
            magnitude = flat * (1 - fade) + spectra[d, :, ear] * gain * fade
            impulse = minimum_phase(magnitude)
            filters[d, ear] = impulse[:TAPS] * window
            kept = np.abs(np.fft.rfft(filters[d, ear], FFT))
            errors.append(20 * np.log10(np.maximum(kept[band], 1e-9) / np.maximum(magnitude[band], 1e-9)))
        measured_delay = interaural_delay_us(hrir[:, 0], hrir[:, 1], itd_freqs)
        missing = measured_delay - interaural_delay_us(filters[d, 0], filters[d, 1], itd_freqs)
        itds[d] = measured_delay.mean()
        stored[d] = missing.mean()
        itd_error[d] = np.abs(missing - stored[d]).max()
    errors = np.abs(np.array(errors))
    print(f"truncation to {TAPS} taps, 200 Hz-16 kHz: mean {errors.mean():.3f} dB, 99th percentile "
          f"{np.percentile(errors, 99):.2f} dB, worst {errors.max():.2f} dB")

    power = np.sum(filters**2, axis=(1, 2))
    print(f"power per direction, dB around the sphere's mean: "
          f"{10 * np.log10(power.min() / np.average(power, weights=weights)):.1f} to "
          f"{10 * np.log10(power.max() / np.average(power, weights=weights)):.1f}; "
          f"mean {10 * np.log10(np.average(power, weights=weights)):.2f} dB")
    for elevation, azimuth in [(0, 0), (0, 90), (0, 180), (0, 270), (90, 0), (-90, 0)]:
        d = next(i for i, x in enumerate(directions) if x[0] == elevation and abs(x[1] - azimuth) < 0.01)
        print(f"  el {elevation:4d} az {azimuth:3d}: ITD {itds[d]:7.1f} us, stored {stored[d]:7.1f} us, "
              f"off by at most {itd_error[d]:5.1f} us across the band")
    print(f"delay left over across the band, all directions: median {np.median(itd_error):.1f} us, "
          f"worst {itd_error.max():.1f} us")

    scale = np.abs(filters).max() / 32767.0
    quantised = np.round(filters / scale).astype(np.int16)
    out = [
        "// Generated by tools/hrtf/build_hrtf.py. Do not edit.",
        "// Derived from subject D1 (Neumann KU100) of the SADIE II database, University of York, licensed under the",
        "// Apache License 2.0 (tools/hrtf/LICENSE-SADIE-II.txt). Changed from the original: reduced to this grid,",
        "// diffuse-field equalised, converted to minimum phase, truncated, and the time differences separated.",
        "// Armstrong, Thresh, Murphy, Kearney, Applied Sciences 8(11):2029, 2018, doi:10.3390/app8112029.",
        "",
        f"constexpr uint32_t k_hrtf_rate = {RATE};",
        f"constexpr uint32_t k_hrtf_taps = {TAPS};",
        f"constexpr float k_hrtf_scale = {scale:.9e}f;",
        "",
        "// Elevation in degrees and points, evenly spaced from azimuth 0 toward the left.",
        "constexpr DataRing k_hrtf_rings[] = {",
    ]
    out += [f"    {{{float(e):.1f}f, {len(p)}}}," for e, p in rings]
    out += ["};", "", "// Microseconds the left ear lags the right on top of the filters' own, per direction.",
            "constexpr int16_t k_hrtf_itd_us[] = {"]
    values = [str(int(round(v))) for v in stored]
    out += ["    " + ", ".join(values[i : i + 16]) + "," for i in range(0, len(values), 16)]
    out += ["};", "", "// Per direction, the left ear's taps, then the right's.", "constexpr int16_t k_hrtf_filters[] = {"]
    flat_values = [str(int(v)) for v in quantised.reshape(-1)]
    out += ["    " + ",".join(flat_values[i : i + 32]) + "," for i in range(0, len(flat_values), 32)]
    out += ["};", ""]
    args.out.write_text("\n".join(out), encoding="ascii", newline="\n")
    print(f"wrote {args.out} ({args.out.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    sys.exit(main())
