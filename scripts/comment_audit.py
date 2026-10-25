"""Count physical C++ comment lines without mistaking string // for comments."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()


def count(path):
    source = path.read_text(encoding='utf-8-sig')
    comment_lines, code_lines = set(), set()
    line, offset = 1, 0
    while offset < len(source):
        char = source[offset]
        if char == '\n':
            line += 1; offset += 1; continue
        if source.startswith('//', offset):
            comment_lines.add(line)
            end = source.find('\n', offset)
            offset = len(source) if end < 0 else end
            continue
        if source.startswith('/*', offset):
            end = source.find('*/', offset + 2)
            end = len(source) if end < 0 else end + 2
            segment = source[offset:end]
            comment_lines.update(range(line, line + segment.count('\n') + 1))
            line += segment.count('\n'); offset = end; continue
        if char in ('"', "'"):
            # String/character contents cannot introduce a comment. Raw strings
            # used by this project contain no embedded comment delimiters.
            quote = char; code_lines.add(line); offset += 1
            while offset < len(source):
                if source[offset] == '\\': offset += 2; continue
                if source[offset] == '\n': line += 1
                if source[offset] == quote: offset += 1; break
                offset += 1
            continue
        if not char.isspace(): code_lines.add(line)
        offset += 1
    total = len(source.splitlines())
    nonblank = sum(bool(row.strip()) for row in source.splitlines())
    return dict(file=path.relative_to(args.root).as_posix(), totalLines=total,
                nonblankLines=nonblank, commentLines=len(comment_lines), codeLines=len(code_lines),
                commentPercentPhysical=round(100 * len(comment_lines) / max(1, total), 2),
                commentPercentNonblank=round(100 * len(comment_lines) / max(1, nonblank), 2))


files = sorted(p for folder in ('src', 'include') for p in (args.root / folder).rglob('*') if p.suffix in ('.cpp', '.hpp', '.h'))
records = [count(p) for p in files]
total = sum(r['totalLines'] for r in records)
nonblank = sum(r['nonblankLines'] for r in records)
comments = sum(r['commentLines'] for r in records)
result = dict(scope='Original production C++ in src and include; tests are not production code',
              totalLines=total, nonblankLines=nonblank, commentLines=comments,
              commentPercentPhysical=round(100 * comments / max(1, total), 2),
              commentPercentNonblank=round(100 * comments / max(1, nonblank), 2), files=records)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in result.items() if k != 'files'}, ensure_ascii=False))
