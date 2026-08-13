#include "PaxtonReader.h"

PaxtonReader::PaxtonReader(int dataPin, int clockPin,
                           int ledRedPin, int ledGreenPin, int ledAmberPin,
                           Mode mode)
    : _dataPin(dataPin), _clockPin(clockPin),
      _ledR(ledRedPin), _ledG(ledGreenPin), _ledA(ledAmberPin),
      _mode(mode) {}

void PaxtonReader::begin() {
    // Internal pull-ups as a baseline; external 10K to 3V3 recommended on
    // long cable runs (the internal ones are weak, ~45K).
    pinMode(_dataPin,  INPUT_PULLUP);
    pinMode(_clockPin, INPUT_PULLUP);

    // LED lines: open-drain, released (HIGH = high-Z = LED off). Open-drain
    // means the pin never sources voltage onto the reader's LED line — it can
    // only sink it to ground, which is exactly how the Net2 drives them.
    for (int pin : { _ledR, _ledG, _ledA }) {
        if (pin < 0) continue;
        pinMode(pin, OUTPUT_OPEN_DRAIN);
        digitalWrite(pin, HIGH);
    }

    if (_mode == CLOCK_AND_DATA) {
        // Magstripe convention: data is valid on the clock falling edge.
        attachInterruptArg(digitalPinToInterrupt(_clockPin), isrClock, this, FALLING);
    } else {
        attachInterruptArg(digitalPinToInterrupt(_dataPin),  isrData0, this, FALLING);
        attachInterruptArg(digitalPinToInterrupt(_clockPin), isrData1, this, FALLING);
    }
}

// -- ISRs ---------------------------------------------------------------------

void IRAM_ATTR PaxtonReader::isrClock(void* arg) {
    PaxtonReader* self = static_cast<PaxtonReader*>(arg);
    // Data line is active-low: low at the clock edge = logical 1.
    self->pushBit(digitalRead(self->_dataPin) == LOW ? 1 : 0);
}

void IRAM_ATTR PaxtonReader::isrData0(void* arg) {
    static_cast<PaxtonReader*>(arg)->pushBit(0);
}

void IRAM_ATTR PaxtonReader::isrData1(void* arg) {
    static_cast<PaxtonReader*>(arg)->pushBit(1);
}

void IRAM_ATTR PaxtonReader::pushBit(uint8_t bit) {
    portENTER_CRITICAL_ISR(&_mux);
    uint32_t now = micros();
    // Glitch filter: a slow rising edge crossing the threshold noisily can
    // re-fire the interrupt within microseconds. Real bits never arrive that
    // fast, so drop the edge (but keep the frame-gap timer fresh).
    if (_count > 0 && (uint32_t)(now - _lastEdgeUs) < MIN_EDGE_GAP_US) {
        _lastEdgeUs = now;
        portEXIT_CRITICAL_ISR(&_mux);
        return;
    }
    _edges++;
    if (_count < MAX_BITS) _bits[_count++] = bit;
    _lastEdgeUs = now;
    portEXIT_CRITICAL_ISR(&_mux);
}

// -- Frame collection -----------------------------------------------------------

bool PaxtonReader::poll(Credential& out) {
    uint8_t  local[MAX_BITS];
    uint16_t n = 0;

    portENTER_CRITICAL(&_mux);
    if (_count > 0 && (uint32_t)(micros() - _lastEdgeUs) > FRAME_GAP_US) {
        n = _count;
        for (uint16_t i = 0; i < n; i++) local[i] = _bits[i];
        _count = 0;
    }
    portEXIT_CRITICAL(&_mux);

    if (n == 0) return false;

    // Keep a printable copy of the raw frame for diagnostics (as captured,
    // before any polarity games below).
    for (uint16_t i = 0; i < n; i++) _lastFrame[i] = local[i] ? '1' : '0';
    _lastFrame[n] = '\0';

    memset(&out, 0, sizeof(out));
    bool ok;
    if (_mode == CLOCK_AND_DATA) {
        ok = decodeClockAndData(local, n, out);
        if (!ok) ok = decodeRepairedCND(local, n, out);
        if (!ok) {
            // Retry with inverted data polarity: some readers/wirings yield
            // an inverted data line relative to the ABA convention. A frame
            // that decodes cleanly inverted is unambiguous — parity and both
            // sentinels all have to line up — so this is safe to accept.
            for (uint16_t i = 0; i < n; i++) local[i] ^= 1;
            ok = decodeClockAndData(local, n, out);
            if (!ok) ok = decodeRepairedCND(local, n, out);
        }
    } else {
        ok = decodeWiegand(local, n, out);
    }
    if (!ok) _errors++;
    return ok;
}

// A marginal clock edge that slips past the ISR filter duplicates one bit
// mid-frame. Deleting each bit position in turn and re-running the strict
// parse recovers such frames; the parity + sentinel + LRC gauntlet makes a
// false accept effectively impossible. Runs only on frames that failed the
// strict parse, in task context (never in the ISR).
bool PaxtonReader::decodeRepairedCND(const uint8_t* bits, uint16_t n,
                                     Credential& out) {
    if (n < 10 || n > MAX_BITS) return false;
    uint8_t work[MAX_BITS];
    for (uint16_t p = 0; p < n; p++) {
        uint16_t w = 0;
        for (uint16_t i = 0; i < n; i++)
            if (i != p) work[w++] = bits[i];
        if (decodeClockAndData(work, w, out)) {
            _repairs++;
            return true;
        }
    }
    return false;
}

