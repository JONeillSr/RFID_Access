# RFID Door Controller — PCB Design Spec

Carrier board integrating the bench-proven breadboard build. Single 2-layer
board, all through-hole, hand-solderable. Decisions: hybrid integration
(MCU + buck module socketed, everything else discrete), pluggable
Phoenix-style terminal blocks for field wiring.

**Universal MCU support:** the board carries TWO sockets — XIAO ESP32-C6
(U1) and classic ESP32 DevKit (U3) — sharing all peripheral nets, each wired
at the pins from `src/Pins.h` for its board. Populate exactly ONE module per
board; the empty socket is inert. Same firmware, selected by build flag.

KiCad 10 project lives in `hardware/RFID_Door_Controller/`.
Official Seeed XIAO libraries in `hardware/lib/` (symbol
`Seeed_Studio_XIAO_Series`, footprint `XIAO-ESP32-C6-DIP`).

## Power architecture

```
J1 12V in ──┬─────────────────────────── 12V_RAW ── reader 12V, relay COM
            └─ D1 1N4007 ── 12V_PROT ── J2 buck IN+
J2 buck OUT+ ── 5V ──┬── D2 1N5819 ── XIAO 5V pad   (diode = USB can stay
                     ├── K1 relay coil               plugged in while field
                     └── U2 LD1117V33 ── 3V3_PU      power is on)
```

- **One buck only** (12→5 V, trimmed to **5.0–5.2 V** — the relay coil is a
  5 V ±10 % part and the XIAO 5V pad prefers ~5 V). The old 3.3 V buck module
  is retired: the 3.3 V rail is linear on purpose.
- **3V3_PU** is the dedicated stiff pull-up rail (LD1117V33 TO-220 +
  10 µF in / 10 µF + 100 nF out). Bench-proven requirement: pull-ups must NOT
  reference the XIAO's own 3V3 pin (it bounces with WiFi bursts and caused
  duplicated clock bits). An LDO is chosen over a buck here deliberately:
  the load is microamps, and a linear rail is quieter and stiffer — noise on
  this reference was the root cause of the duplicated-bit reads.
- A 7805-style linear regulator is NOT a substitute for the 12→5 V buck:
  at 13.8 V in it would dissipate >2 W continuously in a closed cabinet.
- OLED keeps running from the XIAO's 3V3 pin (proven; ~20 mA).
- Strike current never flows through D1 — the relay contact path is 12V_RAW.

## Connectors (field wiring = 5.08 mm pluggable terminal blocks)

| Ref | Type | Pins | Function |
|-----|------|------|----------|
| J1 | Pluggable 2P | 12V, GND | 12 V DC input from PSU |
| J3 | Pluggable 8P | 12V_RAW, GND, DATA, CLOCK, LED_R, LED_G, LED_A, MD | Paxton P50 (yellow, blue, purple, green, orange, white per verified loom) |
| J4 | Pluggable 3P | NO, NC, GND | Strike: fail-secure on NO, maglock on NC; COM is tied to 12V_RAW on-board. Flyback 1N4007 fits AT the strike |
| J5 | Pluggable 2P | EXIT, GND | Exit button (N.O. momentary) |
| J2A/J2B | 2× socket 1×8, 2.54 | corners: IN+, IN−, OUT+, OUT− | HW-411 buck drops in on its soldered headers. **Measured: rows 39.0 mm apart (c-to-c), 8 pins/row, ± terminals at the outermost positions, + rail corners on the same board side.** Middle 12 positions mechanical only. Module is rotationally symmetric — silk copies the module's own IN+/IN−/arrow markings; match silk to silk |
| J6 | Header 1×4, 2.54 | 3V3, GND, SDA, SCL | SSD1306 OLED |
| J7 | Header 1×4, 2.54 | R, G, B, GND | Panel LED (R5-R7 on board). R/B only active with the DevKit fitted; the C6 drives green only |

## Net list — DevKit socket (U3, classic ESP32 DevKit)

Per `Pins.h` BOARD_ESP32DEV: DATA=19, CLOCK=18, RED=17, GREEN=23, AMBER=4,
BUZZER=13, RELAY_CTL=14, SDA=21, SCL=22, EXIT=32, PANEL R/G/B=25/26/27.
5 V from the buck enters at **VIN** (through D2, shared with the XIAO 5V
pad); grounds common. Socket: **confirmed 30-pin DOIT DEVKIT V1** (silk
verified from photo), 2× 1×15 female headers, 2.54 pitch, **rows 25.4 mm
apart (measured)**. Stock footprint `PinSocket_1x15_P2.54mm_Vertical`, one
per row, spacing enforced at layout.

