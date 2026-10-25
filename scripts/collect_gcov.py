"""Preserve original gcov text to avoid this Windows gcov's JSON int32 bug."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--gcov', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
objects = args.build / 'CMakeFiles' / 'chengvault_core.dir' / 'src'
notes = sorted(list((objects / 'core').glob('*.gcno')) +
               list((objects / 'platform').glob('*.gcno')))
if not notes:
    raise SystemExit('No instrumented core/platform object files found')
source_records = []
for note in notes:
    source = args.build.parent / 'src' / note.parent.name / note.name.removesuffix('.gcno')
    if source.stat().st_mtime_ns > note.stat().st_mtime_ns:
        raise SystemExit(f'Source is newer than instrumentation; rebuild before reporting: {source}')
    source_records.append({'file': source.relative_to(args.build.parent).as_posix(),
                           'sha256': hashlib.sha256(source.read_bytes()).hexdigest()})
    # Each translation unit has a separate directory. This preserves included
    # header counters without collisions; gcovr applies the production scope.
    destination = args.output / note.name.removesuffix('.gcno')
    destination.mkdir(exist_ok=True)
    for previous in destination.glob('*.gcov'):
        previous.unlink()
    command = [str(args.gcov.resolve()), '--branch-counts', '--branch-probabilities',
               str(note.resolve())]
    process = subprocess.run(command, cwd=destination, capture_output=True, text=True,
                             encoding='utf-8', errors='replace')
    (destination / 'gcov.log').write_text(process.stdout + process.stderr, encoding='utf-8')
    if process.returncode:
        raise SystemExit(f'gcov failed for {note}: {process.stderr}')
    print(f'{note.name}: extracted unmodified gcov text')
(args.output.parent / 'source-digests.json').write_text(
    json.dumps(source_records, indent=2), encoding='utf-8')
