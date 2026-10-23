# Current shared441 scope

See [shared441](shared441.md) and `source-snapshot.json` for the current development build. Camera qualifications below retain their original revision and executable hashes. They are not current-build regression results.

## Historical records

# Current: shared221

See [current recording results and limits](shared221.md). The older sections below retain their original build scopes.

# Historical: shared146

See [shared146](shared146.md) / [中文](shared146.zh-CN.md) for current source changes, validation and native recording failures. Older sections retain their historical build scopes.

# Historical: shared144

See [shared144](shared144.md) for the current source delta, native failures, independent package checks and reuse limits. The sections below retain their historical build scopes.

# Historical: shared138

See [the shared138 update](shared138.md) for current control changes, validation and limits. The shared136 section below preserves its historical scope.

# Shared EXPEED7 development snapshot — 2026-10-13

This source snapshot updates the shared Z9 C5.32 / ZR C1.11 model to the
private research revision **shared136**. It is published at the user's explicit
request before movie qualification. It is not an accepted H.265/R3D release.
The older Z7 project remains frozen. The private source/input trees are not
Git dependencies of these standalone repositories.

## What changed since the previous Z9 publication

- Shared full-resolution image transport, post-shot ownership and repeated
  photo/menu recovery work, including a functional open-source F0 lossless14
  encoder adapted from [RdWing/z8-qemu](https://github.com/RdWing/z8-qemu).
  Copyright and GPL notices are retained. HE/HE* and RAW pre-capture are open.
- ZR subclass, graphics/LCD banks, landscape orientation inputs and original
  English menus. English uses a declared four-byte cached setting input;
  it is explicitly recorded as a RAM preference change, not unmodified NVM.
- Source/receiver/decoder lifetimes, finite frames, STOP/END/W1C ownership,
  restart transitions, audio configuration and silence input.
- Opt-in Wave5 command/data transport with public host x265 HEVC Main10 and
  x264 AVC backends. Diagnostic buffers really encode and independently decode;
  original codec microcode does not execute. Camera movie delivery is incomplete.
- Opt-in DSP power/mailbox/session transport, workspace handling and generic
  FAST9/SAD motion operations. These are functional substitutes, not CEVA cores.
- The latest retired-idle reset handles only exact old-generation pending
  causes with a stopped source and no live packet, decoder, ISP or codec owner.
  It does not inject ISR acknowledgements, callback results or pixel data.

## Verification and limits

| Evidence | Result and scope |
| --- | --- |
| Earlier standalone Z9 public run 564 | Original shutter, full RAW transport, fresh live view and two real PHOTO menu round trips; all 45,705,600 samples matched LibRaw. This tested the previous published revision, not every new opt-in. |
| Shared lossless14 photo runs Z9 572 / ZR 424 | Four actual NEFs; LibRaw and Adobe decoding matched all 140,408,320 producer samples. This is functional format/pixel acceptance, not execution of Nikon's original compression algorithm. |
| Earlier shared136 device regressions | 26 stopped diagnostic guests, 162 checks covering both model identities and guarded idle reset ownership. Original reset routines were exercised only in labeled synthetic diagnostics. |
| ZR shutter recipe contract | 76 checks. VIDEO HALF focuses; held HALF+FULL starts, a later FULL stops. Main shutter is not aliased to the movie button. |
| Earlier native Main10 run 505 | 6,898 complete trace records; 98 input states consumed. One idle pulse reset retired old causes; the second START timed out. 0 codec picture bytes / 0 pictures. |
| Earlier native R3D run 506 | 418 complete trace records; 82 input states consumed; STOP timed out. 0 codec picture bytes / 0 pictures. AVC initialization is not R3D encoding. |
| Both earlier virtual cards | Clean read-only exFAT audit, only NIKON001.DSC. No camera-written playable movie. |

Native source forwarding reached an indirect call that returned 1; that is
request delivery, not image acceptance. Some earlier hook names called
configuration/capacity queries “buffer/slot acquisition”; that interpretation
is superseded. Unbound observations mean **unobserved**, not zero activity.
Full camera movie acceptance still requires actual stored files, independent
full decode, two START/STOP cycles, fresh live view and menu recovery.

**Original ISP, DSP, H.265, RAW compression and JPEG algorithms are all
unaccepted.** Generic Bayer processing, x265/x264, WLVR/zlib transport, the
functional lossless14 encoder and libjpeg must not be described as original
Nikon algorithms. WLVR is not NEF, REDCODE or R3D.

XM6 materials are available locally, but SDK1.1.0.F equivalence to the
firmware RTL1.2.0 has not been established. Complete NeuPro programming and
arithmetic documentation is missing. Local neural-network reproduction is
**inconclusive**; original inference has not been verified. Vendor materials
and reference R3D recordings are not distributed.

## Reuse across models

Both public repositories carry the same board and helper headers. ZR is a
small QOM subclass; addresses in its native host peers are separately bound
to the C1.11 payload hash. Reuse device semantics and ownership invariants,
then recover model-specific MCU/resource/boot bindings. Do not copy task
addresses or resource bytes from another camera merely because it shares
EXPEED7. Run the whole photo/live-view/menu or movie START/STOP/card/decode
workflow after related changes. Synthetic device checks cannot replace it.

Keep guest-produced RAM, source buffers, headers, packets, card files and
independently decoded pixels separate in evidence. Keep failures as failures.
No physical camera timing, sensor calibration, lens/AF/PDAF, natural boot ROM,
complete peripherals, burst stability or pre-capture qualification is claimed.

## Publication contents

Only authored emulator/tool source, protocol/address metadata, public-format
constants, docs and public upstream patches are included. No firmware,
decrypted code, vendor DSP/microcode, keys, resource bytes, fonts, icons,
calibration, images, camera screenshots, cards, runtime logs or private history
are included. Firmware and any required ZR captured resources remain local
user inputs. An inventory/hash/history audit catches unintended additions;
it is not a legal oracle. See NOTICE.md and docs/publication.md.

The public Z9 `tools/capture.py --profile lossless14 --audit` ports the latest two-photo native workflow and full per-file/sample auditor. The default compatibility profile retains its previous transport scope. See [capture](capture.md).


## Earlier shared136 standalone packaging checks

Both standalone overlays compiled with the public QEMU builder. The shared
Python suite (13 tests), C pixel checks, firmware-free realization and 13
authored service-core event checks passed. The public binary is
`61b21537e2f5cfc6a1d6fb18a727c81539c1c219b56908dc4a5e4b18448915f4`;
the private research reference binary has a different fingerprint. Source
and compiled-source fingerprints are separate in source-snapshot.json.

A bounded 20-second test of the first portable ZR legacy English/menu recipe
ran original instructions, executed 339 checks, generated 19 LCD frames and
stopped its owned QEMU. It failed to consume the complete key sequence; no
video frames were generated. This is partial boot/host-port evidence, not
successful live-view/photo acceptance. The default preview now reuses the
current photo preparation/device configuration, ending at held HALF. Its
whole native workflow still needs separate qualification. No new whole native
photo or movie acceptance is claimed for this public-port fingerprint.
