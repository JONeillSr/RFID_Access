"""Generate RFID_Door_Controller.kicad_pcb with KiCad's bundled pcbnew API.

Run with KiCad's python:
  "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" gen_board.py

Placement from PLACE below; nets from gen_schematic.NETS (the map already
verified against the schematic netlist). GND is delivered by front+back
zone pours; other nets are routed by a grid A* router. Validate afterwards
with kicad-cli pcb drc and a render.
"""
import sys, os, math, heapq, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew

SEED = int(sys.argv[1]) if len(sys.argv) > 1 else 0
from gen_schematic import NETS, PROJ   # noqa: E402

OUT   = os.path.join(PROJ, "RFID_Door_Controller.kicad_pcb")
KFP   = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
SEEED = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      "..", "lib", "footprints",
                                      "Seeed_Studio_XIAO_Series.pretty"))

BW, BH = 110.0, 80.0        # board size mm
GRID   = 0.508              # router grid (2.54/5 - stays pad-aligned)
CLEAR  = 0.25               # routing clearance (rule is 0.2; test is exact-geometry)
W_SIG, W_PWR = 0.4, 1.5     # track widths
W_GND  = 1.0                # explicit GND spine (zones add copper in parallel)
VIA_D, VIA_DRILL = 0.8, 0.4
# Only the nets that genuinely carry current get fat traces; the relay coil
# (72 mA) and the 3V3 rails (pull-ups + OLED) are fine at signal width and
# route far more easily.
POWER_NETS = {"12V_RAW", "12V_PROT", "5V", "5V_MCU", "STRIKE_NO", "STRIKE_NC"}

def MM(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))

# ref -> (library_dir, footprint, x, y, rot)
FP = lambda lib: os.path.join(KFP, lib + ".pretty")
PLACE = {
    "U1":  (SEEED, "XIAO-ESP32-C6-DIP",                              20, 16,   0),
    "J8":  (FP("Connector_PinSocket_2.54mm"), "PinSocket_1x15_P2.54mm_Vertical", 48, 6, 0),
    "J9":  (FP("Connector_PinSocket_2.54mm"), "PinSocket_1x15_P2.54mm_Vertical", 73.4, 6, 0),
    "J2":  (FP("Connector_PinSocket_2.54mm"), "PinSocket_1x08_P2.54mm_Vertical", 86, 12, 90),
    "J10": (FP("Connector_PinSocket_2.54mm"), "PinSocket_1x08_P2.54mm_Vertical", 86, 51, 90),
    "J6":  (FP("Connector_PinHeader_2.54mm"), "PinHeader_1x04_P2.54mm_Vertical", 81, 4, 90),
    "J7":  (FP("Connector_PinHeader_2.54mm"), "PinHeader_1x04_P2.54mm_Vertical", 106, 20, 0),
    "J1":  (FP("Connector_Phoenix_MSTB"), "PhoenixContact_MSTBA_2,5_2-G-5,08_1x02_P5.08mm_Horizontal", 10, 72, 0),
    "J4":  (FP("Connector_Phoenix_MSTB"), "PhoenixContact_MSTBA_2,5_3-G-5,08_1x03_P5.08mm_Horizontal", 26, 72, 0),
    "J5":  (FP("Connector_Phoenix_MSTB"), "PhoenixContact_MSTBA_2,5_2-G-5,08_1x02_P5.08mm_Horizontal", 45.5, 72, 0),
    "J3":  (FP("Connector_Phoenix_MSTB"), "PhoenixContact_MSTBA_2,5_8-G-5,08_1x08_P5.08mm_Horizontal", 60, 72, 0),
    "K1":  (FP("Relay_THT"), "Relay_SPDT_SANYOU_SRD_Series_Form_C",  14, 50,   0),
    "Q1":  (FP("Package_TO_SOT_THT"), "TO-92_Inline_Wide",           40, 54,   0),
    "R3":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 35, 44, 0),
    "R4":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 35, 47, 0),
    "D3":  (FP("Diode_THT"), "D_DO-41_SOD81_P10.16mm_Horizontal",     6, 58,  90),
    "D1":  (FP("Diode_THT"), "D_DO-41_SOD81_P10.16mm_Horizontal",     6, 36,  90),
    "C4":  (FP("Capacitor_THT"), "CP_Radial_D6.3mm_P2.50mm",         12, 30,   0),
    "D2":  (FP("Diode_THT"), "D_DO-41_SOD81_P10.16mm_Horizontal",    62, 50,   0),
    "U2":  (FP("Package_TO_SOT_THT"), "TO-220-3_Vertical",           91, 60,   0),
    "C1":  (FP("Capacitor_THT"), "CP_Radial_D5.0mm_P2.50mm",         84, 59,   0),
    "C2":  (FP("Capacitor_THT"), "CP_Radial_D5.0mm_P2.50mm",         78, 59,   0),
    "C3":  (FP("Capacitor_THT"), "C_Disc_D5.0mm_W2.5mm_P2.50mm",     70, 59,   0),
    "R1":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 56, 58, 0),
    "R2":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 56, 62, 0),
    "BZ1": (FP("Buzzer_Beeper"), "Buzzer_12x9.5RM7.6",               32, 34,   0),
    # R5-R7 sit flat beneath the socketed DevKit (2.5mm part, 8.5mm headroom)
    "R5":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 52, 22, 0),
    "R6":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 52, 25, 0),
    "R7":  (FP("Resistor_THT"), "R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal", 52, 28, 0),
    "H1":  (FP("MountingHole"), "MountingHole_3.2mm_M3",              4,  4,   0),
    "H2":  (FP("MountingHole"), "MountingHole_3.2mm_M3",            106,  4,   0),
    "H3":  (FP("MountingHole"), "MountingHole_3.2mm_M3",              4, 64,   0),
    "H4":  (FP("MountingHole"), "MountingHole_3.2mm_M3",            106, 64,   0),
}

