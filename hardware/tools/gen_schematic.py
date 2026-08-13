"""Generate RFID_Door_Controller.kicad_sch from the DESIGN.md net map.

Approach: every symbol instance is placed on a coarse grid and every pin gets
a global label at its connection point (or a no-connect marker). Nets form by
label name. After writing, run kicad-cli to ERC and to export the netlist,
which check_netlist.py diffs against NETS below.
"""
import os, re, uuid, subprocess, sys
from inspect_libs import tokenize  # reuse the tokenizer

HERE   = os.path.dirname(os.path.abspath(__file__))
PROJ   = os.path.normpath(os.path.join(HERE, "..", "RFID_Door_Controller"))
OUT    = os.path.join(PROJ, "RFID_Door_Controller.kicad_sch")
KS     = r"C:\Program Files\KiCad\10.0\share\kicad\symbols"
KFP    = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
SEEED  = os.path.normpath(os.path.join(HERE, "..", "lib"))

# ---------------- s-expression parse / serialize ----------------

def parse(toks):
    def rd(i):
        assert toks[i] == "("
        out = []; i += 1
        while toks[i] != ")":
            if toks[i] == "(":
                n, i = rd(i); out.append(n)
            else:
                out.append(toks[i]); i += 1
        return out, i + 1
    n, _ = rd(0)
    return n

def load(path):
    with open(path, encoding="utf-8") as f:
        return parse(tokenize(f.read()))

def ser(n, depth=0):
    if not isinstance(n, list):
        return n
    parts = [ser(c, depth + 1) for c in n]
    line = "(" + " ".join(parts) + ")"
    if len(line) < 100 or depth > 6:
        return line
    head = parts[0]
    body = ("\n" + "  " * (depth + 1)).join(parts[1:])
    return "(" + head + "\n" + "  " * (depth + 1) + body + "\n" + "  " * depth + ")"

def q(s):  # quote a string token
    return '"' + s.replace('"', '\\"') + '"'

def uid():
    return q(str(uuid.uuid4()))

def find(node, tag):
    return [c for c in node if isinstance(c, list) and c and c[0] == tag]

def prop_set(sym, name, value):
    for p in find(sym, "property"):
        if p[1] == q(name):
            p[2] = q(value); return
    sym.append(["property", q(name), q(value), ["at", "0", "0", "0"],
                ["effects", ["font", ["size", "1.27", "1.27"]], "hide"]])

# ---------------- symbol library extraction ----------------

_libcache = {}
def lib_symbol(libfile, name):
    if libfile not in _libcache:
        _libcache[libfile] = load(libfile)
    for s in _libcache[libfile][1:]:
        if isinstance(s, list) and s and s[0] == "symbol" and s[1] == q(name):
            import copy
            return copy.deepcopy(s)
    raise KeyError(f"{name} not in {libfile}")

def rename(sym, old, new):
    def walk(n):
        if isinstance(n, list):
            if n and n[0] == "symbol" and isinstance(n[1], str):
                n[1] = q(n[1].strip('"').replace(old, new, 1))
            for c in n: walk(c)
    walk(sym)

def pins_of(sym):
    """[(number, name, (x, y))] connection points in symbol coords."""
    out = []
    def walk(n):
        if isinstance(n, list):
            if n and n[0] == "pin":
                at = find(n, "at")[0]
                num = find(n, "number")[0][1].strip('"')
                nm  = find(n, "name")[0][1].strip('"')
                out.append((num, nm, (float(at[1]), float(at[2]))))
            for c in n: walk(c)
    walk(sym)
    return out

def strip_pins(sym, keep):
    def walk(n):
        if isinstance(n, list):
            n[:] = [c for c in n
                    if not (isinstance(c, list) and c and c[0] == "pin"
                            and find(c, "number")[0][1].strip('"') not in keep)]
            for c in n: walk(c)
    walk(sym)

def renumber_pins(sym, mapping):  # by pin NAME -> new number
    def walk(n):
        if isinstance(n, list):
            if n and n[0] == "pin":
                nm = find(n, "name")[0][1].strip('"')
                if nm in mapping:
                    find(n, "number")[0][1] = q(mapping[nm])
            for c in n: walk(c)
    walk(sym)

# ---------------- build lib_symbols ----------------

