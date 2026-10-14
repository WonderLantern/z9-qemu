# RAW pre-capture evidence audit

`tools/z9_precapture_audit.py` checks the consistency of a stopped experiment's
native trace, producer snapshots, actual virtual-card bytes and independently
decoded NEF files. It requires the pinned Nikon Z9 C5.32 MAIN identity. It does not start QEMU, synthesize a photograph, patch firmware
or write to a card. A passing unit test or ordinary single-shot decode is **not**
evidence that Z9 RAW pre-capture works.

The minimum accepted experiment contains two distinct retained frames whose DMA
and FIFO enqueue both completed strictly before the earliest possible FULL input
request on every declared enabled route, plus at least one distinct frame whose
DMA completed strictly after all bound original FULL consumers. Retained frames must reach
the original writer in FIFO order before current frames. Every scoped candidate
DMA, submission, writer result and NVMe completion must be accounted for.

## Usage and result

Use Python 3.11 or newer with NumPy. Native decoding additionally requires
`rawpy`/LibRaw, for example `python -m pip install 'rawpy>=0.20,<1'` in an isolated
environment. No decoder fallback is used.

```sh
python tools/z9_precapture_audit.py \
  --manifest .local/my-capture/capture-evidence.json \
  --output .local/my-capture/audit-01.json
python -m unittest discover -s tests -p test_z9_precapture_audit.py -v
```

The output must be new; the tool refuses to replace an existing receipt. Evidence
paths are relative to the manifest directory, must resolve inside it, and must
refer to regular files. Keep firmware, cards, NEFs, producer dumps, traces and
screenshots private. The repository contains none of those required artifacts.

| Result | `accepted` / `native_accepted` | CLI exit |
| --- | --- | --- |
| `passed` | both `true` | 0 |
| `failed` | both `false` | 1 |
| `incomplete` | both `false` | 2 |
| `synthetic_checks_passed` | both `false` | no CLI decoder injection |

The Python entry point is `audit_manifest(manifest, base_dir, decoder=None)`.
It returns a receipt containing `issues`, predicted pre/post frame IDs, output
order and per-file sample counts/mismatches. `accepted` remains a compatibility
alias for `native_accepted`. Passing **any** callable through `decoder=` makes the
result test-only, including passing `decode_rawpy` itself. It can never award
native acceptance. The CLI always uses the fixed `decode_rawpy` implementation.

## Manifest schema

The required schema name is `z9-precapture-audit-v2`. A v1 manifest returns
`incomplete` with `trigger_routes_upgrade_required`; choosing one consumer does
not upgrade old evidence. All IDs must be bounded
strings or nonnegative 32-bit integers; digests are lowercase SHA-256 strings.
Numeric values are integers, not booleans or floating-point substitutes.

The following is a structural example, **not a runnable or accepted experiment**:

```json
{
  "schema": "z9-precapture-audit-v2",
  "run": {
    "run_id": "capture-01",
    "model": "Nikon Z9",
    "firmware_sha256": "<actual loaded MAIN SHA-256>",
    "clock_id": "capture-01:qemu_virtual",
    "clock_kind": "qemu_virtual_ns",
    "stopped": true,
    "error": null,
    "full_consumer_pc": 0,
    "full_action_id": "one-full-action",
    "input_routes": ["gpio", "mcu_frame"],
    "full_route_bindings": {
      "gpio": {"consumer_pc": 0, "full_channel_id": 0},
      "mcu_frame": {"consumer_pc": 0}
    },
    "evidence_kind": "native_guest_capture"
  },
  "queue": {"capacity": 2},
  "frames": [],
  "full_input_evidence": [],
  "events": [],
  "storage": {
    "kind": "guest_nvme_export",
    "card_path": "capture-card.raw",
    "sector_size": 512,
    "catalog_complete": true,
    "nef_files": []
  }
}
```

Replace the digest and consumer address with actual evidence. The original
consumer address must be nonzero and instruction-aligned, and the trigger event
must name exactly that address. `stopped` must be the Boolean `true`, and `error`
must be null. `evidence_kind=synthetic` is admitted only with an injected test
decoder and never establishes native capture.

### All enabled FULL routes and conservative timing

`run.input_routes` must enumerate every enabled route in this bounded action,
without duplicates. This version supports only `gpio` and `mcu_frame`. The keys
of `run.full_route_bindings` must match that set. The GPIO binding names the
original callback PC and the actual registered **FULL** channel ID, not the HALF
ID or an arbitrary worker entry argument. MCU binding names the actual original
key-consumer PC. Bindings and `full_action_id` come from the collector's retained
original records and immutable input/source evidence.

`full_input_evidence` contains exactly one object per enabled route:

- `route_id` and `action_id` bind that route to `run.full_action_id`.
- `request` has unique `event_id`, observed `clock_id` and integer `t_ns`,
  `full_state=1`. MCU uses `kind=input_queued` and `expected_keys_hex` (exactly44
  bytes, lowercase hex). GPIO uses `kind=gpio_input_set` and integer `levels`
  in0..3, with FULL bit1 asserted.
