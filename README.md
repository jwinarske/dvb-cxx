<!-- SPDX-FileCopyrightText: 2026 Joel Winarske -->
<!-- SPDX-License-Identifier: Apache-2.0 -->
# dvb-cxx

A C++17 player for ATSC/DVB TV tuners on the Linux DVB API. It tunes a digital
TV channel, demuxes the transport stream, decodes the video, and scans it out
**zero-copy** on a hardware plane via
[drm-cxx](https://github.com/jwinarske/drm-cxx). The KMS player binary is
`dvb-kms`.

Built and tested with the **Hauppauge WinTV-dualHD** (USB `2040:826d`, ATSC),
which the mainline kernel drives out of the box (`em28xx` + `lgdt3306a` +
`si2157`) — no proprietary driver or reverse-engineering required. Targets RPi4/5
and other SoCs as well as desktop.

```
WinTV-dualHD ─USB─► em28xx/lgdt3306a (kernel) ─► /dev/dvb/adapterN
   tv::dvb::Frontend   tune SYS_ATSC, 8-VSB, wait FE_HAS_LOCK            [DONE]
        ▼
   /dev/dvb/adapterN/dvr0 ── MPEG-2 Transport Stream ──►
   tv::ts::Demux        PAT/PMT → video PID → PES → MPEG-2 ES            [DONE]
        ▼
   tv::DecoderSource    VAAPI ▸ V4L2 M2M ▸ software (auto-fallback)
        │   NV12 DMA-BUF (zero-copy HW) │ NV12 dumb buffer (software)
        ▼
   tv::KmsPresenter     drm-cxx LayerScene → HW plane → PageFlip loop    [DONE]
```

The decode→present half mirrors
[carlinkit-cxx](https://github.com/jwinarske/carlinkit-cxx)'s `DecoderSource`
pattern; only the front end (a DVB transport stream instead of a USB H.264
dongle) and the codec (MPEG-2/H.262 for ATSC 1.0) differ.

## Status

- ✅ `dvb::Frontend` — DVBv5 tune + lock (ATSC 8-VSB / QAM), channel table.
- ✅ `ts::Demux` — PAT/PMT scan (kernel section filters) + PES→ES. Pure parsers
  in `ts_parse`, unit-tested (`tests/test_ts_parse.cpp`).
- ✅ `KmsPresenter` — drm-cxx scene, aspect-fit video layer, page-flip loop.
- ✅ `DecoderSource` + `create_decoder_source` fallback factory.
- ✅ Software backend — libavcodec decode → libswscale NV12 → DRM dumb buffer.
- ✅ VAAPI backend — libavcodec hwaccel → DRM-PRIME dmabuf → KMS plane,
  zero-copy. Gated on a driver profile check, so it cleanly defers to V4L2 /
  software when the GPU can't decode the codec (e.g. MPEG-2 on modern AMD/Intel).
- ✅ V4L2 M2M backend — wraps `drm::scene::V4l2DecoderSource` (RPi4 / SoC).
- ✅ `--scan` mode (sweep the ATSC table, list locked programs).

## Decode backends

| Backend | When | Codec path |
|---------|------|-----------|
| `vaapi` | GPUs that expose the codec's `VAProfile` | libavcodec + VAAPI → tiled NV12 DMA-BUF, zero-copy plane |
| `v4l2` | embedded SoC (RPi4 MPEG-2 block, Rockchip, …) | stateful V4L2 M2M → NV12 DMA-BUF, zero-copy plane |
| `software` | anywhere (last resort) | libavcodec CPU decode + libswscale → NV12 dumb buffer |

`TVTUNER_DECODER=vaapi|v4l2|software|auto` (default `auto`) selects the backend;
auto tries each in order and uses the first that opens.

> **MPEG-2 / H.262 note:** ATSC 1.0 over-the-air video is MPEG-2. Modern AMD/Intel
> GPUs **dropped fixed-function MPEG-2 decode**, so VAAPI exposes no MPEG-2
> profile there and the chain falls through to the CPU `software` backend. True
> zero-copy MPEG-2 is the V4L2 path on RPi4 / SoCs (or older GPUs that still
> expose `VAProfileMPEG2Main`).

## Build

Requires `libavcodec`, `libavutil`, `libswscale`, `libdrm`, plus drm-cxx's deps
(libdrm, gbm, libinput, libudev, …). C++17 (the default) or C++23.

```sh
git submodule update --init --recursive          # drm-cxx -> third_party/drm-cxx
cmake -S . -B build -G Ninja
cmake --build build -j
ctest --test-dir build                            # unit tests

# build against a local drm-cxx working tree instead of the pinned submodule:
cmake -S . -B build -G Ninja -DDRM_CXX_DIR=/path/to/drm-cxx
# C++23 build:
cmake -S . -B build -G Ninja -DCMAKE_CXX_STANDARD=23
```

The DRM/KMS backend needs **DRM master** — run it from a bare VT (no compositor
on the card) or via libseat. On embedded targets without a compositor it just
works.

```sh
./build/dvb-kms --card /dev/dri/card0 --adapter 0 --channel 7   # ATSC OTA
./build/dvb-kms --codec mpeg2 stream.es                         # file (test)
```

## Finding channels in your area

Predict coverage with [antennaweb.org](https://antennaweb.org) or the FCC DTV
Reception Map (by ZIP). To enumerate what actually locks, use `w_scan2` or
`dvbv5-scan` with a seed table from `dtv-scan-tables`
(`/usr/share/dvb/atsc/us-ATSC-center-frequencies-8VSB`); a built-in `--scan`
mode is planned.
