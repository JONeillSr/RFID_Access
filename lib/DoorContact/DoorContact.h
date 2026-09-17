/**
 * @file    DoorContact.h
 * @brief   Door-position decisions: was the door forced, is it held open.
 *
 * Without a contact the firmware knows only that the relay fired, not that the
 * door opened. That leaves three things invisible: a release nobody walks
 * through, a door propped open after a legitimate release, and a door forced
 * with no release at all. A reed contact on the frame makes the last two
 * detectable, and this class makes the decisions.
 *
 * NO HARDWARE IN HERE. The caller reads the pin and says whether a release or a
 * scheduled unlock window is in effect; this class only keeps state and decides.
 * That keeps the rules readable in one place and away from pins and timers.
 *
 * THE RULES
 * ---------
 * - FORCED: the door opens while nothing released it. Judged on the opening
 *   itself, so a door that was already open at boot is never "forced" -- no
 *   opening was seen.
 * - HELD: the door has been open longer than the limit since the last release
 *   ended. A door propped during a delivery is timed from when the relay
 *   dropped; a forced door from when it opened. A release or an unlock window
 *   while it is open restarts the clock.
 * - HELD_CLOSED: a door that was reported held open has closed again. The
 *   caller records how long it was open.
 *
 * Readings are debounced: a reed switch chatters as a door swings or slams, and
 * one bounce must not become an opening.
 *
 * Not thread-safe. Call update() from one task only; the accessors are plain
 * scalar reads and are fine for a status page.
 */

#pragma once
#include <cstdint>

class DoorContact {
public:
    enum Event : uint8_t {
        NONE = 0,
        FORCED,        // opened while no release was in effect
        HELD,          // open past the limit since the last release ended
        HELD_CLOSED,   // a door reported held open has closed; see lastOpenMs()
    };

    struct Inputs {
        bool rawOpen;        // the contact reads open right now (not debounced)
        bool released;       // a grant or exit release is in effect, grace included
        bool scheduleOpen;   // a scheduled unlock window is open
    };

    /// A change must hold this long before it counts.
    static const uint32_t DEBOUNCE_MS = 150;

    /// Start from the contact's current reading. heldLimitMs 0 = no held alarm.
    void begin(uint32_t nowMs, bool rawOpen, uint32_t heldLimitMs);

    /// Call regularly (every 100 ms is plenty). Returns at most one event.
    Event update(uint32_t nowMs, const Inputs& in);

    bool     isOpen() const            { return _open; }
    bool     heldReported() const      { return _heldReported; }
    bool     forcedThisOpening() const { return _forced; }
    uint32_t openForMs(uint32_t nowMs) const { return _open ? nowMs - _openedAt : 0; }
    uint32_t heldLimitMs() const       { return _heldLimitMs; }

    /// How long the door was open, as of the last close. Meaningful with HELD_CLOSED.
    uint32_t lastOpenMs() const        { return _lastOpenMs; }

private:
    bool     _open         = false;   // debounced state
    bool     _pending      = false;   // raw reading disagrees with _open
    uint32_t _pendingSince = 0;
    uint32_t _openedAt     = 0;
    bool     _heldTiming   = false;   // an unreleased open stretch is being timed
    uint32_t _heldFrom     = 0;
    bool     _heldReported = false;
    bool     _forced       = false;
    uint32_t _lastOpenMs   = 0;
    uint32_t _heldLimitMs  = 0;
};
