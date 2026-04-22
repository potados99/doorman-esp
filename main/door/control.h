#ifndef DOORMAN_ESP_DOOR_CONTROL_H
#define DOORMAN_ESP_DOOR_CONTROL_H

void door_control_init();

// Triggers a 500ms HIGH pulse on the door GPIO pin.
// Returns true if pulse was started, false if already in progress.
bool door_trigger_pulse();

#endif //DOORMAN_ESP_DOOR_CONTROL_H
