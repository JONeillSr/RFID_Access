"""Validate partitions_rfid.csv and diff it against arduino-esp32's min_spiffs.csv.

Answers the question that decides whether a partition change costs you every
enrolled fob: does the `nvs` partition move?

Checks performed:
  * every partition is 4 KB aligned, app partitions 64 KB aligned
  * no overlaps, no region past the end of flash
  * the table fits 4 MB exactly (unallocated tail is reported)
  * nvs / otadata placement is byte-identical to the old table
  * the current firmware image still fits the new app slot

Usage:  python tools/check_partitions.py
"""
import csv
import os
import re
import sys

FLASH = 4 * 1024 * 1024
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

NEW = os.path.join(PROJ, "partitions_rfid.csv")
OLD = os.path.expanduser(
    "~/.platformio/packages/framework-arduinoespressif32/tools/partitions/min_spiffs.csv"
)
IMAGE = os.path.join(PROJ, ".pio", "build", "esp32dev", "firmware.bin")


def num(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def load(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            line = line.split("#")[0].strip()
            if not line:
                continue
            f = [c.strip() for c in line.split(",")]
            if len(f) < 5 or not f[3]:
                continue
            rows.append(
                {"name": f[0], "type": f[1], "sub": f[2],
                 "off": num(f[3]), "size": num(f[4])}
            )
    return rows


def show(rows, title):
    print("\n%s" % title)
    print("  %-9s %-5s %-9s %10s %10s %10s  %s"
          % ("name", "type", "subtype", "offset", "size", "end", "size (KB)"))
    for r in rows:
        print("  %-9s %-5s %-9s %#10x %#10x %#10x  %9.0f"
              % (r["name"], r["type"], r["sub"], r["off"], r["size"],
                 r["off"] + r["size"], r["size"] / 1024))


def validate(rows, label):
    ok = True
    ordered = sorted(rows, key=lambda r: r["off"])
    for i, r in enumerate(ordered):
        end = r["off"] + r["size"]
        if r["off"] % 0x1000:
            print("  FAIL %s: %s not 4 KB aligned" % (label, r["name"])); ok = False
        if r["type"] == "app" and r["off"] % 0x10000:
            print("  FAIL %s: app %s not 64 KB aligned" % (label, r["name"])); ok = False
        if end > FLASH:
            print("  FAIL %s: %s ends %#x, past %#x" % (label, r["name"], end, FLASH)); ok = False
        if i + 1 < len(ordered):
            nxt = ordered[i + 1]
            if end > nxt["off"]:
                print("  FAIL %s: %s overlaps %s" % (label, r["name"], nxt["name"])); ok = False
            elif end < nxt["off"]:
                print("  note %s: %#x unallocated between %s and %s"
                      % (label, nxt["off"] - end, r["name"], nxt["name"]))
    tail = FLASH - (ordered[-1]["off"] + ordered[-1]["size"])
    if tail:
        print("  note %s: %#x bytes unallocated at end of flash" % (label, tail))
    print("  %s: %s" % (label, "layout OK" if ok else "LAYOUT INVALID"))
    return ok


old = load(OLD)
new = load(NEW)
show(old, "min_spiffs.csv (current)")
show(new, "partitions_rfid.csv (proposed)")

print("\n=== layout validation ===")
ok = validate(old, "old") & validate(new, "new")

print("\n=== does existing NVS-backed state survive? ===")
byname_old = {r["name"]: r for r in old}
byname_new = {r["name"]: r for r in new}
critical_ok = True
for name in ("nvs", "otadata"):
    o, n = byname_old.get(name), byname_new.get(name)
    if not o or not n:
        print("  %-8s MISSING from one table" % name); critical_ok = False; continue
    same = (o["off"] == n["off"] and o["size"] == n["size"])
    print("  %-8s old=%#x/%#x  new=%#x/%#x  -> %s"
          % (name, o["off"], o["size"], n["off"], n["size"],
             "UNCHANGED (data preserved)" if same else "MOVED - DATA WILL BE LOST"))
    critical_ok &= same

print("\n=== what moved ===")
for name in sorted(set(byname_old) | set(byname_new)):
    o, n = byname_old.get(name), byname_new.get(name)
    if o and n and (o["off"], o["size"]) != (n["off"], n["size"]):
        print("  %-8s offset %#x -> %#x   size %#x -> %#x  (%+d KB)"
              % (name, o["off"], n["off"], o["size"], n["size"],
                 (n["size"] - o["size"]) / 1024))

print("\n=== does the current image still fit? ===")
if os.path.exists(IMAGE):
    sz = os.path.getsize(IMAGE)
    for label, t in (("min_spiffs", byname_old["app0"]), ("partitions_rfid", byname_new["app0"])):
        pct = 100.0 * sz / t["size"]
        print("  %-16s slot %#x (%.2f MB)  image %d B  -> %.1f%% used, %.0f KB free"
              % (label, t["size"], t["size"] / 1048576.0, sz, pct, (t["size"] - sz) / 1024))
    if sz > byname_new["app0"]["size"]:
        print("  FAIL: image does not fit the new slot"); ok = False
else:
    print("  (no built image at %s - run: pio run -e esp32dev)" % IMAGE)

print("\nRESULT:", "PASS" if (ok and critical_ok) else "PROBLEM - see above")
sys.exit(0 if (ok and critical_ok) else 1)
