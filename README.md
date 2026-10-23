# WonderLantern Z9 QEMU

**2026-10-23: shared441 development source update.** See [current changes, whole-workflow evidence and limitations](docs/shared441.md) for 1/1000 bursts, ZR Main10/R3D, the receiver fix and the still-failing pre-release path. This portable rebuild does not inherit older camera qualifications. Bring your own firmware; original ISP/DSP/codec algorithms remain unimplemented.

## Historical shared221 and earlier usage/qualification

[Historical shared221 Z9 two-photo qualification / 旧版双拍验收](docs/photo-qualification221.json)


[Historical shared221 ZR recording qualification / 旧版录制验收](docs/recording-qualification221.json)

**shared221 source update.** The shared devices and functional HE codec now
have complete ZR C1.11 two30-second Main10/R3D recording evidence. This is not
Z9 VIDEO qualification. The current portable Z9 two-photo workflow also passed full NEF/sample/preview and menu-recovery checks. Historical results below retain their revision scope;
current source/portable-build scopes are listed in [shared221](docs/shared221.md).
The companion is [WonderLantern/zr-qemu](https://github.com/WonderLantern/zr-qemu).


[中文](README.zh-CN.md) · [Build](docs/build.md) · [Firmware](docs/firmware.md) · [Usage](docs/usage.md) · [RAW capture](docs/capture.md) · [Architecture](docs/architecture.md) · [Debugging](docs/debugging.md) · [Research notes](docs/research-notes.md) · [Testing](docs/testing.md) · [Publication](docs/publication.md)

An experimental QEMU 10.2.4 machine for **Nikon Z9 C 5.32**, with a local
browser controller, a functional Bayer → ISP → guest buffers → LCD path,
and native full-resolution RAW capture to a virtual NVMe/exFAT card.
This is an independent WonderLantern research and learning project.

**Bring your own firmware.** No Nikon firmware, decrypted proprietary code,
boot ROM, flash dump, key, font, icon bitmap, calibration image, photograph or
camera UI screenshot is included. The tools do not download Nikon firmware.
The original UI appears only when you provide the compatible local payload.

This is a host-side emulator, not a camera-installable modification, a Magic
Lantern port, or a complete EXPEED hardware specification. It has no USB
camera access, updater or flashing tool. Z7 is a separate frozen project;
Use the separately documented ZR companion for its own inputs and bindings. See [NOTICE.md](NOTICE.md).

## Historical shared221 scope

The local research model runs the original English menu and shooting HUD.
The firmware renders ISO/shutter, text/icon outlines and the red no-card mark.
The experimental pixel path consumes a local RGGB input and writes the
firmware's submitted output buffers; LCD composition uses those buffers and
the original graphics overlay. It does not paste an image behind a browser UI.

The shared146 public lossless14 profile passed two released full photos in
one original-firmware session and virtual card. Each **8280×5520, 14-bit NEF**
was followed by fresh live view and two actual PHOTO-menu round trips.
RawPy/LibRaw independently decoded all **91,411,200 RAW samples** and matched
them to each photo's guest producer buffer. All three JPEG previews per photo
matched completed encoder outputs; both F0 streams exclude the packed-size
fallback. See [current evidence](docs/shared146.md#current-public-z9-full-photo-requalification). The older preview-only launcher also
passed three MENU returns with independent LCD pixel comparisons.
See [capture](docs/capture.md) for reproducible commands and model limits,
[testing](docs/testing.md) for the evidence, and [service-core events](docs/service-events.md)
for scheduling assumptions. LCD video frames mean live view, not movie recording.

| Implemented in the experimental profile | Outside the verified scope |
| --- | --- |
| Four AArch64 CPUs, GIC, inferred RAM windows and selected peripherals | Physical EXPEED register specification, Boot ROM or natural power-on |
| Original menu/shooting tasks with explicit external inputs | Complete MCU, lens, CMOS or GPIO protocol |
| Static linear RGGB/u16 input and functional NV12/NV16 output | Nikon ISP algorithms, sensor calibration, accurate AE, AF/PDAF |
| Selected graphics paths, coverage, alpha and LCD scanout | Complete GPU implementation or measured silicon timing |
| Shutter/CCOMP peer, full RAW transport, JPEG previews and firmware-written NEF on virtual NVMe/exFAT | Physical CFexpress identity, Original Nikon algorithms, accepted movie recording, RAW pre-capture or mechanical timing |
| Fresh live view after a photo and two post-shot MENU round trips | Indefinite stability, repeated burst capture or physical-camera qualification |

Several resource bodies are missing. The profile supplies small **synthetic
virtual ROM inputs**, one parameter body derived at runtime from the user's
firmware defaults, dummy SPI receive values and explicit external status
events. These assumptions are recorded locally. The host uses hardware
breakpoints to service the incomplete MCU/GPIO model. It does not patch guest
instructions, PC, readiness flags, OS wait results or watchdogs.

## Quick start

Linux, Python 3.12+, C/C++17 toolchains and about 6 GB of available memory are a
practical starting point. On Windows explicitly select your intended WSL
distribution. This project's environment is `ubuntu-noble-nikon`; the
unrelated coursework distribution `ubuntu-noble` is excluded by the tools.

```sh
git clone https://github.com/WonderLantern/z9-qemu.git
cd z9-qemu
sudo apt-get update
sudo apt-get install build-essential ninja-build patch pkg-config python3-venv \
    libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev libjpeg-dev libx265-dev libx264-dev exfatprogs
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
python tools/build.py --work-dir "$HOME/wonderlantern-z9-build" --jobs 2
python tools/make_cmos.py
python tools/run.py --firmware /absolute/path/to/z9-main-payload.bin
```

Open **http://127.0.0.1:8789/**. Wait for the original live-view image and
nonzero RAW/LCD counters. MENU and half-shutter inputs go through the original
MCU receive path. Use **Half shutter hold** to leave the menu for shooting,
and **Release** to release that input. Ctrl+C stops the owned emulator.

The default input is locally generated color bars with a luminance ramp. To
use your own image, run `python tools/make_cmos.py --image /path/to/photo.jpg`
before starting the emulator. The 640×480, 12-bit test input is a convenience;
it is **not a claim about Z9's physical sensor stream**.

For a full-resolution RAW capture, stop the preview process and use a fresh
run directory on a Linux filesystem:

```sh
python -m pip install -r requirements-capture.txt
python tools/make_cmos.py --width 8256 --height 5504 --bits 14 \
    --output .local/z9-14bit.raw
python tools/capture.py --firmware /absolute/path/to/z9-main-payload.bin \
    --output "$HOME/wonderlantern-z9-runs/capture-001" --audit
```

This command drives original input contacts, selects the verified RAW profile,
saves one photograph, waits for fresh live view, opens and returns from two
menus, then stops QEMU before extracting and auditing its actual NEF. It can
take several minutes. The browser controls remain preview/menu-only; they
do not expose shutter release or REC. See [RAW capture](docs/capture.md).

The required extracted MAIN payload is **81,823,629 bytes**, SHA-256:

```text
6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd
```

An upgrade package is not the same file. This repository supplies no
decryption key or extraction service. See [firmware input](docs/firmware.md).

## Layout and contributions

`qemu/` contains the board, pixel code and upstream integration. `tools/`
contains the builder, controller, virtual peers and local input generators.
`tests/` contains synthetic, firmware-free tests. `docs/` explains setup,
model assumptions, evidence and research techniques.

Read [CONTRIBUTING.md](CONTRIBUTING.md) before attaching logs or creating a PR.
Runtime folders can contain proprietary bytes even when the repository is
source-only. Keep them local. Source licensing is GPL-2.0-or-later except as
documented in upstream licensing notices.

当前 shared146 的共享输入/所有权模型、整轮录像失败结果及复用更正见 [shared146 notes](docs/shared146.md)。H.265／R3D 录像和原机算法仍未验收。