def build_libsymbols():
    out = ["lib_symbols"]; pinmaps = {}

    def add(libfile, base, libid, edit=None):
        s = lib_symbol(libfile, base)
        if edit: edit(s)
        rename(s, base, libid.split(":")[1])
        s[1] = q(libid)
        out.append(s)
        pinmaps[libid] = pins_of(s)

    dev = os.path.join(KS, "Device.kicad_sym")
    con = os.path.join(KS, "Connector_Generic.kicad_sym")
    add(dev, "R", "Device:R")
    add(dev, "C", "Device:C")
    add(dev, "C_Polarized", "Device:C_Polarized")
    add(dev, "D", "Device:D")
    add(dev, "Buzzer", "Device:Buzzer")
    add(dev, "Q_NPN", "Device:Q_NPN",
        edit=lambda s: renumber_pins(s, {"E": "1", "B": "2", "C": "3"}))
    for npins in (2, 3, 4, 8, 15):
        add(con, f"Conn_01x{npins:02d}", f"Connector_Generic:Conn_01x{npins:02d}")
    add(os.path.join(KS, "Relay.kicad_sym"), "SANYOU_SRD_Form_C",
        "Relay:SANYOU_SRD_Form_C")
    # LD1117V33 extends AP1117-15 -> flatten the parent under the child name
    add(os.path.join(KS, "Regulator_Linear.kicad_sym"), "AP1117-15",
        "Regulator_Linear:LD1117V33",
        edit=lambda s: prop_set(s, "Value", "LD1117V33"))
    add(os.path.join(KS, "power.kicad_sym"), "PWR_FLAG", "power:PWR_FLAG")
    add(os.path.join(SEEED, "symbols", "Seeed_Studio_XIAO_Series.kicad_sym"),
        "XIAO-ESP32-C6-SMD", "Seeed_XIAO:XIAO-ESP32-C6-SMD",
        edit=lambda s: strip_pins(s, {str(i) for i in range(1, 15)}))
    q_pins = pinmaps["Device:Q_NPN"]
    assert sorted(p[0] for p in q_pins) == ["1", "2", "3"], q_pins
    return out, pinmaps

# ---------------- design data ----------------

FOOTPRINTS = {
    "U1": "Seeed_XIAO:XIAO-ESP32-C6-DIP",
    "U2": "Package_TO_SOT_THT:TO-220-3_Vertical",
    "K1": "Relay_THT:Relay_SPDT_SANYOU_SRD_Series_Form_C",
    "Q1": "Package_TO_SOT_THT:TO-92_Inline_Wide",
    "D1": "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal",
    "D2": "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal",
    "D3": "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal",
    "BZ1": "Buzzer_Beeper:Buzzer_12x9.5RM7.6",
    "C1": "Capacitor_THT:CP_Radial_D5.0mm_P2.50mm",
    "C2": "Capacitor_THT:CP_Radial_D5.0mm_P2.50mm",
    "C3": "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P2.50mm",
    "C4": "Capacitor_THT:CP_Radial_D6.3mm_P2.50mm",
    "J1": "Connector_Phoenix_MSTB:PhoenixContact_MSTBA_2,5_2-G-5,08_1x02_P5.08mm_Horizontal",
    "J5": "Connector_Phoenix_MSTB:PhoenixContact_MSTBA_2,5_2-G-5,08_1x02_P5.08mm_Horizontal",
    "J4": "Connector_Phoenix_MSTB:PhoenixContact_MSTBA_2,5_3-G-5,08_1x03_P5.08mm_Horizontal",
    "J3": "Connector_Phoenix_MSTB:PhoenixContact_MSTBA_2,5_8-G-5,08_1x08_P5.08mm_Horizontal",
    "J6": "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
    "J7": "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
    "J2": "Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical",
    "J10": "Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical",
    "J8": "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical",
    "J9": "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical",
}
for r in ("R1", "R2", "R3", "R4", "R5", "R6", "R7"):
    FOOTPRINTS[r] = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal"