## Net list (XIAO pad → net)

| XIAO pad | GPIO | Net | Notes |
|----------|------|-----|-------|
| D0 | GPIO0 | AMBER_LED | reader amber, open-drain in FW |
| D1 | GPIO1 | RELAY_CTL | → R3 1k → Q1 base; R4 10k pull-down to GND (boot-safe) |
| D2 | GPIO2 | BUZZER | → BZ1 active buzzer + |
| D3 | GPIO21 | RED_LED | reader red (purple wire) |
| D4 | GPIO22 | SDA | OLED (use raw GPIO numbers in FW — D4/D5 aliases broken) |
| D5 | GPIO23 | SCL | OLED |
| D6 | GPIO16 | EXIT_BTN | J5; internal pull-up in FW |
| D7 | GPIO17 | PANEL_LED | → R5 330R → J7 |
| D8 | GPIO19 | DATA | reader Data/D0 (yellow); R1 10k → 3V3_PU |
| D9 | GPIO20 | CLOCK | reader Clock/D1 (blue); R2 10k → 3V3_PU |
| D10 | GPIO18 | GREEN_LED | reader green |
| 3V3 | — | 3V3_XIAO | OLED power only — NOT the pull-up rail |
| GND | — | GND | common ground plane |
| 5V | — | 5V_XIAO | fed via D2 (1N5819) from buck 5V |

## BOM (all through-hole)

| Ref | Part | Package / KiCad footprint |
|-----|------|---------------------------|
| U1 | Seeed XIAO ESP32-C6 | `XIAO-ESP32-C6-DIP` + 2× 1×7 female header 2.54 |
| U2 | LD1117V33 | TO-220 (`Package_TO_SOT_THT:TO-220-3_Vertical`) |
| K1 | SONGLE SRD-05VDC-SL-C | `Relay_THT:Relay_SPDT_SANYOU_SRD_Series_Form_C` |
| Q1 | 2N3904 NPN | TO-92 |
| D1, D3 | 1N4007 | DO-41 (D1 = input series, D3 = relay coil flyback) |
| D2 | 1N5819 schottky | DO-41 |
| R1, R2 | 10 kΩ | axial 1/4 W (data/clock pull-ups to 3V3_PU) |
| R3 | 1 kΩ | axial (Q1 base) |
| R4 | 10 kΩ | axial (RELAY_CTL pull-down — boot-glitch guard) |
| R5 | 330 Ω | axial (panel LED) |
| C1, C2 | 10 µF electrolytic | radial 2.5 mm (LDO in/out) |
| C3 | 100 nF | ceramic 2.5 mm |
| C4 | 100 µF electrolytic | radial (12 V input bulk) |
| BZ1 | active buzzer 12 mm | `Buzzer_Beeper:Buzzer_12x9.5RM7.6` |
| J1, J5 | pluggable terminal 2P 5.08 | Phoenix MSTB 2,5/2-G + plugs |
| J4 | pluggable terminal 3P 5.08 | Phoenix MSTB 2,5/3-G + plug |
| J3 | pluggable terminal 8P 5.08 | Phoenix MSTB 2,5/8-G + plug |
| J2, J6 | pin header 1×4, 2.54 | Pin_Header_1x04 |
| J7 | pin header 1×2, 2.54 | Pin_Header_1x02 |
| H1–H4 | M3 mounting holes | 3.2 mm, 6 mm annular keepout |

## Layout intent

- ~100 × 70 mm outline; terminal blocks along one long edge (field wiring
  side), XIAO + OLED header toward the opposite edge, USB-C accessible.
- GND pour both layers. 12 V/strike traces ≥ 1.5 mm; logic 0.3 mm.
- Relay + strike terminals grouped in one corner, away from DATA/CLOCK entry.
- Silkscreen: every terminal position labeled with function AND Paxton wire
  color (e.g. "DATA (yel)"), board name + rev, and the wire-color legend.

## Deliberately NOT on the board

- Strike flyback diode (mounts at the strike itself — clamp at the source).
- Reader pull-up alternatives: R1/R2 are 10K; long runs may prefer 4.7K —
  through-hole, trivially swappable.
- Second door / Wiegand reader support: this is a one-door controller,
  matching the firmware.