- `consumer` has a distinct unique `event_id`, the same `clock_id`, integer
  `t_ns`, `request_event_id` binding its own request, `origin=original_firmware`,
  `consumer_pc` matching its route binding, and Boolean `matched=true`.
  MCU uses `kind=key_consumer`, Boolean `full_key_state=true`, and `keys_hex` and
  `expected_hex` both identical to the request's full44-byte expectation; bit30
  of its first little-endian word must be set. GPIO uses
  `kind=gpio_input_callback`, integer `state=1`, and integer `channel_id` matching
  the registered FULL channel. A label or `matched=true` alone is insufficient.

Every route's request must be no later than its own consumer. The auditor derives
`pre_cutoff_ns=min(request times)` and `post_cutoff_ns=max(consumer times)`;
callers do not supply these bounds directly. Equal timestamps are allowed for
different actual observations. The canonical `full_press_consumed` in `events`
must identify one **latest** consumer by all of route ID, event ID, PC and time.
If consumers tie, either tied consumer can be canonical, but their identities
remain distinct; a timestamp alone never selects an event.

The request time is a conservative lower bound, not a substitute for an observed
consumer or a claim of unique trigger causality. It must be recorded no later
than the input could affect guest execution. For the current paused input driver,
GPIO is changed before the MCU frame is queued while guest CPUs remain paused;
the collector must retain that ordering and clock/paused provenance. A timestamp
collected after resuming execution cannot be relabeled as this lower bound. If
the old run lacks enough evidence for the bound, leave it incomplete; do not
invent request, channel, state or consumer observations to fill the schema.

Both DMA and enqueue must be strictly before `pre_cutoff_ns`. The retained FIFO
is frozen at that bound; a later enqueue or eviction is `incomplete` with
`late_history_change`. DMA anywhere in the closed interval
`[pre_cutoff_ns, post_cutoff_ns]` is `incomplete` with
`ambiguous_trigger_interval`, regardless of a proposed history/current label.
It cannot be silently removed from the frame list or independent counter scope.
Current DMA must be strictly after `post_cutoff_ns`.

This checks completeness of **declared routes with supplied original-record
bindings**. It does not automatically discover a route omitted from an entirely
fabricated `run.input_routes`, attest the collector, or identify the unique
physical cause. Source logs/launch bindings and independent collector review are
still necessary. A route remaining enabled in `run.input_routes` but missing
evidence is incomplete; unknown, duplicate or disabled-route evidence is
rejected. Additional input events in the scoped capture sequence are unsupported
rather than ignored. Multiple FULL actions require a future explicit contract.

### Frame and producer

Each `frames` item has unique `frame_id`, generation-qualified `job_id`, and a
`producer` object containing:

| Field | Meaning |
| --- | --- |
| `origin` | exactly `guest_producer_dma` |
| `path`, `sha256` | actual producer snapshot and its digest |
| `encoding` | `u16le` or the emulator's explicit `packed14le` layout |
| `width`, `height` | complete RAW geometry |
| `stride_samples` | row pitch in uint16 samples for `u16le` |
| `stride_bytes` | actual packed RAM row pitch for `packed14le` |
| `bit_depth` | sample depth, 1 through 16 |
| `address` | observed original producer buffer address |
| `memory_bytes` | optional complete observed buffer span; defaults to the complete encoded row span times height |
| `marker_scheme`, `marker_frame_id` | optional `cfa32-complement-v1` marker and 32-bit frame ID |

Snapshots must contain all rows, including declared row padding, with no trailing
bytes. Each visible sample must fit the bit depth. The checker hashes actual
samples excluding row padding, and rejects reused static pixels relabeled as
different saved frames. The optional marker occupies a 16×64 sample region with
32 two-column bits and their complements; it supplements full-image comparison,
never replaces it.

The declared address/span must describe the actual retained DMA buffer, not an
invented nonoverlapping address. The checker reserves each buffer at its DMA
completion and rejects overlapping subsequent DMA until capacity eviction or the
matching native writer completion. Leaving the history FIFO at submission does
not release it. This is a conservative acceptance condition: the current schema
cannot accept earlier reuse based on an unrecorded source-read completion, even
when the actual implementation could safely reuse the buffer. The receipt names
this release boundary. It checks recorded completion events, not unobserved DMA
start intervals or arbitrary guest writes; those require separate native lifetime
instrumentation. For `packed14le`, supply the actual complete packed RAM file, its SHA256 and
`stride_bytes`, including row padding. `memory_bytes` must equal that entire
span. The auditor independently extracts contiguous little-endian 14-bit samples;
it does not write a converted artifact. This packing is an emulator convention,
not a recovered physical sensor layout. For normalized `u16le` artifacts derived
elsewhere, preserve the original packing/stride/conversion provenance separately;
never relabel packed bytes as a uint16 dump.

### Event sequence

