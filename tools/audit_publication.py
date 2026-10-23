# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the explicitly reviewed source inventory and every reachable Git blob.

This is a guard against unintended additions, not a license/copyright oracle.
--refresh-manifest is for maintainers after manual content/provenance review.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'publication-manifest.json'
FILES = set(['.gitattributes', '.github/workflows/ci.yml', '.gitignore', 'CONTRIBUTING.md', 'COPYING', 'LICENSES/QEMU-LICENSE.txt', 'NOTICE.md', 'README.md', 'README.zh-CN.md', 'docs/architecture.md', 'docs/build.md', 'docs/capture.md', 'docs/capture.zh-CN.md', 'docs/debugging.md', 'docs/development-snapshot.md', 'docs/device-qualification441.json', 'docs/firmware.md', 'docs/local-workflow-status441.json', 'docs/photo-qualification221.json', 'docs/publication.md', 'docs/recording-qualification221.json', 'docs/research-notes.md', 'docs/service-events.md', 'docs/shared138.md', 'docs/shared144.md', 'docs/shared144.zh-CN.md', 'docs/shared146.md', 'docs/shared146.zh-CN.md', 'docs/shared221.md', 'docs/shared441.md', 'docs/testing.md', 'docs/usage.md', 'qemu/arm-service-events.patch', 'qemu/designware-dma.patch', 'qemu/nikon-dma-observe.h', 'qemu/nikon-expeed-capture-pool.h', 'qemu/nikon-expeed-correction.h', 'qemu/nikon-expeed-dsp-boot.h', 'qemu/nikon-expeed-lossless.h', 'qemu/nikon-expeed-main-output.h', 'qemu/nikon-expeed-motion.h', 'qemu/nikon-expeed-programmed-clock.h', 'qemu/nikon-expeed-register-image.h', 'qemu/nikon-expeed-sensor-fifo.h', 'qemu/nikon-expeed-sensor-pool.h', 'qemu/nikon-expeed-source-handshake.h', 'qemu/nikon-expeed-status-clock.h', 'qemu/nikon-expeed-video-preview.h', 'qemu/nikon-he-forward.inc', 'qemu/nikon-he-public/LICENSE.CDDL', 'qemu/nikon-he-public/LICENSE.LGPL', 'qemu/nikon-he-public/nikon_he_bayer.cpp', 'qemu/nikon-he-public/nikon_he_bayer.h', 'qemu/nikon-he-public/nikon_he_bit_reader.cpp', 'qemu/nikon-he-public/nikon_he_bit_reader.h', 'qemu/nikon-he-public/nikon_he_coefficient_decode.cpp', 'qemu/nikon-he-public/nikon_he_coefficient_decode.h', 'qemu/nikon-he-public/nikon_he_decode.cpp', 'qemu/nikon-he-public/nikon_he_decode.h', 'qemu/nikon-he-public/nikon_he_dequantize.cpp', 'qemu/nikon-he-public/nikon_he_dequantize.h', 'qemu/nikon-he-public/nikon_he_gcli_decode.cpp', 'qemu/nikon-he-public/nikon_he_gcli_decode.h', 'qemu/nikon-he-public/nikon_he_gtli_table.cpp', 'qemu/nikon-he-public/nikon_he_gtli_table.h', 'qemu/nikon-he-public/nikon_he_idwt_horizontal.cpp', 'qemu/nikon-he-public/nikon_he_idwt_horizontal.h', 'qemu/nikon-he-public/nikon_he_idwt_vertical.cpp', 'qemu/nikon-he-public/nikon_he_idwt_vertical.h', 'qemu/nikon-he-public/nikon_he_iqx_iqp_lut_data.h', 'qemu/nikon-he-public/nikon_he_picture_header.cpp', 'qemu/nikon-he-public/nikon_he_picture_header.h', 'qemu/nikon-he-public/nikon_he_precinct_decode.cpp', 'qemu/nikon-he-public/nikon_he_precinct_decode.h', 'qemu/nikon-he-public/nikon_he_precinct_header.cpp', 'qemu/nikon-he-public/nikon_he_precinct_header.h', 'qemu/nikon-he-public/nikon_he_predecessor.cpp', 'qemu/nikon-he-public/nikon_he_predecessor.h', 'qemu/nikon-he-public/nikon_he_predict_lut.cpp', 'qemu/nikon-he-public/nikon_he_predict_lut.h', 'qemu/nikon-he-public/nikon_he_subband_config.cpp', 'qemu/nikon-he-public/nikon_he_subband_config.h', 'qemu/nikon-he-public/nikon_he_tile.cpp', 'qemu/nikon-he-public/nikon_he_tile.h', 'qemu/nikon-he-public/runtime-source-receipt.json', 'qemu/nikon-he-public/source-receipt.json', 'qemu/nikon-he-runtime.cpp', 'qemu/nikon-he-runtime.h', 'qemu/nikon-raw-pixels.h', 'qemu/nikon-temporal-bayer.h', 'qemu/nikon-wave5-codec.h', 'qemu/nikon-z9-capture-nv16.h', 'qemu/nikon-z9-d980.h', 'qemu/nikon-z9-jpeg.h', 'qemu/nikon-z9-lab.c', 'qemu/nikon-z9-pcie.h', 'qemu/nikon-z9-pixels.h', 'qemu/nikon-z9-source-ram.h', 'qemu/nikon-z9-thumbnail-nv16.h', 'qemu/nikon-z9-video-raw.h', 'qemu/nikon-z9-wave5.h', 'qemu/nikon-zr-lab.c', 'qemu/prepare_source.py', 'qemu/public-he-runtime-safety.patch', 'qemu/public-he-width-tail-dpb-evaluation.patch', 'requirements-capture.txt', 'requirements.txt', 'source-snapshot.json', 'tests/run.sh', 'tests/test_capture_contracts.py', 'tests/test_inputs.py', 'tests/test_native_log_guard.py', 'tests/test_pixels.c', 'tools/audit_native_nef.py', 'tools/audit_publication.py', 'tools/audit_repeated_capture.py', 'tools/boot.py', 'tools/build.py', 'tools/build_he_codec.py', 'tools/capture-lossless14-profile.json', 'tools/capture-profile.json', 'tools/capture.py', 'tools/capture_engine151_pixels.py', 'tools/capture_engine152c.py', 'tools/capture_engine153.py', 'tools/capture_engine153_layout.py', 'tools/capture_engine15e2.py', 'tools/capture_frame_conversion.py', 'tools/capture_jpeg_completed.py', 'tools/capture_native_image_arguments.py', 'tools/capture_nv16_pipeline.py', 'tools/capture_processing_input.py', 'tools/capture_readiness.py', 'tools/capture_recovery_flow.py', 'tools/capture_repeat_flow.py', 'tools/capture_sequence.py', 'tools/capture_support.py', 'tools/capture_thumbnail_nv16.py', 'tools/capture_thumbnail_route.py', 'tools/capture_virtual_ram.py', 'tools/capture_workflow_ready.py', 'tools/ccomp_command_peer.py', 'tools/check_receiver_frames.py', 'tools/check_service_events.py', 'tools/check_video_options.py', 'tools/decode_he.cpp', 'tools/deliver_mcu_frame.py', 'tools/export_card.py', 'tools/extract_virtual_exfat.py', 'tools/fast_capture_worker.py', 'tools/guest.py', 'tools/guest_transport.py', 'tools/init_table_input.py', 'tools/inspect_virtual_exfat.py', 'tools/interactive.html', 'tools/isobmff_raw_index.py', 'tools/make_cmos.py', 'tools/mcu_frames.py', 'tools/model-properties.json', 'tools/native_log_guard.py', 'tools/native_menu_screen.py', 'tools/nef_codec_profile.py', 'tools/observe_serial_channel.py', 'tools/observe_z9_state8.py', 'tools/parameter_input.py', 'tools/private_bounded_qmp.py', 'tools/probe.py', 'tools/probe_adc_capture.py', 'tools/probe_graphics_image.py', 'tools/probe_tasks.py', 'tools/receive_mcu_tx.py', 'tools/recording_files.py', 'tools/rtc_mcu_input.py', 'tools/run.py', 'tools/service_serial8.py', 'tools/service_z9_mcu.py', 'tools/smoke_qemu.py', 'tools/validate_run.py', 'tools/virtual_exfat_file.py', 'tools/virtual_exfat_window.py', 'tools/virtual_recording_card.py', 'tools/z9_exposure_input.py', 'tools/z9_global1_boundary_input.py', 'tools/z9_navigation_input.py', 'tools/z9_primary_gate_input.py', 'tools/z9_processing_index_input.py', 'tools/z9_quality_input.py', 'tools/z9_sequence_resource_input.py', 'tools/z9_timing_input.py'])
IGNORED = {'.git','.local','.build','.venv','__pycache__'}
SECRET_PATTERNS = [
    re.compile(r'gh[opusr]_[A-Za-z0-9]{30,}'),
    re.compile(r'github_pat_[A-Za-z0-9_]{30,}'),
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
]