// -- Clock & Data (ABA track 2) -------------------------------------------------
//
// Characters are 5 bits, LSB first: 4 data bits + 1 odd-parity bit.
//   start sentinel ';' = 0xB   -> bits 1,1,0,1 + parity 0
//   digits '0'-'9'     = 0x0-0x9
//   field separator '='= 0xD   -> ends the card number (EM-format tokens
//                                 transmit one before the end sentinel)
//   end sentinel   '?' = 0xF   -> bits 1,1,1,1 + parity 1
// The frame is padded with zero bits before and after, and an LRC character
// (XOR of every character including both sentinels) closes it.
//
// Validation is strict — per-character odd parity AND the LRC must check out,
// or the whole frame is rejected rather than risking a corrupted credential.
//
// Alignment: real captures can carry spurious edges before the frame proper
// (seen on the bench as one extra '1' immediately ahead of the sentinel), so
// the decoder scans for the start-sentinel bit pattern and attempts a full
// parse from each occurrence, instead of trusting the first 1 bit it sees.
// A false alignment cannot survive the parity + sentinel + LRC gauntlet.

bool PaxtonReader::decodeClockAndData(const uint8_t* bits, uint16_t n,
                                      Credential& out) {
    for (uint16_t s = 0; s + 5 <= n; s++) {
        // 0xB LSB-first with parity 0 -> pattern 1,1,0,1,0
        if (bits[s] == 1 && bits[s + 1] == 1 && bits[s + 2] == 0 &&
            bits[s + 3] == 1 && bits[s + 4] == 0) {
            if (tryClockAndDataFrom(bits, n, s, out)) return true;
        }
    }
    return false;
}

bool PaxtonReader::tryClockAndDataFrom(const uint8_t* bits, uint16_t n,
                                       uint16_t i, Credential& out) {
    char     digits[sizeof(out.number)];
    uint8_t  nd  = 0;
    uint8_t  lrc = 0xB;          // running XOR, seeded with the start sentinel
    bool     sepSeen = false, ended = false;

    i += 5;                      // past the start sentinel (already matched)

    while (i + 5 <= n) {
        uint8_t value = 0, ones = 0;
        for (uint8_t b = 0; b < 5; b++) {
            if (bits[i + b]) {
                ones++;
                if (b < 4) value |= (1 << b);   // LSB first
            }
        }
        i += 5;
        if ((ones & 1) == 0) return false;      // odd parity violated

        if (value == 0xF) {
            // End sentinel: the next character is the LRC. Verify it.
            lrc ^= 0xF;
            if (i + 5 > n) return false;
            uint8_t lval = 0, lones = 0;
            for (uint8_t b = 0; b < 5; b++) {
                if (bits[i + b]) {
                    lones++;
                    if (b < 4) lval |= (1 << b);
                }
            }
            if ((lones & 1) == 0) return false;
            if (lval != lrc) return false;      // checksum mismatch
            ended = true;
            break;
        }

        lrc ^= value;
        if (value <= 9) {
            // Digits after a field separator are discretionary data, not part
            // of the card number; they still count toward the LRC above.
            if (!sepSeen) {
                if (nd >= sizeof(digits) - 1) return false;
                digits[nd++] = char('0' + value);
            }
        } else if (value == 0xD) {
            if (sepSeen) return false;          // one separator at most
            sepSeen = true;
        } else {
            return false;                       // 0xA / 0xE never appear
        }
    }

    if (!ended || nd == 0) return false;

    digits[nd] = '\0';
    strncpy(out.number, digits, sizeof(out.number) - 1);
    strncpy(out.format, "Clock&Data", sizeof(out.format) - 1);
    return true;
}

// -- Wiegand ------------------------------------------------------------------

bool PaxtonReader::decodeWiegand(const uint8_t* bits, uint16_t n,
                                 Credential& out) {
    if (n < 4 || n > 64) return false;

    uint64_t value = 0;

    if (n == 26) {
        // Standard 26-bit: even parity over bits 0-12, odd over bits 13-25.
        uint8_t ones1 = 0, ones2 = 0;
        for (uint8_t i = 0;  i < 13; i++) ones1 += bits[i];
        for (uint8_t i = 13; i < 26; i++) ones2 += bits[i];
        if ((ones1 & 1) != 0 || (ones2 & 1) != 1) return false;
        // Strip the two parity bits; keep the 24-bit payload (facility+card).
        for (uint8_t i = 1; i < 25; i++) value = (value << 1) | bits[i];
        strncpy(out.format, "Wiegand-26", sizeof(out.format) - 1);
    } else {
        // Unknown length: no parity scheme assumed, return the raw payload.
        for (uint16_t i = 0; i < n; i++) value = (value << 1) | bits[i];
        snprintf(out.format, sizeof(out.format), "Wiegand-%u", n);
    }

    snprintf(out.number, sizeof(out.number), "%llu", (unsigned long long)value);
    return true;
}

// -- Reader LEDs ----------------------------------------------------------------

void PaxtonReader::ledWrite(int pin, bool on) {
    if (pin < 0) return;
    digitalWrite(pin, on ? LOW : HIGH);   // active-low: sink to light
}

void PaxtonReader::ledSet(bool red, bool green, bool amber) {
    ledWrite(_ledR, red);
    ledWrite(_ledG, green);
    ledWrite(_ledA, amber);
}

void PaxtonReader::ledIdle()    { ledSet(false, false, true ); }
void PaxtonReader::ledGranted() { ledSet(false, true,  false); }
void PaxtonReader::ledDenied()  { ledSet(true,  false, false); }
void PaxtonReader::ledsOff()    { ledSet(false, false, false); }
