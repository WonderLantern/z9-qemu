# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit a finite, time-marked native RAW pre-capture experiment.

Schema z9-precapture-audit-v2 links all capture-candidate DMA events to jobs,
FIFO history, every declared FULL input route, writer-assigned file order, completed
NVMe byte ranges and a complete exported NEF catalog. Producer snapshots are
little-endian uint16 or actual packed14 RAM, with explicit row stride. This is a consistency
audit of supplied native evidence, not cryptographic camera attestation.

The CLI always uses rawpy/LibRaw and never creates an image or edits a card.
An injected decoder is solely a test seam: it can never return accepted=True.
See docs/z9-precapture-audit.md for the schema, provenance and timing limits.
"""
from dataclasses import dataclass
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import numpy as np

SCHEMA = 'z9-precapture-audit-v2'
MODEL = 'Nikon Z9'
MAIN_SHA = '6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd'
MAX_SAMPLES = 64 * 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_EVENTS = 100000


class AuditError(ValueError):
    def __init__(self, code, message, *, incomplete=False):
        super().__init__(message)
        self.code, self.incomplete = code, incomplete


@dataclass
class DecodedRaw:
    samples: np.ndarray
    white_level: int
    decoder: str = 'injected decoder'


def decode_rawpy(path):
    """Independent whole-mosaic decoding; deliberately no alternative decoder."""
    try:
        import rawpy
    except ImportError as error:
        raise AuditError('decoder_unavailable', 'Install rawpy for native NEF auditing', incomplete=True) from error
    try:
        with rawpy.imread(str(path)) as image:
            samples = image.raw_image.copy()
            white = int(image.white_level)
        return DecodedRaw(samples, white, f'rawpy {rawpy.__version__}; LibRaw {rawpy.libraw_version}')
    except Exception as error:
        raise AuditError('decode', f'LibRaw could not decode {path.name}: {error}') from error


def _need(obj, key):
    if not isinstance(obj, dict) or key not in obj:
        raise AuditError('missing_evidence', f'Missing required field: {key}', incomplete=True)
    return obj[key]


def _integer(value, name, minimum=0, maximum=(1 << 64) - 1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise AuditError('schema', f'{name} must be an integer in {minimum}..{maximum}')
    return value


def _identity(value, name):
    if type(value) is int and 0 <= value <= 0xffffffff:
        return str(value)
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.:#-]{0,127}', value):
        raise AuditError('schema', f'Invalid {name}')
    return value


def _digest(value, name):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise AuditError('schema', f'{name} must be a lowercase SHA256')
    return value


def _list(value, name, maximum=MAX_EVENTS):
    if not isinstance(value, list) or len(value) > maximum:
        raise AuditError('schema', f'{name} must be a bounded list')
    return value


def _path(root, value):
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        raise AuditError('path', 'Evidence paths must be relative to the manifest directory')
    target = (root / value).resolve()
    if not target.is_relative_to(root):
        raise AuditError('path', 'Evidence path escapes the manifest directory')
    if not target.is_file():
        raise AuditError('missing_artifact', f'Missing evidence file: {value}', incomplete=True)
    return target


def _sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def _marker(rows, producer, fid):
    scheme = producer.get('marker_scheme')
    if scheme is None:
        return None
    if scheme != 'cfa32-complement-v1' or len(rows) < 16 or len(rows[0]) < 64:
        raise AuditError('marker', 'Unsupported or cropped producer marker')
    expected = producer.get('marker_frame_id', int(fid) if fid.isdecimal() else None)
    expected = _integer(expected, 'marker_frame_id', 0, 0xffffffff)
    white = (1 << producer['bit_depth']) - 1
    low, high = white // 8, white * 7 // 8
    value = 0
    patch = np.stack(rows[:16])[:, :64]
    for bit in range(32):
        original, complement = patch[:8, bit * 2:bit * 2 + 2], patch[8:, bit * 2:bit * 2 + 2]
        if np.all(original == high) and np.all(complement == low):
            value |= 1 << bit
        elif not (np.all(original == low) and np.all(complement == high)):
            raise AuditError('marker', f'Bad bit/complement in frame {fid}, bit {bit}')
    if value != expected or (fid.isdecimal() and value != int(fid)):
        raise AuditError('marker', f'Producer marker ID differs for frame {fid}')
    return value


def _producer_rows(path, width, height, row_bytes, encoding):
    """Read whole actual RAM rows; packed14 is the explicit emulator layout."""
    with path.open('rb') as stream:
        for _ in range(height):
            raw = stream.read(row_bytes)
            if len(raw) != row_bytes:
                raise AuditError('producer_hash', 'Truncated producer row')
            if encoding == 'u16le':
                yield np.frombuffer(raw[:width * 2], dtype='<u2')
            else:
                # Independent per-sample bit extraction, including partial groups.
                data = np.frombuffer(raw + bytes(2), dtype=np.uint8).astype(np.uint32)
                bits = np.arange(width, dtype=np.uint32) * 14
                byte = bits // 8
                words = data[byte] | (data[byte + 1] << 8) | (data[byte + 2] << 16)
                yield ((words >> (bits % 8)) & 16383).astype('<u2')


def _frames(manifest, root):
    result, jobs = {}, set()
    for item in _list(_need(manifest, 'frames'), 'frames', 4096):
        fid, job = _identity(_need(item, 'frame_id'), 'frame_id'), _identity(_need(item, 'job_id'), 'job_id')
        if fid in result or job in jobs:
            raise AuditError('duplicate_identity', 'Frame IDs and generation-qualified job IDs must be unique')
        jobs.add(job)
        producer = _need(item, 'producer')
        encoding = _need(producer, 'encoding')
        if _need(producer, 'origin') != 'guest_producer_dma' or encoding not in ('u16le', 'packed14le'):
            raise AuditError('producer_origin', 'Expected an observed uint16 or packed14 guest producer snapshot')
        width = _integer(_need(producer, 'width'), 'width', 1, 32768)
        height = _integer(_need(producer, 'height'), 'height', 1, 32768)
        bits = _integer(_need(producer, 'bit_depth'), 'bit_depth', 1, 16)
        if encoding == 'u16le':
            stride = _integer(_need(producer, 'stride_samples'), 'stride_samples', width, 65536)
            row_bytes = stride * 2
        else:
            if bits != 14:
                raise AuditError('producer_depth', 'packed14le requires 14-bit samples')
            stride = None
            row_bytes = _integer(_need(producer, 'stride_bytes'), 'stride_bytes', (width * 14 + 7) // 8, 131072)
        if width * height > MAX_SAMPLES or row_bytes * height > MAX_FILE_BYTES:
            raise AuditError('producer_geometry', 'Producer exceeds audit memory/file bounds')
        address = _integer(_need(producer, 'address'), 'producer address', 1)
        span = _integer(producer.get('memory_bytes', row_bytes * height), 'memory_bytes', 1, MAX_FILE_BYTES)
        if encoding == 'packed14le' and span != row_bytes * height:
            raise AuditError('producer_geometry', 'Packed RAM span must cover every actual row and padding')
        if address + span > 1 << 64:
            raise AuditError('producer_geometry', 'Producer memory span overflows')
        path = _path(root, _need(producer, 'path'))
        expected_sha = _digest(_need(producer, 'sha256'), 'producer sha256')
        if path.stat().st_size != row_bytes * height or _sha(path) != expected_sha:
            raise AuditError('producer_hash', f'Changed/truncated producer {fid}')
        pixels_sha, marker_rows = hashlib.sha256(), []
        for y, values in enumerate(_producer_rows(path, width, height, row_bytes, encoding)):
            if int(values.max()) >= 1 << bits:
                raise AuditError('producer_depth', f'Out-of-range producer sample in {fid}')
            pixels_sha.update(values.astype('<u2').tobytes())
            if producer.get('marker_scheme') and y < 16:
                marker_rows.append(values.copy())
        marker = _marker(marker_rows, producer, fid)
        result[fid] = dict(job_id=job, path=path, width=width, height=height, stride=stride,
                           bits=bits, row_bytes=row_bytes, encoding=encoding,
                           address=address, span=span, sha256=expected_sha,
                           pixels_sha256=pixels_sha.hexdigest(), marker_id=marker)
    if not result:
        raise AuditError('missing_frames', 'No actual producer frames', incomplete=True)
    return result


def _full_input_bounds(manifest, report):
    """Conservative bounds, not discovery/attestation of undeclared routes."""
    run = manifest['run']
    clock_id = _identity(_need(run, 'clock_id'), 'run clock ID')
    enabled = _list(_need(run, 'input_routes'), 'input_routes', 8)
    allowed = {'gpio', 'mcu_frame'}
    if (not enabled or any(type(r) is not str or r not in allowed for r in enabled)
            or len(enabled) != len(set(enabled))):
        raise AuditError('trigger_routes', 'Declare each enabled gpio/mcu_frame route exactly once')
    bindings = _need(run, 'full_route_bindings')
    if not isinstance(bindings, dict) or set(bindings) != set(enabled):
        raise AuditError('trigger_routes', 'FULL consumer bindings must match every enabled route')
    action = _identity(_need(run, 'full_action_id'), 'FULL action ID')
    rows = _list(_need(manifest, 'full_input_evidence'), 'FULL route evidence', 8)
    seen, event_ids, observations = set(), set(), []
    for row in rows:
        route = _need(row, 'route_id')
        if type(route) is not str or route not in enabled or route in seen:
            raise AuditError('trigger_routes', 'Unknown, disabled or duplicate FULL route evidence')
        seen.add(route)
        if _identity(_need(row, 'action_id'), 'route action ID') != action:
            raise AuditError('trigger_binding', 'Route evidence belongs to a different FULL action')
        request, consumed = _need(row, 'request'), _need(row, 'consumer')
        ids, times = {}, {}
        for name, event in (('request', request), ('consumer', consumed)):
            eid = _identity(_need(event, 'event_id'), name + ' event ID')
            if eid in event_ids:
                raise AuditError('trigger_binding', 'FULL route event IDs must be unique')
            event_ids.add(eid); ids[name] = eid
            times[name] = _integer(_need(event, 't_ns'), name + ' t_ns')
            if _identity(_need(event, 'clock_id'), 'input event clock ID') != clock_id:
                raise AuditError('clock', 'FULL requests/consumers require the same observed virtual clock')
        if times['request'] > times['consumer']:
            raise AuditError('trigger_binding', 'FULL consumption precedes its bound input request')
        if _identity(_need(consumed, 'request_event_id'), 'bound request ID') != ids['request']:
            raise AuditError('trigger_binding', 'Consumer does not bind its own route request')
        pc = _integer(_need(consumed, 'consumer_pc'), 'route consumer PC', 1)
        bound_pc = _integer(_need(bindings[route], 'consumer_pc'), 'bound FULL consumer PC', 1)
        if pc % 4 or pc != bound_pc:
            raise AuditError('trigger_binding', 'Original route consumer PC differs from its binding')
        if (_need(consumed, 'origin') != 'original_firmware' or _need(consumed, 'matched') is not True
                or _integer(_need(request, 'full_state'), 'requested FULL state', 0, 1) != 1):
            raise AuditError('trigger_state', 'Require matched original FULL assertion, not a label alone')
        if route == 'mcu_frame':
            expected = _need(request, 'expected_keys_hex')
            if (_need(request, 'kind') != 'input_queued' or _need(consumed, 'kind') != 'key_consumer'
                    or not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{88}', expected)
                    or not (int.from_bytes(bytes.fromhex(expected)[:4], 'little') & (1 << 30))
                    or _need(consumed, 'expected_hex') != expected or _need(consumed, 'keys_hex') != expected
                    or _need(consumed, 'full_key_state') is not True):
                raise AuditError('trigger_state', 'MCU FULL requires the complete matching 44-byte key state')
        else:
            levels = _integer(_need(request, 'levels'), 'GPIO levels', 0, 3)
            channel = _integer(_need(consumed, 'channel_id'), 'GPIO consumed channel ID', 0, 0xffffffff)
            full_channel = _integer(_need(bindings[route], 'full_channel_id'), 'GPIO FULL binding', 0, 0xffffffff)
            state = _integer(_need(consumed, 'state'), 'GPIO consumed state', 0, 1)
            if (_need(request, 'kind') != 'gpio_input_set' or _need(consumed, 'kind') != 'gpio_input_callback'
                    or not levels & 2 or channel != full_channel or state != 1):
                raise AuditError('trigger_state', 'GPIO FULL requires the original bound FULL channel and asserted level')
        observations.append(dict(route_id=route, request_event_id=ids['request'], request_ns=times['request'],
                                 consumer_event_id=ids['consumer'], consumer_ns=times['consumer'], consumer_pc=pc))
    if seen != set(enabled):
        raise AuditError('trigger_routes_incomplete', 'An enabled FULL route lacks its request/consumer evidence', incomplete=True)
    before = min(r['request_ns'] for r in observations)
    after = max(r['consumer_ns'] for r in observations)
    report['trigger_bounds'] = dict(pre_cutoff_ns=before, post_cutoff_ns=after, routes=observations,
        action_id=action, clock_id=run['clock_id'], sole_trigger_verified=False,
        scope='Complete declared routes only; strict pre-request FIFO and post-all-consumers bounds, not unique cause')
    return before, after, observations


def _trace(manifest, frames, report):
    run, events = manifest['run'], _list(_need(manifest, 'events'), 'events')
    clock_id = _identity(_need(run, 'clock_id'), 'run clock ID')
    capacity = _integer(_need(_need(manifest, 'queue'), 'capacity'), 'queue capacity', 2, 4096)
    consumer = _integer(_need(run, 'full_consumer_pc'), 'original FULL consumer PC', 1)
    pre_cutoff, post_cutoff, routes = _full_input_bounds(manifest, report)
    route_events = {r[k] for r in routes for k in ('request_event_id', 'consumer_event_id')}
    frozen_pre = None
    history, pre, post, submissions = [], [], [], []
    dma, entered, evicted, writers, nvme = {}, set(), set(), {}, []
    # Submission only transfers queue ownership. Without an independently
    # observed final source read, conservatively retain the producer through
    # native writer completion. Do not infer release from leaving the FIFO.
    owned = set()
    ids, previous_time, trigger, end = set(), -1, None, None
    kinds = {'dma_complete', 'enqueue', 'evict', 'full_press_consumed', 'submit',
             'writer_complete', 'nvme_write_complete', 'capture_end'}
    for index, event in enumerate(events):
        kind = _need(event, 'kind')
        if kind not in kinds:
            raise AuditError('event_kind', f'Unknown event kind {kind}')
        eid = _identity(_need(event, 'event_id'), 'event_id')
        tick = _integer(_need(event, 't_ns'), 'event t_ns')
        if (_integer(_need(event, 'seq'), 'event seq') != index or eid in ids
                or tick < previous_time or end is not None):
            raise AuditError('event_order', 'Events must be complete, unique and monotonic')
        if _identity(_need(event, 'clock_id'), 'capture event clock ID') != clock_id:
            raise AuditError('clock', 'Mixed clock domains cannot establish pre-trigger time')
        ids.add(eid); previous_time = tick
        if frozen_pre is None and tick >= pre_cutoff:
            frozen_pre = list(history)
        if kind != 'full_press_consumed' and eid in route_events:
            raise AuditError('trigger_binding', 'Input event ID reused for a different capture event')
        fid = None
        if kind in {'dma_complete', 'enqueue', 'evict', 'submit', 'writer_complete'}:
            fid = _identity(_need(event, 'frame_id'), 'event frame_id')
            if fid not in frames or _identity(_need(event, 'job_id'), 'event job_id') != frames[fid]['job_id']:
                raise AuditError('job_identity', 'Event does not identify the same observed frame/job')
        if kind == 'dma_complete':
            if pre_cutoff <= tick <= post_cutoff:
                raise AuditError('ambiguous_trigger_interval', 'DMA inside the FULL request/consumer interval cannot be pre or post', incomplete=True)
            if fid in dma:
                raise AuditError('duplicate_dma', 'A frame has multiple producer completions')
            f = frames[fid]
            for retained in owned:
                other = frames[retained]
                if f['address'] < other['address'] + other['span'] and other['address'] < f['address'] + f['span']:
                    raise AuditError('buffer_ownership', 'DMA overwrites a producer before eviction or native writer completion')
            dma[fid] = tick
            owned.add(fid)
            if trigger is not None:
                if tick <= trigger:
                    raise AuditError('post_time', 'Current DMA must complete strictly after FULL consumption')
                post.append(fid)
        elif kind == 'enqueue':
            if tick >= pre_cutoff:
                raise AuditError('late_history_change', 'History DMA and FIFO retention must both precede every FULL request', incomplete=True)
            if trigger is not None or fid not in dma or fid in entered or len(history) >= capacity:
                raise AuditError('queue_enqueue', 'Invalid history enqueue or missing capacity eviction')
            history.append(fid); entered.add(fid)
        elif kind == 'evict':
            if tick >= pre_cutoff:
                raise AuditError('late_history_change', 'Retained FIFO changed at/after the earliest FULL request', incomplete=True)
            if (trigger is not None or event.get('reason') != 'capacity' or len(history) != capacity
                    or not history or history[0] != fid):
                raise AuditError('queue_eviction', 'Eviction must remove the oldest full-queue entry')
            history.pop(0); evicted.add(fid)
            owned.remove(fid)
        elif kind == 'full_press_consumed':
            if (trigger is not None or event.get('origin') != 'original_firmware'
                    or type(event.get('consumer_pc')) is not int
                    or event.get('consumer_pc') != consumer or consumer % 4):
                raise AuditError('trigger', 'One original FULL-consumer observation is required')
            selected = [r for r in routes if r['route_id'] == event.get('route_id')
                        and r['consumer_event_id'] == eid and r['consumer_pc'] == consumer
                        and r['consumer_ns'] == tick == post_cutoff]
            if len(selected) != 1:
                raise AuditError('trigger_binding', 'Canonical FULL must name an actual latest consumer by route and event ID')
            trigger = tick; pre = list(frozen_pre or [])
            if history != pre or any(dma[x] >= pre_cutoff for x in pre):
                raise AuditError('pre_time', 'History must remain the FIFO retained before the earliest FULL request')
            report['trigger'] = dict(event_id=eid, t_ns=tick, consumer_pc=consumer)
        elif kind == 'submit':
            if trigger is None or fid not in dma or fid in submissions:
                raise AuditError('submit_order', 'Missing trigger/DMA or duplicate submission')
            role = _need(event, 'role')
            if (fid in pre and role != 'history') or (fid in post and role != 'current') or fid not in pre + post:
                raise AuditError('submit_role', 'Submission is neither retained history nor a post-trigger frame')
            if fid in pre:
                if not history or history[0] != fid:
                    raise AuditError('submit_order', 'History submission must preserve FIFO order')
                history.pop(0)
            elif history:
                raise AuditError('submit_order', 'Current frame overtook retained history')
            submissions.append(fid); frames[fid]['submit_ns'] = tick
        elif kind == 'writer_complete':
            file_id = _identity(_need(event, 'file_id'), 'file_id')
            if event.get('origin') != 'original_firmware' or fid not in submissions or file_id in writers:
                raise AuditError('writer', 'Unbound or duplicate native NEF writer completion')
            writers[file_id] = dict(frame_id=fid, output_index=_integer(_need(event, 'output_index'), 'output_index'),
                                    bytes=_integer(_need(event, 'bytes'), 'writer bytes', 1, MAX_FILE_BYTES), t_ns=tick)
            owned.discard(fid)
        elif kind == 'nvme_write_complete':
            if trigger is None or type(event.get('status')) is not int or event['status'] != 0:
                raise AuditError('nvme', 'Expected successful post-trigger native NVMe write completion')
            offset = _integer(_need(event, 'offset'), 'NVMe offset')
            length = _integer(_need(event, 'length'), 'NVMe length', 1)
            nvme.append(dict(offset=offset, length=length, t_ns=tick))
        else:
            end = event
    if trigger is None or end is None:
        raise AuditError('unfinished_trace', 'Original trigger and capture-end observations required', incomplete=True)
    for key, actual in (('dma_count', len(dma)), ('submit_count', len(submissions)),
                        ('writer_count', len(writers)), ('nvme_count', len(nvme))):
        if _integer(_need(end, key), key) != actual:
            raise AuditError('counter_coverage', 'Trace count differs from scoped native counters: ' + key)
    if set(dma) != set(frames) or set(dma) - set(post) != entered:
        raise AuditError('frame_coverage', 'Producer/event/queue coverage is incomplete')
    if len(pre) < 2 or not post:
        raise AuditError('pre_post_count', 'At least two retained pre-trigger and one post-trigger frame required')
    expected = pre + post
    if submissions != expected or history:
        raise AuditError('submit_order', 'Selected frame set/order differs from queue prediction')
    report.update(predicted_pre=pre, predicted_post=post, evicted_frames=sorted(evicted), output_frames=expected,
                  buffer_release_boundary='capacity eviction or native writer completion')
    return writers, nvme


def _nikon_tiff(path):
    """Small container identity guard; LibRaw remains the independent decoder."""
    with path.open('rb') as stream:
        header = stream.read(8)
        if len(header) != 8 or header[:2] not in (b'II', b'MM'):
            raise AuditError('nef_container', 'NEF must have a classic TIFF header')
        endian = '<' if header[:2] == b'II' else '>'
        if struct.unpack(endian + 'H', header[2:4])[0] != 42:
            raise AuditError('nef_container', 'Unsupported NEF TIFF version')
        offset = struct.unpack(endian + 'I', header[4:])[0]
        if not 8 <= offset <= path.stat().st_size - 2:
            raise AuditError('nef_container', 'Invalid first IFD')
        stream.seek(offset); count = struct.unpack(endian + 'H', stream.read(2))[0]
        if count > 2048:
            raise AuditError('nef_container', 'Unbounded first IFD')
        records = stream.read(count * 12)
        if len(records) != count * 12:
            raise AuditError('nef_container', 'Truncated first IFD')
        make = None
        for i in range(count):
            record = records[i * 12:i * 12 + 12]
            tag, kind, size, address = struct.unpack(endian + 'HHII', record)
            if tag == 50706:
                raise AuditError('nef_container', 'DNG renamed to NEF is not accepted')
            if tag == 271 and kind == 2 and 1 <= size <= 256:
                if size <= 4:
                    make = record[8:8 + size]
                elif address + size <= path.stat().st_size:
                    stream.seek(address); make = stream.read(size)
        if make is None or not make.rstrip(b'\0').upper().startswith(b'NIKON'):
            raise AuditError('nef_container', 'Expected Nikon NEF manufacturer metadata')


def _covered(start, length, writes, minimum_time):
    position, end = start, start + length
    for row in sorted((r for r in writes if r['t_ns'] >= minimum_time), key=lambda r: r['offset']):
        if row['offset'] > position:
            break
        if row['offset'] + row['length'] > position:
            position = row['offset'] + row['length']
        if position >= end:
            return True
    return False


def _storage(manifest, root, frames, writers, writes, report):
    storage = _need(manifest, 'storage')
    if storage.get('kind') != 'guest_nvme_export' or storage.get('catalog_complete') is not True:
        raise AuditError('storage_origin', 'A complete original NVMe-export catalog is required', incomplete=True)
    catalog = _list(_need(storage, 'nef_files'), 'NEF catalog', 4096)
    if not catalog:
        raise AuditError('no_nef', 'No actual exported NEF files', incomplete=True)
    sector = _integer(_need(storage, 'sector_size'), 'sector_size', 512, 4096)
    if sector & (sector - 1):
        raise AuditError('schema', 'Sector size must be a power of two')
    card = _path(root, _need(storage, 'card_path'))
    card_size = card.stat().st_size
    for row in writes:
        if row['offset'] % sector or row['length'] % sector or row['offset'] + row['length'] > card_size:
            raise AuditError('nvme', 'NVMe write outside the card or sector geometry')
    files, paths, indices, extents_seen = {}, set(), set(), []
    for row in catalog:
        file_id = _identity(_need(row, 'file_id'), 'catalog file_id')
        index = _integer(_need(row, 'output_index'), 'catalog output_index')
        if file_id in files or index in indices:
            raise AuditError('output_order', 'Duplicate catalog identity/order')
        indices.add(index)
        if file_id not in writers:
            raise AuditError('output_coverage', 'Catalog contains a NEF absent from the native writer log')
        writer = writers[file_id]
        if index != writer['output_index']:
            raise AuditError('output_order', 'Catalog order differs from the writer-assigned order')
        path = _path(root, _need(row, 'path'))
        if path in paths or path.suffix.lower() != '.nef':
            raise AuditError('output_coverage', 'Duplicate path or non-NEF catalog entry')
        paths.add(path)
        length = _integer(_need(row, 'bytes'), 'NEF bytes', 1, MAX_FILE_BYTES)
        sha = _digest(_need(row, 'sha256'), 'NEF sha256')
        if length != writer['bytes'] or path.stat().st_size != length or _sha(path) != sha:
            raise AuditError('nef_hash', 'NEF/writer/export identity differs')
        _nikon_tiff(path)
        total = 0
        with card.open('rb') as disk, path.open('rb') as exported:
            for extent in _list(_need(row, 'extents'), 'file extents', 65536):
                offset = _integer(_need(extent, 'offset'), 'extent offset')
                amount = _integer(_need(extent, 'length'), 'extent length', 1, MAX_FILE_BYTES)
                if offset + amount > card_size or total + amount > length:
                    raise AuditError('card_extent', 'File extent exceeds card/file bounds')
                if any(offset < end and start < offset + amount for start, end in extents_seen):
                    raise AuditError('card_extent', 'Overlapping NEF data extents')
                extents_seen.append((offset, offset + amount))
                if not _covered(offset, amount, writes, frames[writer['frame_id']]['submit_ns']):
                    raise AuditError('nvme_coverage', 'NEF extent lacks completed post-submission NVMe writes')
                disk.seek(offset); remaining = amount
                while remaining:
                    n = min(1024 * 1024, remaining)
                    if disk.read(n) != exported.read(n):
                        raise AuditError('card_bytes', 'Exported NEF differs from actual card extents')
                    remaining -= n
                total += amount
        if total != length:
            raise AuditError('card_extent', 'NEF extents do not cover the complete file')
        files[file_id] = dict(path=path, sha256=sha, frame_id=writer['frame_id'], output_index=index)
    if set(files) != set(writers) or len(files) != len(report['output_frames']):
        raise AuditError('output_coverage', 'NEF catalog/writer/predicted frame counts differ')
    ordered = sorted(files.values(), key=lambda x: x['output_index'])
    if sorted(indices) != list(range(len(ordered))) or [r['frame_id'] for r in ordered] != report['output_frames']:
        raise AuditError('output_order', 'NEF logical order differs from retained/current frame order')
    return ordered


def audit_manifest(manifest, base_dir, *, decoder=None):
    """Return a fail-closed receipt; decoder injection is always test-only."""
    report = dict(schema=SCHEMA, status='incomplete', accepted=False, native_accepted=False,
                  issues=[], files=[],
                  physical_camera_verified=False, injected_decoder=decoder is not None,
                  provenance_scope='consistency of supplied native logs, producer snapshots and actual card byte extents')
    try:
        root = Path(base_dir).resolve()
        schema = _need(manifest, 'schema')
        if schema == 'z9-precapture-audit-v1':
            raise AuditError('trigger_routes_upgrade_required', 'v1 lacks complete FULL route bounds; supply original request/consumer evidence for v2', incomplete=True)
        if schema != SCHEMA:
            raise AuditError('schema', 'Unsupported audit manifest version')
        run = _need(manifest, 'run')
        report['run_id'] = _identity(_need(run, 'run_id'), 'run_id')
        report['model'] = _need(run, 'model')
        report['firmware_sha256'] = _digest(_need(run, 'firmware_sha256'), 'firmware sha256')
        if report['model'] != MODEL or report['firmware_sha256'] != MAIN_SHA:
            raise AuditError('firmware_identity', 'This auditor requires Nikon Z9 C5.32 MAIN')
        if _need(run, 'stopped') is not True or _need(run, 'error') is not None:
            raise AuditError('run_incomplete', 'Capture must finish and stop without an error', incomplete=True)
        if _need(run, 'clock_kind') != 'qemu_virtual_ns':
            raise AuditError('clock', 'Use one actual QEMU virtual nanosecond clock')
        _identity(_need(run, 'clock_id'), 'clock_id')
        evidence = _need(run, 'evidence_kind')
        if evidence not in ('native_guest_capture', 'synthetic'):
            raise AuditError('schema', 'Unknown evidence_kind')
        if evidence == 'synthetic' and decoder is None:
            raise AuditError('synthetic_evidence', 'Synthetic data cannot establish native capture', incomplete=True)
        frames = _frames(manifest, root)
        if decoder is None and any((f['width'], f['height'], f['bits']) != (8280, 5520, 14)
                                   for f in frames.values()):
            raise AuditError('producer_geometry', 'Native Z9 audit requires complete 8280x5520 14-bit RAW frames')
        writers, writes = _trace(manifest, frames, report)
        files = _storage(manifest, root, frames, writers, writes, report)
        pixel_ids = [frames[fid]['pixels_sha256'] for fid in report['output_frames']]
        if len(set(pixel_ids)) != len(pixel_ids):
            raise AuditError('pixel_identity', 'Repeated static pixels cannot prove different historical frames')
        decode = decode_rawpy if decoder is None else decoder
        for file in files:
            f = frames[file['frame_id']]
            try:
                decoded = decode(file['path'])
            except AuditError:
                raise
            except Exception as error:
                raise AuditError('decode', f'Decoder failed: {error}') from error
            pixels = np.asarray(decoded.samples)
            if pixels.shape != (f['height'], f['width']) or pixels.dtype.kind != 'u' or pixels.dtype.itemsize != 2:
                raise AuditError('decode_geometry', 'Independent RAW decode differs in full geometry/type')
            if type(decoded.white_level) is not int or decoded.white_level != (1 << f['bits']) - 1:
                raise AuditError('decode_depth', 'Independent RAW white level differs from producer bit depth')
            compared, mismatches, first = 0, 0, None
            for y, expected in enumerate(_producer_rows(f['path'], f['width'], f['height'],
                                                       f['row_bytes'], f['encoding'])):
                bad = np.flatnonzero(pixels[y] != expected)
                compared += f['width']; mismatches += len(bad)
                if first is None and len(bad):
                    x = int(bad[0]); first = dict(y=y, x=x, expected=int(expected[x]), actual=int(pixels[y, x]))
            if _sha(f['path']) != f['sha256']:
                raise AuditError('producer_hash', 'Producer changed during independent decoding/comparison')
            if _sha(file['path']) != file['sha256']:
                raise AuditError('nef_hash', 'NEF changed during independent decoding/comparison')
            item = dict(frame_id=file['frame_id'], job_id=f['job_id'], file=str(file['path']),
                        nef_sha256=file['sha256'], producer_sha256=f['sha256'], pixels_sha256=f['pixels_sha256'],
                        marker_id=f['marker_id'], decoder=decoded.decoder, samples_compared=compared,
                        mismatching_samples=int(mismatches), first_mismatch=first)
            report['files'].append(item)
            if mismatches:
                report['issues'].append(dict(code='pixels', message='Whole RAW samples differ', frame_id=file['frame_id']))
        if report['issues']:
            report['status'] = 'failed'
        elif decoder is not None:
            report['status'] = 'synthetic_checks_passed'
        else:
            report.update(status='passed', accepted=True, native_accepted=True)
    except AuditError as error:
        report['status'] = 'incomplete' if error.incomplete else 'failed'
        report['issues'].append(dict(code=error.code, message=str(error)))
    except (OSError, KeyError, TypeError, ValueError) as error:
        report['status'] = 'incomplete'
        report['issues'].append(dict(code='invalid_or_unavailable_evidence', message=str(error)))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New receipt file; existing files are never overwritten')
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.error('Output already exists; preserve previous evidence')
    try:
        if args.manifest.stat().st_size > 32 * 1024 * 1024:
            raise ValueError('Manifest exceeds the bounded audit input size')
        raw = args.manifest.read_bytes()
        if len(raw) > 32 * 1024 * 1024:
            raise ValueError('Manifest exceeds the bounded audit input size')
        manifest = json.loads(raw)
        result = audit_manifest(manifest, args.manifest.resolve().parent)
        result['manifest_sha256'] = hashlib.sha256(raw).hexdigest()
    except (OSError, ValueError) as error:
        result = dict(schema=SCHEMA, status='incomplete', accepted=False, native_accepted=False,
                      issues=[dict(code='manifest_unavailable', message=str(error))])
    result['auditor_sha256'] = _sha(Path(__file__))
    with args.output.open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['accepted'] else 2 if result['status'] == 'incomplete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
