# Bounded producer generation journal

The default-off `producer-generation-journal=on` machine property records actual
D980 RAM output, instance0 151 NV16/RAW output, and RAW transport output on the
1540 port or the explicitly enabled alternate 153c port. Read its JSON with QMP
`qom-get`, path `/machine`, property `producer-generation-journal-state`.
It is immutable after realization, including off-to-on changes.

This is metadata telemetry, not a historical frame cache or RAW pre-capture.
It introduces no pixel source, instruction patch, readiness result or successful
callback. Existing admission, latest-source restrictions, DMA, IRQ/W1C, errors
and stop behavior remain. The inherited ZR machine uses the same optional hooks;
native ZR capture is not qualified by these tests.

## Records and limits

The fixed 64-record journal stores complete destination spans, geometry,
packing, full SHA256 read back from actual RAM, virtual start/completion times,
model operation/write/reset epochs and parent lineage. NV16 Y and UV plus the
optional RAW side output publish atomically only after the original full 151
operation succeeds. Parent matching uses kind, geometry, address, model frame/job
and the complete source hash read before destination invalidation.

The Z9 static input has no frame marker. `sensor_tagged` is false; equal pixels
can still belong to distinct model writes. Model counters and lineage are not
native allocator identities. An external run manifest must bind every receipt.
Missing parents retain the actual source span/hash and explicitly flag a gap.
Capacity exhaustion never silently evicts an older record.

Packed12/14 and NV16 spans match row stride times height. F0 lossless14 output is
explicitly named `nikon-f0-lossless14`: its span/hash covers the actual encoded
length, not the reserved packed capacity. Width/height/stride describe its Bayer
input layout. Both RAW ports share a monotonically increasing telemetry job ID;
the actual source and destination addresses distinguish their outputs. The
original codec implementation and per-port functional counters are unchanged.

Monitored destination writes invalidate overlapping records before the first
write, even on partial failure and same-content ABA reuse. Primary and instance0
151 reset barriers close their own producer availability without recursively
erasing downstream copies. Stop/cancel/W1C are not reset barriers. General machine
reset and 1540/153c reset lifetimes are outside this journal's coverage.

CPU writes, other DMA engines (including video), native ownership and unobserved
allocation/free events remain untracked. The telemetry explicitly reports these
gaps. `no_monitored_overlap_since_completion` does not prove a complete RAM
lifetime. Error returns from telemetry do not replace producer results. SHA uses
one reusable GLib checksum and a bounded 16KiB scratch allocated only when enabled.
The first readback failure preserves requested span, failed chunk, virtual time
and MemTxResult when available. Existing unrecoverable QEMU/GLib OOM semantics
remain unchanged.

## Reproduce source-only checks

Build using the ordinary pinned overlay entrypoint, then run:

```sh
sh tests/run.sh
python3 -B tests/test_z9_producer_journal.py \
  --qemu .build/build/qemu-system-aarch64 --output .local/journal-qtest
python3 -O -B tests/test_z9_producer_journal.py \
  --qemu .build/build/qemu-system-aarch64 --output .local/journal-qtest-optimized
```

Output directories must be fresh. Thirteen QTest cases execute authored zero-kernel
transactions without Nikon firmware or CPU execution. They check full A/B
D980-to-151-to-RAW RAM hashes, IRQ/W1C, off/on functional equivalence, actual
same-address/same-content reuse, unchanged old-source rejections, immutable
options, variable F0 output spans/canaries the alternate RAW port and accepted 151 reset barriers. Every case
retains raw QTest transactions/stderr and verifies its owned QEMU stopped.

The helper/adapter tests compile at O0 and O2/NDEBUG. Twelve core scenarios cover
partial writes, readback failure, overflow, capacity, malformed geometry,
parent ambiguity, reset barriers and compressed spans; four receipt contract
tests reject incomplete cases and changed source/binary/equivalence. These are
authored stub evidence, not QEMU fault injection or native camera acceptance.

The local integration checks do not execute the original camera firmware, prove
sensor/ISP fidelity, establish historical ownership or produce a pre-trigger NEF.
Existing `source-snapshot.json` remains the historical shared146 build receipt;
the changed overlay must be built and evaluated separately.
