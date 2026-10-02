// Product features switched at compile time.
//
// A feature that is off is off everywhere: no GPIO configured, no
// interrupt, no setting, no event, no alarm. Its fields in the trip
// record stay, written as "not fitted", so the record layout does not
// change with the feature set and a reader never has to guess which
// layout it is looking at.
#pragma once

// Door contact on GPIO7. Off by decision, 2026-10-02. With it off the
// record's door byte is 0 (unknown / not fitted) and its count 0.
#define MCOLD_DOOR 0

// GPIO7 level that means "shut", if the door is ever switched back on.
// Never checked with a real magnet in a real lid [VERIFY].
#define MCOLD_DOOR_CLOSED_LEVEL 0