VALUES = {"K1": "SRD-05VDC-SL-C", "Q1": "2N3904", "U2": "LD1117V33",
          "D1": "1N4007", "D2": "1N5819", "D3": "1N4007",
          "R1": "10k", "R2": "10k", "R3": "1k", "R4": "10k",
          "R5": "330", "R6": "330", "R7": "330",
          "C1": "10u", "C2": "10u", "C3": "100n", "C4": "100u"}

SILK = [
    ("RFID DOOR CONTROLLER rev A", 24, 61, 0, 1.2),
    ("12V IN",   12.5, 66.5, 0, 1.0), ("+",  10, 68, 0, 1.2), ("-", 15.1, 68, 0, 1.2),
    ("STRIKE",   31, 66.5, 0, 1.0), ("NO", 26, 68, 0, 1.0), ("NC", 31.2, 68, 0, 1.0), ("0V", 36.3, 68, 0, 1.0),
    ("EXIT",     48, 66.5, 0, 1.0), ("BTN", 48, 68, 0, 1.0),
    ("PAXTON READER", 77, 61.5, 0, 1.0),
    ("12V red",  60, 65.5, 90, 0.9), ("0V blk", 65.1, 65.5, 90, 0.9),
    ("DAT yel",  70.2, 65.5, 90, 0.9), ("CLK blu", 75.2, 65.5, 90, 0.9),
    ("RED pur",  80.3, 65.5, 90, 0.9), ("GRN grn", 85.4, 65.5, 90, 0.9),
    ("AMB org",  90.5, 65.5, 90, 0.9), ("MD wht", 95.6, 65.5, 90, 0.9),
    ("BUCK LM2596 HW-411", 92.5, 31.5, 90, 1.0),
    ("match module silk!", 97.5, 31.5, 90, 0.9),
    ("IN+", 82.3, 12, 0, 1.0), ("IN-", 107, 12, 0, 1.0),
    ("OUT+", 82.3, 51, 0, 1.0), ("OUT-", 107, 51, 0, 1.0),
    ("OLED", 85, 1.5, 0, 1.0), ("PANEL LED", 102.5, 24, 90, 1.0),
    ("XIAO ESP32-C6", 20, 2.2, 0, 1.0),
    ("ESP32 DEVKIT V1", 60.7, 46, 0, 1.0),
    ("fit ONE mcu only", 60.7, 43.5, 0, 0.9),
]

