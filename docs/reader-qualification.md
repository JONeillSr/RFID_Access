# Reader qualification checklist

Run this on the bench **before using any reader other than a Paxton P-series on
Clock & Data**. One pass per reader *model*, not per door. Keep the completed
copy with the model name and firmware version: it is the evidence behind
"supported".

**Reader:** ______________________ **Model / part no.:** ______________________
**Output format(s):** ______________ **Card type(s) tested:** ______________
**Controller board:** ______________ **Firmware:** ______ **Date / tester:** ______________

---

## 1. Before connecting anything (datasheet + meter)

- [ ] Supply voltage and current fit the 12 V rail (note the peak current: ______ mA).
- [ ] Data outputs are **open-collector / open-drain**, or push-pull at **3.3 V or
      lower**. A 5 V push-pull output **must not** go straight to an ESP32 pin —
      it needs a level shifter. *If unclear, stop here.*
- [ ] Reader powered, data lines **not** connected: measure each data line to 0V.
      Idle reading: D0 ______ V, D1 ______ V (with 10K pull-ups to the clean 3.3 V
      rail, expect ~3.3 V; anything above ~3.5 V → level shifter).
- [ ] LED and beeper control lines identified; idle voltage on each: ______ V.
      Above 3.3 V → drive through an NPN/N-MOSFET, not the GPIO.
- [ ] Wire colours recorded (don't assume the Wiegand convention — Paxton doesn't
      follow it, and others may not either):
      D0 ______ D1 ______ LED ______ Beep ______ 12V ______ 0V ______

## 2. Wiring and setup

- [ ] Reader 0V is common with controller GND.
- [ ] D0 → **Data** terminal, D1 → **Clock** terminal.
- [ ] `/setup` → **Reader format: Wiegand** → save → **reboot**.
- [ ] `/status` shows `Reader: Wiegand`.

## 3. Reads

Tap each test card and watch `/status` and `/webserial`.

- [ ] Edge count rises on every tap (nothing → wiring/ground, go back to §2).
- [ ] Every tap produces a card number, error count stays flat.
      Format reported: `Wiegand-____`
- [ ] **20 taps of the same card give the same number 20 times.** Any variation
      means a noisy line or an unvalidated frame length — not supported.
- [ ] Three different cards give three different numbers.
- [ ] Record the reported number against what's printed on each card.
      (Wiegand-26 reports facility + card as **one combined number**, so it will
      not match the printed card number. Enrol by tapping the card and taking the
      number from **Unknown cards seen** in the admin app, not by typing it.)

| Card | Printed number | Reported number | Format |
|------|----------------|-----------------|--------|
|      |                |                 |        |
|      |                |                 |        |
|      |                |                 |        |

- [ ] Frame length is **26 bits**. Any other length is accepted raw with **no
      parity check** today — note it, and treat the reader as *not yet
      supported* until that format is decoded (ROADMAP Phase 8, Step 1).
- [ ] Held card: hold a card on the reader for 5 s and note how often it re-sends:
      ______. The firmware ignores repeats of the same card within 1.5 s; a reader
      that re-sends faster than that is fine, one that re-sends every 2–3 s will
      log several grants per presentation.

## 4. Grant / deny behaviour

- [ ] Enrolled card → relay releases, strike opens, event reaches the admin app.
- [ ] Unenrolled card → denied, appears under **Unknown cards seen**.
- [ ] Door-side feedback: reader LED/beeper on grant ______ on deny ______
      (single-LED readers may not show distinct grant/deny yet — record what it
      does so the site knows what to expect).

## 5. Stress and field conditions

- [ ] Reader on a **long cable run** representative of the site (at least 25 m
      if possible): all §3 reads still clean, `repaired` count flat.
- [ ] Strike fired 20 times in a row (flyback diode fitted): no missed reads, no
      reader lock-up afterwards.
- [ ] Controller reboot and power cycle: reader comes back without re-pairing
      or reconfiguration, format still Wiegand.
- [ ] Wi-Fi disconnected: taps still grant and deny locally, and the events
      upload once Wi-Fi returns.
- [ ] Reader disconnected mid-operation, then reconnected: resumes reading.
      (Wiegand cannot report the disconnect — note this for the customer.)

## 6. Result

- [ ] **Qualified** — add the model to the supported list in the README.
- [ ] **Qualified with conditions:** ______________________________________
- [ ] **Not qualified — reason:** ____________________________________________

Notes:

