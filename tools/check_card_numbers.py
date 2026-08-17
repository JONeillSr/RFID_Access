"""Block card numbers from being committed.

Fob numbers are credentials: anyone holding one can clone a working card. The
gitignore rules already keep `seed.json` and `*-fobs.json` out of the repo, and
they work -- but they only protect known FILES. A number pasted into source as a
placeholder or a test fixture walks straight past them, into a public repo, in a
file nobody would think to check. That is not hypothetical: a real enrolled card
number was once used as the placeholder in the fob form and reached a commit
here. (Deliberately not quoted -- this file would then be the leak.)

WHAT IT LOOKS FOR

  A. A zero-padded run of 8-12 digits.
     Paxton Clock&Data cards are read digit-for-digit off the card and are
     conventionally zero-padded, e.g. 0007808773. Every legitimate long constant
     in this tree -- 86400000 ms/day, 31536000 max-age, 253402300799999 -- starts
     with a non-zero digit, which makes the leading zero a clean discriminator.

  B. A quoted string whose entire contents are 6-12 digits.
     Wiegand credentials are printed with %llu and are NOT padded, so rule A
     misses them. What still separates them from a duration is that a card number
     is a STRING and a duration is a bare numeral. Requiring the quotes to
     contain nothing but digits keeps "max-age=31536000; ..." and
     "00000000-0000-..." out of it.

Calibrated against the whole tracked tree before shipping: rule A matched
nothing, rule B matched one KiCad format version. A hook that cries wolf gets
bypassed with --no-verify and is then worth nothing, so the false-positive rate
was measured rather than assumed.

WHAT IT DELIBERATELY MISSES
A short unpadded Wiegand number written as a bare numeral (7808773 with no
quotes) is indistinguishable from an ordinary integer and is not detected.

ESCAPE HATCH
Put `card-check: allow` on the line. Use it for genuine non-cards, and prefer an
obvious dummy (0000000000) over a realistic-looking placeholder.

USAGE
  python tools/check_card_numbers.py               # staged changes (pre-commit)
  python tools/check_card_numbers.py --message F   # a commit message (commit-msg)
  python tools/check_card_numbers.py --all         # every tracked file
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

SKIP_EXT = {
    '.png', '.jpg', '.jpeg', '.gif', '.ico', '.svg', '.bin', '.pem', '.pdf',
    '.zip', '.step', '.stl', '.f3d', '.3mf',
    '.kicad_pcb', '.kicad_sch', '.kicad_prl', '.gbr', '.drl',
}

PADDED = re.compile(r'(?<![\d.\-A-Za-z])0\d{7,11}(?![\d.\-A-Za-z])')
QUOTED = re.compile(r'''(["'])(\d{6,12})\1''')
ALLOW = 'card-check: allow'


def is_dummy(s: str) -> bool:
    """Obvious placeholders: all one digit, or a plain digit run."""
    return len(set(s)) == 1 or s in ('0123456789', '1234567890')


def git(*args: str) -> str:
    """Run git and decode as UTF-8 regardless of the console codepage.

    Without the explicit encoding, subprocess decodes with the locale codec --
    cp1252 on Windows -- and any UTF-8 byte in a staged file (an em-dash, an
    emoji, a name with an accent) raises UnicodeDecodeError inside the reader
    thread. stdout then comes back as None and the check dies with a TypeError
    instead of examining the file. A commit containing a card number would be
    blocked by the crash rather than the rule, which only looks like it works.
    """
    r = subprocess.run(['git', *args], capture_output=True,
                       encoding='utf-8', errors='replace')
    return r.stdout or ''


def findings(path: str, text: str) -> list[tuple[int, str, str]]:
    out = []
    lines = text.split('\n')
    for n, line in enumerate(lines, 1):
        # Honour the escape on the line itself or the line above it. Trailing
        # comments do not fit everywhere -- a long line, or a language whose
        # syntax puts the value last -- and an escape that is awkward to place
        # gets replaced by --no-verify, which switches off every other check too.
        prev = lines[n - 2] if n >= 2 else ''
        if ALLOW in line or ALLOW in prev:
            continue
        for m in PADDED.finditer(line):
            if not is_dummy(m.group()):
                out.append((n, m.group(), line.strip()[:90]))
        for m in QUOTED.finditer(line):
            v = m.group(2)
            if not is_dummy(v) and not PADDED.fullmatch(v):
                out.append((n, v, line.strip()[:90]))
    return out


def main() -> int:
    scan_all = '--all' in sys.argv

    # A commit message is published with the repo and is not a file, so the
    # staged-content scan cannot see it.
    if '--message' in sys.argv:
        path = sys.argv[sys.argv.index('--message') + 1]
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            return 0
        # Strip the comment block git appends; it lists staged paths and would
        # otherwise match on filenames the commit is not actually publishing.
        body = '\n'.join(l for l in text.split('\n') if not l.startswith('#'))
        hits = findings(path, body)
        if not hits:
            return 0
        print('', file=sys.stderr)
        print('BLOCKED: the commit message looks like it contains a card number.',
              file=sys.stderr)
        print('A message is published with the repository just as file content is.',
              file=sys.stderr)
        print('', file=sys.stderr)
        for n, v, ctx in hits:
            print(f'  line {n}:  {v}', file=sys.stderr)
            print(f'      {ctx}', file=sys.stderr)
        print('', file=sys.stderr)
        print('Describe the value instead of quoting it.', file=sys.stderr)
        print('', file=sys.stderr)
        return 1

    if scan_all:
        paths = [p for p in git('ls-files').split('\n') if p.strip()]
        read = lambda p: open(p, encoding='utf-8', errors='replace').read()
    else:
        # Staged content, not the working tree: what is about to be committed is
        # what matters, and it may differ from what is on disk.
        paths = [p for p in git('diff', '--cached', '--name-only',
                                '--diff-filter=ACM').split('\n') if p.strip()]
        read = lambda p: git('show', f':{p}')

    hits: list[tuple[str, int, str, str]] = []
    for p in paths:
        if os.path.splitext(p)[1].lower() in SKIP_EXT:
            continue
        if scan_all and not os.path.isfile(p):
            continue
        try:
            text = read(p)
        except (OSError, UnicodeDecodeError):
            continue
        if not text:                     # unreadable, empty, or deleted
            continue
        if '\0' in text[:8000]:          # binary
            continue
        for line_no, value, context in findings(p, text):
            hits.append((p, line_no, value, context))

    if not hits:
        return 0

    print('', file=sys.stderr)
    print('BLOCKED: what looks like a card number is about to be committed.',
          file=sys.stderr)
    print('Fob numbers are credentials -- anyone holding one can clone a working card.',
          file=sys.stderr)
    print('', file=sys.stderr)
    for p, n, v, ctx in hits:
        print(f'  {p}:{n}  {v}', file=sys.stderr)
        print(f'      {ctx}', file=sys.stderr)
    print('', file=sys.stderr)
    print('Fix: use an obvious dummy such as 0000000000, or move the real value into', file=sys.stderr)
    print('a gitignored file (seed.json, *-fobs.json).', file=sys.stderr)
    print(f'If this genuinely is not a card number, add "{ALLOW}" to the line.', file=sys.stderr)
    print('', file=sys.stderr)
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
