# shared441 development source update — 2026-10-23

This is a source development snapshot, not a newly qualified camera release.
The current overlay retains the local shared441 device implementation, with
public SPDX license headers. The public inverse keeps its existing dated
modification notices, LGPL-2.1-only selection and pinned provenance receipts.
The portable launchers retain their previously tested recipes; the newer local
burst/pre-release harness is not silently substituted for those recipes.
Build and no-firmware device checks are recorded in `source-snapshot.json`.
Historical shared221 camera qualifications remain tied to that older build.

Both repository build entrypoints completed against the same reused Linux
build tree. Exact executable SHA-256: `d47c30344fe2ed28fdb7389d20a19d2ddcaf8eeaf83a12d1bd6f3a3ff95b4c7e`.
The 22 Z9 and 33 ZR Python tests, existing C pixel tests plus all 4,096
ten-bit grey-ramp values, 138 frame-ownership checks in eight device
contexts, 108 video-option checks and 13 service-event checks passed.
See [public device qualification](device-qualification441.json) and
[historical local workflow receipts](local-workflow-status441.json).
The current portable camera launchers were **not** rerun end to end.

## What changed since shared221

- Direct ten-bit functional Bayer/YUV arithmetic for Main10, replacing the
  older eight-bit result shifted into a ten-bit container. This is a numerical
  component improvement, not a physical sensor-information-depth claim.
- Temporal CFA input, explicit source domains/crops, sensor FIFO/pool ownership,
  source drain/program epochs and full-frame acquisition boundaries.
- Explicit source-counter timing, receiver START/STOP handling, per-job RAW
  ownership and retained output notifications for capture/video integration.
- Default-off DMA observation, register-image/status-clock fixtures and source
  handshake models. These are diagnostic functional models, not measured silicon.
- An additional default-off secondary-input frame owner: STOP prevents new
  automatic BEGIN; an already started frame may drain exactly one END. ACK,
  pending status, explicit external inputs and pixel DMA keep separate ownership.
  It never reads a firmware PC, task pointer or allocator state.

Many new behaviors are opt-in. Merely replacing the overlay does not enable
every experiment or turn an old launcher into a verified burst/video recipe.
See the reproducible no-firmware [frame-owner check](testing.md).

## Latest local whole-workflow results: baseline shared439

These results belong to binary
`2ef855d22c0c054a537656fd0abbd73a926bb0905e01401eb1bd54faf8905e85`
and its frozen local harness, **not** to this rebuilt portable snapshot.

| Workflow | Independently checked result | Scope |
| --- | --- | --- |
| Z9 C5.32 ordinary held burst | Two holds, eight NEFs each; 731,289,600 RAW samples; 48 JPEG previews | 1/1000 exposure; four MENU-close then HALF-held/released recovery gates |
| ZR C1.11 ordinary held burst | Two holds, sixteen NEFs each; 783,953,920 RAW samples; 96 JPEG previews | 1/1000 exposure; four MENU-close/HALF recoveries; Fn3 is also present in the recipe |
| ZR C1.11 HEVC Main10 | Two 3840×2160 ten-bit clips, 909 + 906 frames, both over 30 seconds; all 1,815 frames decoded | Direct ten-bit functional source; two fresh menu returns |
| ZR C1.11 R3D NE | Two clips, 1,803 + 1,809 actual RAW packets; all packets and proxies checked | Public inverse and complete repeated-packet byte comparison; six independent RED first/middle/last image checks |

There are 48 NEFs, 1,515,243,520 independently checked RAW sample positions and
144 JPEG previews across the two burst runs. This does not establish indefinite
burst stability or a pure MENU-only recovery. The ZR burst parent's exit status
5 is retained: later independent file/storage reconciliation passed. The local
Main10 and R3D parent statuses were 5 and 4 respectively; the qualified functional
file results include explicit post-execution reconciliation. A nonzero parent
exit is not rewritten into a clean process exit.

## Current C30/PRE experiment: shared441

The previous experiment produced secondary-input END events while that receiver
was stopped, and observed an original request freed before the waiting caller
returned. The frame-owner change passed 62 local two-model device checks and
80 default-behavior checks, with 31 related host checks.

Z9 run938 used 1,664 frozen host sources, original menu navigation and all 126
original input contacts. It passed its RawRecord=1, PreRelease=3, PostRelease=0
read-only setting gates and reached the final HALF/FULL sequence. It observed
5,138 BEGIN/END pairs and suppressed 1,878 unowned automatic END events. All
99,620 selected lifecycle records were retained; across 152 allocations this
trace contained zero selected late calls/stores and no observed native fatal.
The earlier failing runs had five late calls and two late stores.

**It still failed capture acceptance.** One 8280×5520 functional RAW packet
was present in RAM, but no camera image was completed on the virtual card and
the RAW ring was not latched. No independent complete packet-pixel audit was
performed for this run. Visible live view and the absence of the previous
failure do not qualify pre-release recording or prove a universal heap fix.
Private reference binary:
`67eba488585fc72ee6a903214d09c10bb35f26be37e4ea744c30c94cd0b01bb0`.

## Remaining work and rights

- Actual pre-FULL RAW-ring retention and firmware-written pre-release files.
- Current portable whole-camera requalification; Z9 movie recording and ZR1.10
  whole workflows are not qualified by ZR1.11 results.
- Pure MENU-only returns, long repeated bursts and measured SD/CFexpress identity.
- Original ISP/DSP/JPEG/H.265/RAW-compression algorithms. Current processing is
  authored/public functional code: Bayer conversion, libjpeg, x265/x264 and an
  authored HE forward encoder with a pinned public inverse. Vendor arithmetic
  is not executed merely because its ARM driver or program upload completed.
- Detailed NeuPro programming/arithmetic documentation is missing; local neural
  reproduction is **inconclusive**. The available XM6 material does not establish
  that its revision equals the firmware's DSP or implement a CEVA execution core.
- Physical CMOS/MCU/lens/GPIO protocols, calibration, autofocus and silicon timing.

Firmware, decrypted proprietary code, keys, ROM/calibration/font/icon payloads,
vendor SDKs, camera dumps, test photographs, screenshots and recorded movies are
excluded. Users supply their own compatible firmware and local resources.
Public third-party inverse code retains its LGPL/CDDL notices and provenance.
Independent research and learning; no Nikon/RED affiliation or endorsement.

## 中文状态说明

本次是共享441开发源码同步，不是已完成预拍摄的发布版。双机普通连拍
以及ZR的H.265/R3D整轮结果属于439/2ef本地构建；不转授当前公开重构建。
新帧归属模型默认关闭。Z9最新整轮已越过旧崩溃点、执行完126个原始输入，
99,620条观察零丢弃、未出现此前的释放后调用/写入；但仍没有照片文件，
预拍摄未通过。新Main10算术直接使用10位功能分量，旧221的8位有效源限制
只适用于旧实现。两者均不代表原机ISP、物理传感器有效位深或原厂编码算法。
NeuPro详细文档缺失、本地神经网络复现inconclusive，不能宣称已经复现。