def validate(path, data):
    if path not in FILES | {MANIFEST}: raise ValueError('Unreviewed public path: '+path)
    if len(data)>(2*1024*1024 if path == 'qemu/nikon-z9-lab.c' else 512*1024) or b'\0' in data: raise ValueError('Unexpected binary/large content: '+path)
    text = data.decode('utf-8')
    if any(pattern.search(text) for pattern in SECRET_PATTERNS): raise ValueError('Credential pattern in '+path)
    if re.search(r"['\"][0-9a-fA-F]{512,}['\"]",text): raise ValueError('Large embedded hex data needs review: '+path)
    source=path.endswith(('.py','.c','.h','.cpp','.inc','.sh'))
    public_lgpl=(path.startswith('qemu/nikon-he-public/') and path.endswith(('.cpp','.h')) and
        'GNU LESSER GENERAL PUBLIC LICENSE version 2.1' in text[:2048] and 'LICENSE.CDDL' in text[:2048])
    if source and not public_lgpl and 'SPDX-License-Identifier: GPL-2.0-or-later' not in text[:200]:
        raise ValueError('Missing retained source license: '+path)
    return hashlib.sha256(data).hexdigest()

def git(*args):
    return subprocess.check_output(['git',*args],cwd=ROOT)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--refresh-manifest',action='store_true')
    a = p.parse_args()
    discovered = set()
    if (ROOT/'.git').exists():
        # Exclude explicitly ignored local firmware/archives, but never ignore a
        # tracked path. Every staged/history blob is validated again below.
        candidates=[ROOT/name.decode('utf-8') for name in git('ls-files','--cached','--others','--exclude-standard','-z').split(b'\0') if name]
    else:
        candidates=[]
        for directory,dirs,files in os.walk(ROOT):
            dirs[:] = [name for name in dirs if name not in IGNORED]
            candidates.extend(Path(directory)/name for name in files)
    for path in candidates:
        if path.is_symlink() or not path.is_file(): raise ValueError('Nonregular public path: '+str(path))
        discovered.add(path.relative_to(ROOT).as_posix())
    if discovered - {MANIFEST} != FILES:
        raise ValueError('Inventory differs: '+str({'extra':sorted(discovered-FILES-{MANIFEST}),'missing':sorted(FILES-discovered)}))
    hashes = {name:validate(name,(ROOT/name).read_bytes()) for name in sorted(FILES)}
    if a.refresh_manifest:
        (ROOT/MANIFEST).write_text(json.dumps({'schema':1,'scope':'reviewed source-only publication inventory; no runtime artifacts','files':hashes},indent=2)+'\n',encoding='utf-8',newline='\n')
    manifest = json.loads((ROOT/MANIFEST).read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('files') != hashes: raise ValueError('Content changed since reviewed manifest')
    validate(MANIFEST,(ROOT/MANIFEST).read_bytes())
    commits = 0; blobs = set()
    if (ROOT/'.git').exists():
        # Inspect all refs, rather than only the currently visible checkout.
        refs = git('rev-list','--all').decode().splitlines()
        commits = len(refs)
        for ref in refs:
            tree = git('ls-tree','-rz','--full-tree',ref).split(b'\0')
            for row in tree:
                if not row: continue
                meta,name = row.split(b'\t',1)
                mode,kind,oid = meta.decode().split()
                name = name.decode('utf-8')
                if kind != 'blob' or mode not in ('100644','100755'): raise ValueError('Unexpected history object: '+name)
                if (oid,name) not in blobs:
                    validate(name,git('cat-file','blob',oid)); blobs.add((oid,name))
        tracked = set()
        for row in git('ls-files','--stage','-z').split(b'\0'):
            if not row: continue
            meta,name = row.split(b'\t',1)
            mode,oid,stage = meta.decode().split()
            name = name.decode('utf-8')
            if stage != '0' or mode not in ('100644','100755'):
                raise ValueError('Unexpected index entry: '+name)
            staged_hash = validate(name,git('cat-file','blob',oid))
            if staged_hash != hashlib.sha256((ROOT/name).read_bytes()).hexdigest():
                raise ValueError('Staged content differs from reviewed worktree: '+name)
            tracked.add(name)
        if tracked != FILES | {MANIFEST}: raise ValueError('Git index differs from reviewed inventory')
    print(json.dumps({'passed':True,'reviewed_files':len(hashes)+1,'history_commits':commits,'history_blobs':len(blobs),'runtime_data_excluded':True}))

if __name__ == '__main__': main()
