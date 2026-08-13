"""Inspect KiCad symbol libraries: list pins (number, name, type) for the
symbols the RFID door controller schematic needs. Also dump relay footprint
pad numbers. Run with the system Python; no deps."""
import re, sys, os

KICAD_SYM = r"C:\Program Files\KiCad\10.0\share\kicad\symbols"
KICAD_FP  = r"C:\Program Files\KiCad\10.0\share\kicad\footprints"
SEEED_SYM = os.path.join(os.path.dirname(__file__), "..", "lib", "symbols",
                         "Seeed_Studio_XIAO_Series.kicad_sym")

def tokenize(text):
    # minimal s-expression tokenizer
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "()":
            toks.append(c); i += 1
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == '\\': j += 2; continue
                if text[j] == '"': break
                j += 1
            toks.append(text[i:j+1]); i = j + 1
        elif c.isspace():
            i += 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '()"':
                j += 1
            toks.append(text[i:j]); i = j
    return toks

def parse(toks):
    def rd(idx):
        assert toks[idx] == "("
        out = []; idx += 1
        while toks[idx] != ")":
            if toks[idx] == "(":
                node, idx = rd(idx)
                out.append(node)
            else:
                out.append(toks[idx]); idx += 1
        return out, idx + 1
    node, _ = rd(0)
    return node

def load_lib(path):
    with open(path, encoding="utf-8") as f:
        return parse(tokenize(f.read()))

def sym_index(tree):
    return {s[1].strip('"'): s for s in tree[1:]
            if isinstance(s, list) and s and s[0] == "symbol"}

def walk_pins(node, out):
    if isinstance(node, list):
        if node and node[0] == "pin":
            etype = node[1] if len(node) > 1 and isinstance(node[1], str) else "?"
            name = number = "?"
            for sub in node:
                if isinstance(sub, list):
                    if sub[0] == "name":   name = sub[1].strip('"')
                    if sub[0] == "number": number = sub[1].strip('"')
            out.append((number, name, etype))
        for sub in node:
            walk_pins(sub, out)

def show(libfile, wanted, contains=None):
    tree = sym_index(load_lib(libfile))
    names = list(tree)
    if contains:
        matches = [n for n in names if contains.lower() in n.lower()]
        print(f"  [{os.path.basename(libfile)}] symbols matching '{contains}': {matches}")
        wanted = wanted + matches[:3]
    for w in wanted:
        if w not in tree:
            print(f"  !! {w}: NOT FOUND"); continue
        node = tree[w]
        ext = [s[1].strip('"') for s in node if isinstance(s, list) and s[0] == "extends"]
        pins = []; walk_pins(node, pins)
        pins.sort(key=lambda p: (len(p[0]), p[0]))
        print(f"  {w}: extends={ext or 'no'} pins={pins}")

print("== Device =="); show(os.path.join(KICAD_SYM, "Device.kicad_sym"),
     ["R", "C", "CP", "D", "Q_NPN_EBC", "Buzzer"])
print("== Connector_Generic =="); show(os.path.join(KICAD_SYM, "Connector_Generic.kicad_sym"),
     ["Conn_01x02", "Conn_01x03", "Conn_01x04", "Conn_01x08", "Conn_01x15"])
print("== Relay =="); show(os.path.join(KICAD_SYM, "Relay.kicad_sym"), [], contains="SRD")
print("== Regulator_Linear =="); show(os.path.join(KICAD_SYM, "Regulator_Linear.kicad_sym"), [], contains="LD1117V33")
print("== power =="); show(os.path.join(KICAD_SYM, "power.kicad_sym"), ["PWR_FLAG", "GND", "+5V", "+12V", "+3V3"])
print("== Seeed =="); show(SEEED_SYM, [], contains="C6")

fp = os.path.join(KICAD_FP, "Relay_THT.pretty", "Relay_SPDT_SANYOU_SRD_Series_Form_C.kicad_mod")
print("== Relay footprint pads ==")
with open(fp, encoding="utf-8") as f:
    pads = re.findall(r'\(pad\s+"([^"]+)"\s+(\S+)\s+(\S+)', f.read())
print(" ", pads)