# ref: (lib_id, value, sheet position)
PARTS = {
    "J1":  ("Connector_Generic:Conn_01x02", "12V IN",        (30, 40)),
    "C4":  ("Device:C_Polarized",           "100u",          (55, 40)),
    "D1":  ("Device:D",                     "1N4007",        (80, 40)),
    "J2":  ("Connector_Generic:Conn_01x08", "BUCK IN ROW",   (110, 50)),
    "J10": ("Connector_Generic:Conn_01x08", "BUCK OUT ROW",  (140, 50)),
    "D2":  ("Device:D",                     "1N5819",        (170, 40)),
    "U2":  ("Regulator_Linear:LD1117V33",   "LD1117V33",     (210, 40)),
    "C1":  ("Device:C_Polarized",           "10u",           (240, 40)),
    "C2":  ("Device:C_Polarized",           "10u",           (260, 40)),
    "C3":  ("Device:C",                     "100n",          (280, 40)),
    "U1":  ("Seeed_XIAO:XIAO-ESP32-C6-SMD", "XIAO-ESP32-C6", (60, 120)),
    "J8":  ("Connector_Generic:Conn_01x15", "DEVKIT L",      (140, 130)),
    "J9":  ("Connector_Generic:Conn_01x15", "DEVKIT R",      (180, 130)),
    "J3":  ("Connector_Generic:Conn_01x08", "READER",        (250, 120)),
    "R1":  ("Device:R",                     "10k",           (290, 110)),
    "R2":  ("Device:R",                     "10k",           (310, 110)),
    "J5":  ("Connector_Generic:Conn_01x02", "EXIT BTN",      (340, 110)),
    "J6":  ("Connector_Generic:Conn_01x04", "OLED",          (370, 110)),
    "R3":  ("Device:R",                     "1k",            (40, 210)),
    "R4":  ("Device:R",                     "10k",           (60, 215)),
    "Q1":  ("Device:Q_NPN",                 "2N3904",        (90, 210)),
    "D3":  ("Device:D",                     "1N4007",        (115, 195)),
    "K1":  ("Relay:SANYOU_SRD_Form_C",      "SRD-05VDC-SL-C", (150, 210)),
    "J4":  ("Connector_Generic:Conn_01x03", "STRIKE",        (195, 210)),
    "BZ1": ("Device:Buzzer",                "Active buzzer", (235, 210)),
    "R5":  ("Device:R",                     "330",           (280, 200)),
    "R6":  ("Device:R",                     "330",           (300, 200)),
    "R7":  ("Device:R",                     "330",           (320, 200)),
    "J7":  ("Connector_Generic:Conn_01x04", "PANEL LED",     (350, 200)),
    "PWR1": ("power:PWR_FLAG",              "PWR_FLAG",      (30, 260)),
    "PWR2": ("power:PWR_FLAG",              "PWR_FLAG",      (50, 260)),
}

NETS = {
    "12V_RAW":  [("J1", "1"), ("C4", "1"), ("D1", "2"), ("J3", "1"), ("K1", "1")],
    "GND":      [("J1", "2"), ("C4", "2"), ("J2", "8"), ("J10", "8"),
                 ("U2", "1"), ("C1", "2"), ("C2", "2"), ("C3", "2"),
                 ("U1", "13"), ("J8", "2"), ("J9", "2"), ("J3", "2"),
                 ("J4", "3"), ("J5", "2"), ("J6", "2"), ("J7", "4"),
                 ("Q1", "1"), ("R4", "2"), ("BZ1", "2"), ("PWR1", "1")],
    "12V_PROT": [("D1", "1"), ("J2", "1")],
    "5V":       [("J10", "1"), ("D2", "2"), ("U2", "3"), ("C1", "1"),
                 ("K1", "2"), ("D3", "1"), ("PWR2", "1")],
    "5V_MCU":   [("D2", "1"), ("U1", "14"), ("J9", "1")],
    "3V3_PU":   [("U2", "2"), ("C2", "1"), ("C3", "1"), ("R1", "1"), ("R2", "1")],
    "3V3_MCU":  [("U1", "12"), ("J8", "1"), ("J6", "1")],
    "DATA":     [("J3", "3"), ("R1", "2"), ("U1", "9"), ("J8", "10")],
    "CLOCK":    [("J3", "4"), ("R2", "2"), ("U1", "10"), ("J8", "9")],
    "RED_LED":  [("J3", "5"), ("U1", "4"), ("J8", "7")],
    "GREEN_LED": [("J3", "6"), ("U1", "11"), ("J8", "15")],
    "AMBER_LED": [("J3", "7"), ("U1", "1"), ("J8", "5")],
    "RELAY_CTL": [("U1", "2"), ("J9", "5"), ("R3", "1"), ("R4", "1")],
    "Q1_B":     [("R3", "2"), ("Q1", "2")],
    "COIL_SW":  [("Q1", "3"), ("K1", "5"), ("D3", "2")],
    "STRIKE_NO": [("K1", "3"), ("J4", "1")],
    "STRIKE_NC": [("K1", "4"), ("J4", "2")],
    "BUZZER":   [("U1", "3"), ("J9", "3"), ("BZ1", "1")],
    "SDA":      [("U1", "5"), ("J8", "11"), ("J6", "3")],
    "SCL":      [("U1", "6"), ("J8", "14"), ("J6", "4")],
    "EXIT_BTN": [("U1", "7"), ("J9", "10"), ("J5", "1")],
    "PANEL_G":  [("U1", "8"), ("J9", "7"), ("R5", "1")],
    "PANEL_R":  [("J9", "8"), ("R6", "1")],
    "PANEL_B":  [("J9", "6"), ("R7", "1")],
    "PANEL_R_J": [("R6", "2"), ("J7", "1")],
    "PANEL_G_J": [("R5", "2"), ("J7", "2")],
    "PANEL_B_J": [("R7", "2"), ("J7", "3")],
}
NC = [("J3", "8"),
      ("J2", "2"), ("J2", "3"), ("J2", "4"), ("J2", "5"), ("J2", "6"), ("J2", "7"),
      ("J10", "2"), ("J10", "3"), ("J10", "4"), ("J10", "5"), ("J10", "6"), ("J10", "7"),
      ("J8", "3"), ("J8", "4"), ("J8", "6"), ("J8", "8"), ("J8", "12"), ("J8", "13"),
      ("J9", "4"), ("J9", "9"), ("J9", "11"), ("J9", "12"), ("J9", "13"),
      ("J9", "14"), ("J9", "15")]

