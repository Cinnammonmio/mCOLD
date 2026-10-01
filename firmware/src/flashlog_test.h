// Power-cut tests for the trip log, against a RAM image of NOR flash.
// Prints one line per test; returns the number that failed.
#pragma once

int flashlog_selftest(void);