# Component identifier + value, printed on silk CENTERED on each footprint as
# an assembly aid: read "R1 10k" right at the part, no schematic needed. The
# part body covers it once installed (fine — that's intentional). Axial parts
# (R, D) have wide pad spacing so the text lands clear of pads; tighter parts
# (C, Q1) may print partly under a pad, harmless for hand soldering. Short,
# human values (not the schematic's raw "100n") since this is a build guide.
SILK_VALUE = {
    "R1": "10k", "R2": "10k", "R3": "1k", "R4": "10k",
    "R5": "330", "R6": "330", "R7": "330",
    "C1": "10uF", "C2": "10uF", "C3": "0.1uF", "C4": "100uF",
    "D1": "1N4007", "D2": "1N5819", "D3": "1N4007",
    "Q1": "2N3904", "U2": "LD1117-3.3", "K1": "relay 5V", "BZ1": "buzzer",
}
# Vertical parts (rot 90 in PLACE) keep horizontal text but nudged off the
# pad column; everything else centers on the footprint origin.
REFDES = []
for _ref, _val in SILK_VALUE.items():
    _x, _y = PLACE[_ref][2], PLACE[_ref][3]
    REFDES.append((f"{_ref} {_val}", _x, _y, 0, 0.8))
SILK = SILK + REFDES

