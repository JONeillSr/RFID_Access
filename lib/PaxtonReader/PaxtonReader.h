/**
 * @file    PaxtonReader.h
 * @brief   Driver for Paxton P-series proximity readers (e.g. P50 / 345-110-US)
 *          and other Clock & Data or Wiegand readers.
 *
 * Unlike the MFRC522 (an SPI peripheral the MCU polls), a Paxton reader is a
 * transmitter: each card read is pushed out spontaneously on two open-collector
 * lines. This driver captures those transmissions with GPIO interrupts and
 * decodes them into a credential string.
 *
 * Supported line formats:
 *   CLOCK_AND_DATA (default) — Paxton's native output; ABA track-2 magstripe
 *     emulation. Bits are sampled on the CLOCK falling edge; DATA low = 1.
 *     Frames are leading zeros, start sentinel (0xB), 4-bit BCD digits with
 *     odd parity, end sentinel (0xF), LRC. Yields the card number as printed
 *     on the token (decimal digits).
 *   WIEGAND — pulse-per-bit on two lines (D0 = 0, D1 = 1). 26-bit frames get
 *     parity validation and parity stripping; other lengths are returned raw.
 *
 * Electrical notes (reader side is 12 V powered, data lines open-collector):
 *   - DATA/CLOCK: enable internal pull-ups here, but fit external 10K pull-ups
 *     to the ESP32's 3V3 rail for reliability on long runs. NEVER pull up to
 *     5 V or 12 V — ESP32 pins are not 5 V tolerant. Verify with a meter that
 *     the lines idle at ~3.3 V before connecting.
 *   - LED lines: active-low, driven in open-drain mode (pin sinks to light the
 *     LED, floats to release it). Measure the idle voltage on each LED line
 *     first: if it floats above 3.3 V, sink it through a small NPN/N-MOSFET
 *     instead of the GPIO directly.
 *
 * Threading model: ISRs only append bits to a buffer. Call poll() from a task
 * loop; it detects end-of-frame by inter-edge gap, decodes, and returns the
 * credential. One instance per reader; multiple instances are supported via
 * attachInterruptArg.
 */

#pragma once
#include <Arduino.h>

class PaxtonReader {
public:
    enum Mode : uint8_t {
        CLOCK_AND_DATA,   // Paxton native (default for P-series on Net2)
        WIEGAND,          // dataPin = D0, clockPin = D1
    };

    struct Credential {
        char number[20];  // decimal card number, NUL-terminated
        char format[16];  // "Clock&Data", "Wiegand-26", "Wiegand-N"
    };

    /// LED pins may be -1 if a line isn't wired; those calls become no-ops.
    PaxtonReader(int dataPin, int clockPin,
                 int ledRedPin = -1, int ledGreenPin = -1, int ledAmberPin = -1,
                 Mode mode = CLOCK_AND_DATA);

    /// Configure pins and attach interrupts. Call once from setup().
    void begin();

    /// Non-blocking. Returns true when a complete, validated card read is
    /// available and fills `out`. Call every few ms from a task loop.
    bool poll(Credential& out);

    /// Frames that arrived but failed validation (parity/sentinel) since boot.
    /// Useful as a wiring/format diagnostic: taps that bump this counter but
    /// never yield a credential usually mean the wrong mode or a swapped pair.
    uint32_t errorCount() const { return _errors; }

    /// Raw interrupt edges captured since boot — the lowest-level "is anything
    /// arriving at all" diagnostic. A tap should add roughly 80+ edges. Taps
    /// that leave this at zero mean no signal reaches the pins: check the
    /// common ground between reader 0V and the MCU, then the pull-ups/wiring.
    uint32_t edgeCount() const { return _edges; }

    /// The most recent completed frame as a '0'/'1' string, in arrival order,
    /// refreshed on every frame (pass or fail). The definitive bench
    /// diagnostic: all-same bits = the data pin is on the wrong wire; a
    /// structured pattern that still fails = polarity/edge/format mismatch.
    const char* lastFrame() const { return _lastFrame; }

    /// Frames that only decoded after single-bit repair (a duplicated bit from
    /// a marginal clock edge was dropped). Reads succeed either way; a rising
    /// count is an early warning that the line quality is poor — check the
    /// pull-ups (10K to 3V3, or 4.7K on long runs) and cable routing.
    uint32_t repairCount() const { return _repairs; }

    // -- Reader LED control (active-low open-drain; -1 pins are skipped) ------
    void ledIdle();      // amber only — "ready, present token"
    void ledGranted();   // green only
    void ledDenied();    // red only
    void ledsOff();      // all released

private:
    static void IRAM_ATTR isrClock(void* arg);   // C&D: sample data on clock edge
    static void IRAM_ATTR isrData0(void* arg);   // Wiegand: 0 bit
    static void IRAM_ATTR isrData1(void* arg);   // Wiegand: 1 bit
    void IRAM_ATTR pushBit(uint8_t bit);

    void ledWrite(int pin, bool on);
    void ledSet(bool red, bool green, bool amber);
    bool decodeClockAndData(const uint8_t* bits, uint16_t n, Credential& out);
    bool tryClockAndDataFrom(const uint8_t* bits, uint16_t n, uint16_t start,
                             Credential& out);
    bool decodeRepairedCND(const uint8_t* bits, uint16_t n, Credential& out);
    bool decodeWiegand(const uint8_t* bits, uint16_t n, Credential& out);

    // A frame is over once the lines have been quiet this long. C&D bit clocks
    // run ~1 kHz and Wiegand inter-pulse gaps are ~1-2 ms, so 20 ms is safely
    // past any intra-frame gap while adding no perceptible latency.
    static const uint32_t FRAME_GAP_US = 20000;
    // Edges closer together than this are electrical glitches (a slow rising
    // clock re-crossing the input threshold), not real bits: real C&D clocks
    // and Wiegand pulse intervals are hundreds of microseconds apart.
    static const uint32_t MIN_EDGE_GAP_US = 60;
    static const uint16_t MAX_BITS     = 256;   // 8-digit C&D frame is ~80 bits

    int  _dataPin, _clockPin, _ledR, _ledG, _ledA;
    Mode _mode;

    volatile uint8_t  _bits[MAX_BITS];
    volatile uint16_t _count      = 0;
    volatile uint32_t _lastEdgeUs = 0;
    volatile uint32_t _edges      = 0;
    uint32_t          _errors     = 0;
    uint32_t          _repairs    = 0;
    char              _lastFrame[MAX_BITS + 1] = {0};
    portMUX_TYPE      _mux        = portMUX_INITIALIZER_UNLOCKED;
};
