#include "DoorContact.h"

void DoorContact::begin(uint32_t nowMs, bool rawOpen, uint32_t heldLimitMs) {
    _open         = rawOpen;
    _pending      = false;
    _openedAt     = nowMs;
    // Found open: not an opening we saw, so not forced -- but it is open with no
    // release, so it is timed for held open from now.
    _heldTiming   = rawOpen;
    _heldFrom     = nowMs;
    _heldReported = false;
    _forced       = false;
    _lastOpenMs   = 0;
    _heldLimitMs  = heldLimitMs;
}

DoorContact::Event DoorContact::update(uint32_t nowMs, const Inputs& in) {
    // ---- debounce -------------------------------------------------------------
    bool changed = false;
    if (in.rawOpen != _open) {
        if (!_pending) {
            _pending      = true;
            _pendingSince = nowMs;
        } else if (nowMs - _pendingSince >= DEBOUNCE_MS) {
            _pending = false;
            _open    = in.rawOpen;
            changed  = true;
        }
    } else {
        _pending = false;                 // a bounce that came back: not a change
    }

    const bool authorised = in.released || in.scheduleOpen;

    // ---- opened ---------------------------------------------------------------
    if (changed && _open) {
        _openedAt     = nowMs;
        _heldReported = false;
        _forced       = !authorised;
        _heldTiming   = !authorised;      // a forced door is timed from opening
        _heldFrom     = nowMs;
        return _forced ? FORCED : NONE;
    }

    // ---- closed ---------------------------------------------------------------
    if (changed && !_open) {
        _lastOpenMs = nowMs - _openedAt;
        const bool wasHeld = _heldReported;
        _heldTiming   = false;
        _heldReported = false;
        _forced       = false;
        return wasHeld ? HELD_CLOSED : NONE;
    }

    // ---- still open -----------------------------------------------------------
    if (_open) {
        if (authorised) {
            // Released again, or inside an unlock window: nothing is being held
            // open against the rules, so the clock stops until that ends.
            _heldTiming = false;
        } else if (!_heldTiming) {
            _heldTiming = true;
            _heldFrom   = nowMs;
        }
        if (_heldTiming && !_heldReported && _heldLimitMs > 0 &&
            nowMs - _heldFrom >= _heldLimitMs) {
            _heldReported = true;
            return HELD;
        }
    }
    return NONE;
}
