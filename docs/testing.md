# Current shared441 scope

See [shared441](shared441.md) and `source-snapshot.json` for the current development build. Camera qualifications below retain their original revision and executable hashes. They are not current-build regression results.

Run the current no-firmware frame STOP/drain/mask/ACK/resume contracts on both
boards, in paired/split mode and with the new option both off/on:

```sh
python tools/check_receiver_frames.py --qemu /absolute/path/to/qemu-system-aarch64 \
  --output .local/receiver-frames441.json
```

This does not execute proprietary firmware or qualify native capture. The C
pixel test also checks a complete 12-bit grey ramp against independently computed
ten-bit transfer values, ensuring the new path preserves more than eight bits.

## Historical records

# Validation status

## Full RAW capture update (2026-10-11)

The current shared Z9 board passed a full shutter/capture/recovery workflow
in run563: all 57 original input states consumed, one initialized native NEF,
fresh post-shot RAW/LCD, two actual PHOTO-menu returns and no observed native
fault. This is a bounded single-shot test; movie recording and RAW pre-capture
remain unaccepted. The standalone public capture command is checked separately
below; shared research evidence is not silently counted as package acceptance.

| Check | Result in run563 |
| --- | --- |
| Card file | `DCIM/100NCZ_9/DSC_0001.NEF`, 81,222,656 bytes |
| RAW | 8280×5520, 14-bit; all 45,705,600 independently decoded samples equal guest producer RAM |
| Embedded JPEG previews | 8256×5504, 1620×1080, 160×120; each byte-identical to its completed codec stream |
| Post-shot recovery | New RAW and visible LCD frames after image work; two actual menus followed by fresh live view |
| Original control flow | Request-bound CCOMP reply, GPIO request/feedback/ack and original wait returns observed |
| Unsupported acceptance | Movie recording, playable video, RAW pre-capture, repeated burst and physical sensor/codec behavior |

Result SHA-256:
`de87b5f06e4e89913ee4594e7c4368aa62d59d09222f6dc3664dad76823b4c64`.
Native NEF SHA-256:
`87fbeba50825a34f513d9215ebe5c9c64b92d8ee246a024504c55e89a63f3a34`.
Shared tested binary SHA-256:
`682926d5502d5c24eaa9a52be07e8f388201c17bf076f05e84efdf1495c948b6`.
The native run took about 309 host seconds plus about 5 seconds for file
auditing in that environment. This is evidence provenance, not a performance
guarantee or measured physical camera rate. Artifact sizes/hashes depend on
the user's input and metadata. Original firmware bytes were unchanged.

The accepted host source was **8256×5504 RGGB/u16 at 14-bit range**, resampled
by the functional sensor model into the firmware's 8280×5520 RAW request.
This does not recover physical border pixels or sensor packing. The output
RAW/file oracle compares the actual producer, not an assumption that the
input and capture geometries are identical. See [capture.md](capture.md).

The public build passes 13 Python tests (including request-bound CCOMP ACK,
14-bit range, fresh-frame/all-preview gating and existing-card preservation),
synthetic C pixel tests, default-off realization and 13 WFE/SEV/IRQ execution
checks. CI uses only authored/synthetic inputs and cannot establish native
camera acceptance without the user's firmware.

## Standalone public package acceptance (run564)

The repository's own `tools/capture.py` command, rebuilt from this source
overlay without the private shared-build launcher or lifecycle plugin,
passed the same 57-contact complete workflow. It resumed fresh visible
RAW-derived live view after the photo, recognized two actual PHOTO menus,
returned to fresh frames, stopped normally and extracted the actual NEF.
The standalone auditor then independently decoded all 45,705,600 samples
and matched all three embedded JPEG streams. Final counters were RAW765
and LCD-video851; counters alone were not the acceptance criterion.

| Artifact | SHA-256 |
| --- | --- |
| Public tested executable | `18ca5bb07cb514f9c57f78d31f43b9beb1e3566e65d354245961facdf52f9006` |
| Native result | `f3a3e2d13980c620730f269fd1a1771cf7c70c1b5884a3f25e61da0dc08d1b9b` |
| Actual 81,222,656-byte NEF | `c138a2a90b0020891f3b9513c6dfef561641472865e57ff6eb4b4dc21831f702` |

Capture and filesystem extraction ran in the authorized Nikon Linux/WSL
environment. Independent decoding used rawpy0.25.1 / LibRaw0.21.4 in the
existing Windows Python environment. The tools require no private fixture
paths; the user supplies firmware and their own local source. Runtime evidence,
the source photograph, NEF, UI images and memory snapshots are not published.
After adding the JPEG source license marker, the rebuilt binary hash remained
identical to the tested executable; no functional code changed.

