"""Refill zones on the saved board in a clean process (LoadBoard builds the
internal caches that ZONE_FILLER needs; in-memory construction does not)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
from gen_schematic import PROJ

OUT = os.path.join(PROJ, "RFID_Door_Controller.kicad_pcb")
b = pcbnew.LoadBoard(OUT)
b.BuildListOfNets()
b.BuildConnectivity()
pcbnew.ZONE_FILLER(b).Fill(b.Zones())

# prune stitching vias that ended up outside the filled pours (e.g. in the
# dead channel between socket columns) and touch no GND track either
zones = list(b.Zones())
gnd_tracks = [t for t in b.Tracks()
              if t.GetClass() == "PCB_TRACK" and t.GetNetname() == "GND"]
removed = 0
for via in [t for t in b.Tracks() if t.GetClass() == "PCB_VIA"
            and t.GetNetname() == "GND"]:
    pos = via.GetPosition()
    in_pour = any(z.HitTestFilledArea(z.GetFirstLayer(), pos, 0) for z in zones)
    on_track = any(t.HitTest(pos) for t in gnd_tracks)
    if not (in_pour or on_track):
        b.Remove(via)
        removed += 1

b.Save(OUT)
print(f"zones filled: {len(b.Zones())}, stranded vias removed: {removed}")
