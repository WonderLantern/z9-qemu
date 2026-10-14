# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile authored journal helper/adapter tests; no firmware or QEMU process."""
from pathlib import Path
import hashlib
import json
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProducerJournalHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [name for name in ('cc', 'pkg-config') if not shutil.which(name)]
        if missing:
            raise RuntimeError('Journal helper tests require Linux build dependencies: ' + ', '.join(missing))

    def compile_and_run(self, source, optimized):
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
        with tempfile.TemporaryDirectory(prefix='journal-helper-') as folder:
            binary = Path(folder) / 'helper'
            command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', *(['-O2', '-DNDEBUG'] if optimized else ['-O0']),
                '-I', str(ROOT/'qemu'), str(ROOT/'tests'/source), *flags, '-o', str(binary)]
            built = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            return result.stdout

    def check_core(self, optimized):
        result = self.compile_and_run('test_producer_journal.c', optimized)
        self.assertIn('journal helper scenarios passed: 12', result)

    def check_adapter(self, optimized):
        data = json.loads(self.compile_and_run('test_producer_journal_adapter.c', optimized))
        raw = bytes((i*13+11)&255 for i in range(33024))
        self.assertEqual(data['records'][0]['spans'][0]['sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(data['records'][1]['source_observation']['actual_read_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertIsNone(data['records'][0]['parent_record_id'])
        self.assertEqual(data['first_readback_failure']['actual_chunk_pa'], 0x9000)
        self.assertEqual(data['first_readback_failure']['actual_chunk_bytes'], 233)
        self.assertEqual(data['first_readback_failure']['memtx_result'], 1)

    def test_core_normal(self): self.check_core(False)
    def test_core_optimized(self): self.check_core(True)
    def test_adapter_normal(self): self.check_adapter(False)
    def test_adapter_optimized(self): self.check_adapter(True)


if __name__ == '__main__': unittest.main()
