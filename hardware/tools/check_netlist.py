"""Round-trip verification: export the netlist from the generated schematic
via kicad-cli and diff KiCad's view of connectivity against the intended
NETS map in gen_schematic.py. Net names may differ (KiCad picks one label);
we compare the PARTITION of (ref,pin) endpoints, which is what the PCB gets."""
import os, subprocess, sys
from gen_schematic import NETS, NC, PROJ, parse
from inspect_libs import tokenize

CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"
SCH = os.path.join(PROJ, "RFID_Door_Controller.kicad_sch")
NET = os.path.join(PROJ, "netlist.net")

subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                "-o", NET, SCH], check=True, capture_output=True)

tree = parse(tokenize(open(NET, encoding="utf-8").read()))
nets_node = [n for n in tree if isinstance(n, list) and n[0] == "nets"][0]

got = {}   # frozenset((ref,pin)) -> netname
for net in nets_node[1:]:
    name = [c for c in net if isinstance(c, list) and c[0] == "name"][0][1].strip('"')
    nodes = [c for c in net if isinstance(c, list) and c[0] == "node"]
    eps = set()
    for nd in nodes:
        ref = [c for c in nd if isinstance(c, list) and c[0] == "ref"][0][1].strip('"')
        pin = [c for c in nd if isinstance(c, list) and c[0] == "pin"][0][1].strip('"')
        eps.add((ref, pin))
    if len(eps) > 1:
        got[frozenset(eps)] = name

want = {}
for name, pins in NETS.items():
    eps = frozenset((r, p) for r, p in pins if not r.startswith("PWR"))
    if len(eps) > 1:
        want[eps] = name

ok = True
for eps, name in sorted(want.items(), key=lambda kv: kv[1]):
    if eps in got:
        print(f"  OK   {name:12s} == {got[eps]}  ({len(eps)} pins)")
    else:
        ok = False
        print(f"  FAIL {name}: intended {sorted(eps)}")
        for geps, gname in got.items():
            if eps & geps:
                print(f"        overlaps exported net {gname}: {sorted(geps)}")
extra = [ (g, n) for g, n in got.items() if g not in want ]
for g, n in extra:
    ok = False
    print(f"  EXTRA exported net {n}: {sorted(g)}")
print("RESULT:", "PASS - netlist matches design intent" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
