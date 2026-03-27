#pragma once

void door_control_init();

// Triggers a 500ms HIGH pulse on the door GPIO pin.
// Returns true if pulse was started, false if already in progress.
bool door_trigger_pulse();
