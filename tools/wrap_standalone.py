"""Wrap the artifact source as a standalone HTML document for the repo.

The artifact host supplies the doctype, <head> and a minimal CSS reset, so the
published source starts at <title> and has no skeleton of its own. Opened
straight from disk that would render, because browsers are forgiving, but it
would be an invalid document relying on error recovery -- and the point of the
repo copy is printing from a TOP-LEVEL page, where the browser honours @page and
the frame cannot interfere.

Generated rather than hand-copied so the two cannot quietly diverge.
"""
import os
import re

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'board-bringup.html')
DST = r"c:\PlatformIO\Projects\RFID_Access\docs\board-bringup.html"

src = open(SRC, encoding='utf-8').read()

head_end = src.index('</style>') + len('</style>')
head, body = src[:head_end], src[head_end:].strip()

title = re.search(r'<title>(.*?)</title>', head).group(1)

doc = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<!--
  Field guide: bringing a new board into the door fleet.

  Open this file directly in a browser and print it. Printing a top-level
  document is the whole reason this copy exists: the shared/published version
  renders inside a frame, where the page box belongs to the host document and an
  orientation or margin request is silently dropped.

  Laid out for ONE portrait Letter/A4 sheet. The sheet is sized in millimetres,
  not left to fill the viewport, so it prints at the same physical size whatever
  the window width -- "Actual size" and "Fit to printable area" both work.

  Regenerate with tools/wrap_standalone.py if the published version changes.
-->
{head[head.index('<title>'):]}
</head>
<body>
{body}
</body>
</html>
"""

os.makedirs(os.path.dirname(DST), exist_ok=True)
open(DST, 'w', encoding='utf-8', newline='\n').write(doc)
print(f'wrote {DST}')
print(f'  title : {title}')
print(f'  size  : {len(doc)/1024:.1f} KB')