def main():
    b = pcbnew.CreateEmptyBoard()

    # ---- nets ----
    netmap = {}
    for name in sorted(NETS):
        ni = pcbnew.NETINFO_ITEM(b, name)
        b.Add(ni)
        netmap[name] = ni
    pin2net = {}
    for name, pins in NETS.items():
        for ref, pin in pins:
            pin2net[(ref, pin)] = name

    # ---- outline ----
    rect = pcbnew.PCB_SHAPE(b, pcbnew.SHAPE_T_RECT)
    rect.SetStart(MM(0, 0)); rect.SetEnd(MM(BW, BH))
    rect.SetLayer(pcbnew.Edge_Cuts); rect.SetWidth(pcbnew.FromMM(0.1))
    b.Add(rect)

    # ---- footprints ----
    pads = {}       # (ref, pin) -> routing endpoint (x, y, half)
    obstacles = []  # every pad: (x, y, half, netname-or-unique)
    for ref, (lib, name, x, y, rot) in PLACE.items():
        fp = pcbnew.FootprintLoad(lib, name)
        assert fp, f"load fail {lib}:{name}"
        fp.SetReference(ref)
        fp.SetValue(VALUES.get(ref, name))
        b.Add(fp)
        fp.SetPosition(MM(x, y))
        fp.SetOrientationDegrees(rot)
        # hide EVERY auto-placed refdes — they stack on the hand-placed silk.
        # Components that need identifying get a controlled label from REFDES
        # below; connectors/holes are named by their functional labels.
        fp.Reference().SetVisible(False)
        # Value fields sit on F.Fab (documentation, not manufactured) but
        # clutter the 2D editor. For parts NOT in VALUES the value is just the
        # raw footprint-library name (e.g. "PinSocket_1x15_P2.54mm_Vertical")
        # — pure noise; hide it. Discretes keep their real value (1N4007, 10k).
        if ref not in VALUES:
            fp.Value().SetVisible(False)
        mech = 0
        for pad in fp.Pads():
            num = pad.GetNumber()
            key = (ref, num)
            if key in pin2net:
                pad.SetNet(netmap[pin2net[key]])
            p = pad.GetPosition()
            bx = pad.GetBoundingBox()
            half = (bx.GetWidth() / 2 / 1e6, bx.GetHeight() / 2 / 1e6)  # (hw, hh) rect model
            mech += 1
            net = pin2net.get(key) if num else None
            # obstacle carries the pad's REAL net so a pin's duplicate twin
            # (XIAO: THT hole + SMD castellation share a number) never blocks
            # its own net; unconnected/mechanical pads get a unique blocker
            obstacles.append((p.x / 1e6, p.y / 1e6, half,
                              net or f"__{ref}_{num or 'm'}_{mech}"))
            if num:
                # endpoint: prefer the through-hole twin
                if key not in pads or pad.HasHole():
                    pads[key] = (p.x / 1e6, p.y / 1e6, half)

    # ---- GND zones on both copper layers ----
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(b)
        z.SetLayer(layer)
        z.SetNet(netmap["GND"])
        ol = z.Outline(); ol.NewOutline()
        for (px, py) in ((0.5, 0.5), (BW - 0.5, 0.5), (BW - 0.5, BH - 0.5), (0.5, BH - 0.5)):
            ol.Append(pcbnew.FromMM(px), pcbnew.FromMM(py))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(pcbnew.FromMM(0.3))
        z.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.5))
        z.SetMinThickness(pcbnew.FromMM(0.25))
        z.SetLocalClearance(pcbnew.FromMM(0.3))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        b.Add(z)

    # ---- silkscreen ----
    for text, x, y, rot, size in SILK:
        t = pcbnew.PCB_TEXT(b)
        t.SetText(text); t.SetPosition(MM(x, y))
        t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
        t.SetTextThickness(pcbnew.FromMM(max(0.15, size * 0.15)))
        t.SetTextAngleDegrees(rot)
        b.Add(t)

    # ---- router ----
    # Per-layer maps of cell -> set of net names whose copper forbids a trace
    # CENTERLINE there. Two maps per layer: one computed for signal width, one
    # for power width (bigger keep-away). A cell is blocked for net N iff its
    # blocker set contains anything besides N.
    nx, ny = int(BW / GRID), int(BH / GRID)
    blkS = [{}, {}]
    blkP = [{}, {}]

    def cells_around(x, y, hw, hh):
        rx = int(math.ceil((x + hw) / GRID)); lx = int((x - hw) / GRID)
        ty = int((y - hh) / GRID); by = int(math.ceil((y + hh) / GRID))
        out = []
        for ix in range(lx, rx + 1):
            for iy in range(ty, by + 1):
                if 0 <= ix < nx and 0 <= iy < ny and \
                   abs(ix * GRID - x) <= hw and abs(iy * GRID - y) <= hh:
                    out.append((ix, iy))
        return out

    def add_obstacle(x, y, half, net, layers=(0, 1)):
        hw, hh = half if isinstance(half, tuple) else (half, half)
        for m, w in ((blkS, max(W_SIG, VIA_D)), (blkP, max(W_PWR, VIA_D))):
            g = CLEAR + w / 2
            for c in cells_around(x, y, hw + g, hh + g):
                for L in layers:
                    m[L].setdefault(c, set()).add(net)

    for x, y, half, net in obstacles:
        add_obstacle(x, y, half, net)

    for ix in range(nx):
        for iy in range(ny):
            x, y = ix * GRID, iy * GRID
            if x < 1.2 or x > BW - 1.2 or y < 1.2 or y > BH - 1.2:
                for m in (blkS, blkP):
                    m[0].setdefault((ix, iy), set()).add("__edge")
                    m[1].setdefault((ix, iy), set()).add("__edge")

    def astar(starts, goals, net, m, via_cost=25):
        def free(ix, iy, L):
            s = m[L].get((ix, iy))
            return s is None or not (s - {net})
        gset = set(goals)
        pq = [(0, s, None) for s in starts if free(*s[:2], s[2])]
        heapq.heapify(pq)
        seen = {}
        while pq:
            cost, node, parent = heapq.heappop(pq)
            if node in seen:
                continue
            seen[node] = parent
            if node in gset:
                path = [node]
                while seen[path[-1]] is not None:
                    path.append(seen[path[-1]])
                return path[::-1]
            ix, iy, L = node
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                jx, jy = ix + dx, iy + dy
                nxt = (jx, jy, L)
                if 0 <= jx < nx and 0 <= jy < ny and free(jx, jy, L) \
                   and nxt not in seen:
                    heapq.heappush(pq, (cost + 1, nxt, node))
            other = (ix, iy, 1 - L)
            if free(ix, iy, 1 - L) and other not in seen:
                heapq.heappush(pq, (cost + via_cost, other, node))
        return None

    LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)

    def emit(path, net, width):
        # compress into runs, add tracks + vias, register obstacles
        pts = [(p[0] * GRID, p[1] * GRID, p[2]) for p in path]
        i = 0
        while i < len(pts) - 1:
            if pts[i][2] != pts[i + 1][2]:
                via = pcbnew.PCB_VIA(b)
                via.SetPosition(MM(pts[i][0], pts[i][1]))
                via.SetDrill(pcbnew.FromMM(VIA_DRILL))
                via.SetWidth(pcbnew.FromMM(VIA_D))
                via.SetNet(netmap[net])
                b.Add(via)
                add_obstacle(pts[i][0], pts[i][1], VIA_D / 2, net)
                i += 1
                continue
            j = i + 1
            dx = pts[j][0] - pts[i][0]; dy = pts[j][1] - pts[i][1]
            while j + 1 < len(pts) and pts[j + 1][2] == pts[i][2] and \
                  (pts[j + 1][0] - pts[j][0], pts[j + 1][1] - pts[j][1]) == (dx, dy):
                j += 1
            tr = pcbnew.PCB_TRACK(b)
            tr.SetStart(MM(pts[i][0], pts[i][1]))
            tr.SetEnd(MM(pts[j][0], pts[j][1]))
            tr.SetWidth(pcbnew.FromMM(width))
            tr.SetLayer(LAYERS[pts[i][2]])
            tr.SetNet(netmap[net])
            b.Add(tr)
            i = j
        for (px, py, L) in pts:
            add_obstacle(px, py, width / 2, net, layers=(L,))

    def pad_cells(ref, num):
        x, y, half = pads[(ref, num)]
        c = (round(x / GRID), round(y / GRID))
        return [(c[0], c[1], L) for L in (0, 1)]

    def span(net):
        ps = [pads[rp] for rp in NETS[net] if rp in pads]
        if len(ps) < 2:
            return 0
        return (max(p[0] for p in ps) - min(p[0] for p in ps) +
                max(p[1] for p in ps) - min(p[1] for p in ps))
    # GND spine first (zones connect in parallel and kill starved thermals),
    # then power (fat traces need freedom), then signals. Routing success is
    # order-sensitive, so the signal order is shuffled by SEED and the sweep
    # driver keeps the best outcome.
    power = sorted((n for n in NETS if n in POWER_NETS), key=lambda n: -span(n))
    sigs = sorted((n for n in NETS if n != "GND" and n not in POWER_NETS),
                  key=lambda n: -span(n))
    if SEED:
        random.Random(SEED).shuffle(sigs)
    order = ["GND"] + power + sigs
    failed = []
    for net in order:
        pins = [rp for rp in NETS[net] if rp in pads and not rp[1].startswith("__")]
        if len(pins) < 2:
            continue
        if net == "GND":
            width, m = W_GND, blkP
        elif net in POWER_NETS:
            width, m = W_PWR, blkP
        else:
            width, m = W_SIG, blkS
        connected = pad_cells(*pins[0])
        for rp in pins[1:]:
            path = astar(pad_cells(*rp), connected, net, m)
            if path is None:
                # retry with cheap vias: congestion often has a hop-over escape
                path = astar(pad_cells(*rp), connected, net, m, via_cost=10)
            if path is None:
                failed.append((net, rp))
                continue
            emit(path, net, width)
            connected += [(p[0], p[1], p[2]) for p in path] + pad_cells(*rp)

    # ---- GND stitching vias: tie F/B pours together on a coarse grid ----
    # (also rescues pour islands near GND pads from "isolated island" status)
    stitched = 0
    yv = 6.0
    while yv < BH - 4:
        xv = 6.0
        while xv < BW - 4:
            c = (round(xv / GRID), round(yv / GRID))
            s = blkP[0].get(c, set()) | blkP[1].get(c, set())
            if s <= {"GND"}:
                via = pcbnew.PCB_VIA(b)
                via.SetPosition(MM(c[0] * GRID, c[1] * GRID))
                via.SetDrill(pcbnew.FromMM(VIA_DRILL))
                via.SetWidth(pcbnew.FromMM(VIA_D))
                via.SetNet(netmap["GND"])
                b.Add(via)
                add_obstacle(c[0] * GRID, c[1] * GRID, VIA_D / 2, "GND")
                stitched += 1
            xv += 7
        yv += 7
    print("stitching vias:", stitched)

    # ---- save; zones are filled by fill_zones.py in a fresh process ----
    # (ZONE_FILLER access-violates on an in-memory-constructed board even
    # after BuildConnectivity; a reloaded board fills fine.)
    b.Save(OUT)
    print("saved", OUT)
    if os.environ.get("PRINT_BBOX"):
        for ref in ("J1", "J4", "J5", "J3", "K1", "C2", "C3"):
            fp = b.FindFootprintByReference(ref)
            bb = fp.GetBoundingBox(False)
            print(f"BBOX {ref}: x {bb.GetLeft()/1e6:.1f}..{bb.GetRight()/1e6:.1f}"
                  f"  y {bb.GetTop()/1e6:.1f}..{bb.GetBottom()/1e6:.1f}")
    print("FAILCOUNT:", len(failed))
    if failed:
        print("UNROUTED:", failed)

if __name__ == "__main__":
    main()