def snap(v):
    return round(round(v / 1.27) * 1.27, 4)

def main():
    # sanity: every footprint file exists
    for ref, fp in FOOTPRINTS.items():
        lib, name = fp.split(":")
        base = SEEED + os.sep + "footprints" if lib == "Seeed_XIAO" else KFP
        p = os.path.join(base, lib + ".pretty", name + ".kicad_mod")
        if lib == "Seeed_XIAO":
            p = os.path.join(SEEED, "footprints", "Seeed_Studio_XIAO_Series.pretty",
                             name + ".kicad_mod")
        assert os.path.exists(p), f"missing footprint {fp} -> {p}"

    libsyms, pinmaps = build_libsymbols()

    # net lookup: (ref, pin) -> net
    pin2net = {}
    for net, pins in NETS.items():
        for rp in pins:
            assert rp not in pin2net, f"{rp} in two nets"
            pin2net[rp] = net

    doc = ["kicad_sch", ["version", "20250114"], ["generator", q("eeschema")],
           ["generator_version", q("10.0")], ["uuid", uid()], ["paper", q("A3")],
           ["title_block", ["title", q("RFID Door Controller")], ["rev", q("A")],
            ["comment", "1", q("Universal carrier: XIAO ESP32-C6 or ESP32 DevKit V1")]],
           libsyms]

    for ref, (libid, value, (X, Y)) in PARTS.items():
        X, Y = snap(X), snap(Y)
        inst = ["symbol", ["lib_id", q(libid)], ["at", str(X), str(Y), "0"],
                ["unit", "1"], ["exclude_from_sim", "no"], ["in_bom", "yes"],
                ["on_board", "yes"], ["dnp", "no"], ["uuid", uid()]]
        show_ref = "PWR" not in ref
        inst.append(["property", q("Reference"), q(ref if show_ref else "#FLG" + ref[-1]),
                     ["at", str(X), str(Y - 5), "0"],
                     ["effects", ["font", ["size", "1.27", "1.27"]]]])
        inst.append(["property", q("Value"), q(value),
                     ["at", str(X), str(Y + 5), "0"],
                     ["effects", ["font", ["size", "1.27", "1.27"]]]])
        inst.append(["property", q("Footprint"), q(FOOTPRINTS.get(ref, "")),
                     ["at", str(X), str(Y), "0"],
                     ["effects", ["font", ["size", "1.27", "1.27"]], "hide"]])
        for num, _nm, _xy in pinmaps[libid]:
            inst.append(["pin", q(num), ["uuid", uid()]])
        refname = ref if show_ref else "#FLG" + ref[-1]
        inst.append(["instances", ["project", q("RFID_Door_Controller"),
                     ["path", q("/"), ["reference", q(refname)], ["unit", "1"]]]])
        doc.append(inst)

        # labels / no-connects at pin endpoints
        for num, _nm, (px, py) in pinmaps[libid]:
            gx, gy = snap(X + px), snap(Y - py)
            rp = (ref, num)
            if rp in pin2net:
                doc.append(["global_label", q(pin2net[rp]), ["shape", "passive"],
                            ["at", str(gx), str(gy), "0"],
                            ["effects", ["font", ["size", "1.27", "1.27"]],
                             ["justify", "left"]], ["uuid", uid()]])
            elif rp in NC:
                doc.append(["no_connect", ["at", str(gx), str(gy)], ["uuid", uid()]])
            elif ref.startswith("PWR"):
                pass  # PWR_FLAG pin handled via NETS
            else:
                raise AssertionError(f"pin {rp} has no net and no NC")

    doc.append(["sheet_instances", ["path", q("/"), ["page", q("1")]]])
    doc.append(["embedded_fonts", "no"])

    with open(OUT, "w", encoding="utf-8") as f:
        f.write(ser(doc) + "\n")
    print("wrote", OUT)

if __name__ == "__main__":
    main()


