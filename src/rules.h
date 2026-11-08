// Firmware glue around the rule engine: thread-safe rule table, scheduling of delayed
// actions (own high-priority task, ~0.1 ms resolution), NVS save/load and the "#rule" command.
#pragma once

#include <stdint.h>

#include "can_frame.h"

void rules_init();

// Called by the CAN RX task for every received frame while ACTIVE.
void rules_on_frame(const CanFrame& f, uint64_t ts_us);

void rules_clear_pending();   // drop all scheduled actions (any task)
void rules_disable_all();     // any task

int rules_count();
int rules_enabled_count();
uint32_t rules_pending_overflows();

// Handles the arguments of "#rule ..." (e.g. "add poll match=..."). Writes the reply lines.
void rules_command(char* args);
