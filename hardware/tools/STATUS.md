# Board generation status — COMPLETE

**DRC: 0 violations, 0 unconnected, 0 footprint errors. All 27 nets routed.**

Canonical regeneration (seed 2 is the routing order that fully routes):
```
cd hardware/tools
"C:\Program Files\KiCad\10.0\bin\python.exe" gen_board.py 2
"C:\Program Files\KiCad\10.0\bin\python.exe" fill_zones.py
kicad-cli pcb drc --severity-error --exit-code-violations -o ../RFID_Door_Controller/drc.rpt ../RFID_Door_Controller/RFID_Door_Controller.kicad_pcb
```

## Deliverables (hardware/RFID_Door_Controller/)
- `RFID_Door_Controller.kicad_pcb` — 110×80 mm 2-layer, GND pours both
  sides + 59 stitching vias, DRC-clean
- `RFID_Door_Controller_gerbers.zip` — upload as-is to JLCPCB/PCBWay
  (fab/ holds the loose gerbers + Excellon drill + map)
- `RFID_Door_Controller.step` — 3D model for the Fusion enclosure
- `board_top.png` — render
- Schematic: ERC 0, netlist round-trip PASS (kept in sync incl. Q1
  TO-92_Inline_Wide)

## Router lessons encoded in gen_board.py (for rev B changes)
- Obstacles carry the pad's REAL net (XIAO THT+SMD twin pads per number)
- Routing endpoints prefer the through-hole twin (pad.HasHole())
- Rectangular obstacle model (hw, hh from pad bbox) — circles either miss
  rect-pad corners or waste corridors
- Inflate by max(track, via)/2 per map — vias are the widest element a net
  places
- Unnumbered mechanical pads (Phoenix pegs) are obstacles
- GRID 0.508 (2.54/5), CLEAR 0.25; signal-order shuffle via argv seed,
  sweep and keep the best (seed 2 for this placement)
- TO-92_Inline (1.27 pitch) is unroutable at these clearances; use _Wide
- ZONE_FILLER: only on a re-loaded board (fill_zones.py), which also prunes
  stitching vias stranded outside the pours
- Silk: hide J*/H* refdes (functional labels instead); keep labels clear of
  connector body outlines (Phoenix bodies reach 4.1 mm past end pads and
  3 mm into the board from the pad row)

## Assembly notes
- Buck module drops into J2/J10 rows on its own headers — MATCH the
  module's IN+/OUT silk to the board's; it fits rotated 180° too, which
  would reverse polarity. Trim to 5.0-5.2 V BEFORE fitting.
- Fit exactly ONE MCU (XIAO socket or DevKit sockets).
- R5-R7 mount flat under the DevKit socket space.
- Strike flyback 1N4007 mounts at the strike, not on this board (D3 on
  board is the relay-coil flyback).