Every event needs `seq` (contiguous from zero), unique `event_id`, `kind`, `clock_id`
and `t_ns`. Use one observed QEMU virtual nanosecond clock throughout. Host elapsed
time and EXIF timestamps cannot substitute for original FULL consumption. Actual
virtual-clock input requests only establish the conservative lower bound above.
Events must be nondecreasing in time; history DMA/enqueue must precede the lower
bound and current DMA must follow the upper bound. No events may follow `capture_end`.

| `kind` | Additional required data / constraint |
| --- | --- |
| `dma_complete` | `frame_id`, `job_id`; exactly one completion per scoped frame |
| `enqueue` | matching frame/job; strictly before earliest FULL request, after DMA, no queue overflow |
| `evict` | matching oldest frame/job; `reason=capacity`; queue must have been full |
| `full_press_consumed` | exactly once; `origin=original_firmware`, integer `consumer_pc` matching `run.full_consumer_pc`; `route_id`, event ID/PC/time bind an actual latest route consumer |
| `submit` | matching frame/job; `role=history` or `current`; FIFO history first |
| `writer_complete` | matching submitted frame/job, unique `file_id`, zero-based writer-assigned `output_index`, positive `bytes`, `origin=original_firmware` |
| `nvme_write_complete` | actual card-byte `offset`, `length`, integer `status=0`; Boolean false is invalid |
| `capture_end` | independent `dma_count`, `submit_count`, `writer_count`, `nvme_count` for this bounded capture scope |

For a capacity-two trace without eviction, an example order is:
`DMA A → enqueue A → DMA B → enqueue B → earliest FULL request → all route consumers → submit A → submit B → DMA C → submit C`.
The three original writer completions and actual successful card writes must then
be covered before the final counters. DMA completion is only a timing boundary;
this tool does not infer a physical exposure-start time or measured pre-capture
duration from it.

### Actual card and complete NEF catalog

Each `storage.nef_files` item has `file_id`, relative `path`, `bytes`, `sha256`,
`output_index`, and an `extents` list. Each extent contains card-byte `offset` and
`length`. Extents are in **file logical order**, can be physically fragmented, and
include the partition offset because they refer to the full `card_path` file.

The exporter must derive this complete catalog and its extents from the stopped
guest's actual filesystem. The auditor does not infer completeness by scanning
the filesystem itself: `catalog_complete` and the scoped counter observations
remain provenance claims supplied by the collector. It independently checks that:

- every NEF has a matching original writer completion and contiguous logical
  output index, and that counts/order equal the FIFO prediction;
- declared extents cover the entire file exactly, fit the card and do not overlap
  any other NEF data extent;
- their bytes match the exported NEF, and completed sector-aligned NVMe writes
  cover each extent after that frame's submission;
- the file digest and byte count match the writer/export metadata;
- the file has a Nikon TIFF container, is not a first-IFD DNG renamed `.NEF`, and
  rawpy/LibRaw can decode its **whole** RAW mosaic;
- dimensions, uint16 type, white level and every RAW sample match the producer.

## What a pass does not establish

This is a consistency audit, not cryptographic attestation of a camera or of the
collector. A dishonest collector could fabricate mutually consistent metadata;
retain original logs, collector/source/build hashes and independent review of the
instrumented firmware addresses. The declared model and firmware fingerprint
must match the pinned Z9 C5.32 identity; this does not attest which binary actually
ran or bind the collector to a reviewed source checkout.

A pass does not prove physical sensor behavior, real-time throughput, proprietary
NEF compression, final color quality, long bursts or real camera safety. This
schema does not accept JPEG header inspection, a few sampled pixels, three names
for one static Bayer image, injected-decoder output, or an ordinary one-frame NEF
as RAW pre-capture evidence. Menu restoration, filesystem integrity checks and
absence of firmware patches need separate run evidence.

The unit tests use explicitly synthetic TIFF-shaped byte fixtures and an injected
decoder; those fixtures are not usable camera images. The real-decoder smoke check
performed during development used an already stopped upstream Z9 ordinary NEF:
it confirmed the decoding/comparison path only, not Z9 or pre-capture acceptance.

## Z9 integration and acceptance boundary

The model must be `Nikon Z9`, with C5.32 MAIN SHA256
`6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd`.
The CLI additionally requires full 8280x5520 14-bit producer geometry. Small
authored fixtures are available only through the injected test decoder, which
always leaves `accepted` and `native_accepted` false. The implementation is
independent of the optional producer journal and can be reviewed/merged separately.

The schema is a new evidence collection contract; existing ordinary/repeated
`capture.py` receipts are not silently converted into pre-capture evidence.
An experiment must observe and supply all declared original FULL routes,
generation-qualified DMA/queue/writer events, actual card writes, full catalog
and original RAM snapshots. This PR supplies the checker, not an observer that
collects those missing native records or a working Z9 pre-capture feature.

Tests use authored TIFF-shaped bytes and injected sample arrays, not decodable
NEFs or Nikon firmware. They cover identity rejection, packed14 whole-row
extraction including partial groups/extremes/padding, byte-span corruption,
all trigger routes, FIFO, buffer reuse, card extents, complete output accounting,
pixel mismatch and synthetic-versus-native acceptance. Run both normal and
optimized Python; no `assert` statement controls production acceptance.