The repository's full-size synthetic generator also produced the documented
90,882,080-byte 8256×5504/14-bit source. This generated scene is not the scene
used for run564; input-generation validation and native image acceptance are
recorded separately. Small synthetic sources cover range/CFA tests in CI.

## Historical preview-only acceptance (2026-10-10)

The standalone launcher passed three native MENU returns on 2026-10-10,
with fresh RAW/LCD video and four independent pixel checks. Three preceding
cold boots of the same functional profile also passed, including a run with
detailed tracing removed. These are bounded functional tests, not physical
camera qualification or indefinite stability coverage.

| Run | Configuration | Initial RAW/video | Return 1 | Return 2 | Return 3 |
| --- | --- | --- | --- | --- | --- |
|405|Candidate, sparse submission trace|1403/69|1517/293|1628/511|1734/724|
|406|Candidate, no detailed trace|1406/72|1521/296|1634/518|1746/738|
|407|Candidate, CPU3 wait-site counts|1401/66|1512/284|1624/505|1735/722|
|408|Standalone source launcher, no private wrapper/plugin|1403/68|1512/286|1621/503|1732/721|

Every menu in these runs was reached with one press. Run408 compares610,199
clear-alpha pixels at each live-view stage. Mean absolute RGB error is
0.1508–0.1521/255, maximum0.624; luminance standard deviation exceeds60.
The owned emulator stopped normally, without a recorded error. The test
input was synthetic640×480 RGGB12/u16 color bars, not a Nikon sensor dump.

Run408 result SHA-256:
`498601b76356c82c2fa8ee49a04a1e0fe508176f57a1a491c38eaad23d53dbcf`.
Tested executable SHA-256:
`72e2b48214af7477e7bdee4afd86a320afc71f4b71ac570c7ffa4fc062aa26b9`.
The original firmware fingerprint is in [firmware input](firmware.md).
Full runtime evidence stays private because it includes firmware-derived
memory planes and UI captures. These hashes identify local evidence; the
source repository does not distribute those artifacts.

The public build also passes13 firmware-free WFE/SEV/IRQ execution checks,
9 Python tests, pixel tests, default-off board realization and rejection of
unsupported CPU/clock settings. These test the
[documented service event contract](service-events.md), not complete ARM
event architecture. Patch application and repeated preparation were checked.

The private Z9 C5.32 baseline has independently demonstrated 915 RAW-derived
frames, 5,174,830,080 pixel bytes, zero rejected destinations and 1,144 video
scanouts. For 610,199 clear-alpha LCD pixels, an independent NV12 decode had
mean RGB absolute error 0.243/255 and maximum error 0.524. This establishes the
functional pixel path for that recorded run, not physical sensor accuracy.

Source drain tests execute original driver and ISR routines with explicitly
prepared diagnostic state. Masked pending, unmask, unrelated W1C, reset and
restart are checked. The RAW DMA regression checks destination guards,
backpressure and complete-bank replacement. These tests are local because
they require the user's proprietary firmware and are not public CI inputs.

Public CI will run only synthetic source/RAW/protocol tests, publication audit
and a verified upstream build. It cannot boot the camera UI without firmware.
Do not interpret a green CI badge as camera acceptance.

Remaining physical limits include real CMOS programming, sensor timing,
calibration, AE/AF/PDAF, recording, physical card behavior and complete GPU
behavior. Functional virtual-card writing is now supported by the separate
capture profile; the historical preview profile still has no card attached.

The current display correction isolates directly enabled NV12 video from the
two optional layer controls. Ten fixed-pixel checks cover layer combinations,
direct stop/restart, source preservation and strict MMIO. Fourteen existing
display/alpha/IRQ/bounds checks also pass on the corrected standalone build.
QOM `last_frame_has_video` describes the last composed frame; the video frame
counter remains historical. MENU acceptance requires both new RAW and new LCD
video after returning, followed by independent pixel validation.

Earlier native menu-return diagnostics exposed premature frame release during
submission. MVD interrupt delivery and acknowledgement lifetime were
corrected in separate original-ISR checks, but this does not establish restart
acceptance. A further unsupported two-byte plane clear was captured and its
functional support now passes 184 local clear checks, including full-buffer
comparison and invalid-request boundaries. Diagnostic MVD scheduling changes
are excluded from this launcher. The current combined service-event,
instruction-clock, virtual-input and synthetic-timing profile passes the
three-return acceptance above. The historical failing observations remain
valid evidence for the earlier profiles; no single component is claimed as
an independently proved cure for every lifetime failure.

Current development scope and corrected algorithm/movie claims: [2026-10-13 shared136 snapshot](development-snapshot.md). Earlier test results retain their original revision scope.
