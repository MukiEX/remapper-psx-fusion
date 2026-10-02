#ifndef _PS1_PORT_H_
#define _PS1_PORT_H_

#include <stdint.h>

// PlayStation controller-port side of the adapter.
//
// Emulates a PS1 Mouse, DualShock or NeGcon depending on HID Remapper's
// "Emulated device type" (see ps1_port.cc).
//
// HID Remapper runs on core 0. This module runs the PS1 protocol on core 1
// (the bit-banged state machine from usb-to-ps1-mouse-pro) and receives the
// remapper's output reports through ps1_port_feed_report().

// Call on core 0 after tusb_init(), so PIO-USB has already loaded its programs.
void ps1_port_init();

// Call with every output report HID Remapper produces (report ID first).
void ps1_port_feed_report(const uint8_t* report_with_id, uint8_t len);

// Call from the core 0 main loop. Drives the status LED.
void ps1_port_task();

#endif
