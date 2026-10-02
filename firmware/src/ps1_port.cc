// PlayStation controller-port side of the HID Remapper PS1 adapter.
//
// The PS1 protocol state machine, keyboard->pad key map and LED colours are
// taken from usb-to-ps1-mouse-pro by Vojtech Salajka (MIT License,
// https://github.com/Franticware/usb-to-ps1-mouse-pro). The USB side
// (parsemouse.c and the tuh_hid_* callbacks) is replaced by HID Remapper,
// which feeds its remapped output reports in through ps1_port_feed_report().
//
// Core 0: HID Remapper (USB host, USB device for the web configurator, mapping)
//         + the status LED.
// Core 1: the PS1 state machine below, polling the port pins in a tight loop.
//
// What the port emulates follows HID Remapper's "Emulated device type":
//   Mouse and keyboard  -> PS1 Mouse (SCPH-1090), or digital pad after keyboard input
//   Switch gamepad      -> DualShock (SCPH-1200), digital/analog, config commands
//   PS4 arcade stick    -> DualShock (same as above)
//   Stadia controller   -> Namco NeGcon
//   anything else       -> idle digital pad

#include "ps1_port.h"

#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/timer.h>
#include <pico/critical_section.h>
#include <pico/multicore.h>

#include <tusb.h>

#include "globals.h"
#include "ws2812.pio.h"

#ifndef PS1_GP_ATT
#define PS1_GP_ATT 7
#define PS1_GP_CLK 10
#define PS1_GP_DAT 11
#define PS1_GP_CMD 14
#define PS1_GP_ACK 15
#endif

#define GP_ATT PS1_GP_ATT
#define GP_CLK PS1_GP_CLK
#define GP_DAT PS1_GP_DAT
#define GP_CMD PS1_GP_CMD
#define GP_ACK PS1_GP_ACK

// What the port answers as in "Mouse and keyboard" mode before any report
// arrives. A mouse by default, so games that probe for one at boot find it.
#ifndef PS1_DEFAULT_MODE
#define PS1_DEFAULT_MODE PROT_MOUSE
#endif

// DualShock mode at power-on. A real DualShock starts in digital mode and
// the Analog button (Home on Switch, PS on PS4) toggles it. Set to 1 to start
// in analog mode instead.
#ifndef PS1_DUALSHOCK_START_ANALOG
#define PS1_DUALSHOCK_START_ANALOG 0
#endif

// NeGcon twist direction. Set to 1 if steering comes out reversed.
#ifndef PS1_NEGCON_INVERT_TWIST
#define PS1_NEGCON_INVERT_TWIST 0
#endif

// Protocol timing from the original firmware, in microseconds.
#define PS1_DAT_RELEASE_DELAY_US 15
#define PS1_ACK_PULSE_US 4

// ---------------------------------------------------------------------------
// HID Remapper output types (our_descriptors[] in our_descriptor.cc) and the
// layouts of their reports as they arrive here (report ID byte first; 0 for
// descriptors without report IDs).

#define DESC_MOUSE_KEYBOARD 0
#define DESC_SWITCH 2  // [0][btn 1-8][btn 9-14][hat][LX][LY][RX][RY][vendor]
#define DESC_PS4 3     // [1][LX][LY][RX][RY][hat | btn 1-4 <<4][btn 5-12][btn 13-14 | counter][L2 axis][R2 axis]...
#define DESC_STADIA 4  // [3][hat][16 button bits, see below][LX][LY][RX][RY][brake][accel][consumer]

#define REMAPPER_REPORT_ID_MOUSE 1     // [1][buttons][X lo][X hi][Y lo][Y hi][wheel][pan]
#define REMAPPER_REPORT_ID_KEYBOARD 2  // [2][modifiers][1 bit per usage 0x04..0x73]...
#define REMAPPER_REPORT_ID_PS4 1
#define REMAPPER_REPORT_ID_STADIA 3

#define MOUSE_REPORT_MIN_LEN 6
#define KEYBOARD_REPORT_MIN_LEN 16
#define KEYBOARD_FIRST_USAGE 0x04
#define KEYBOARD_LAST_USAGE 0x73
#define SWITCH_REPORT_MIN_LEN 8
#define PS4_REPORT_MIN_LEN 10
#define STADIA_REPORT_MIN_LEN 10

// ---------------------------------------------------------------------------
// PS1 pad button bits (as sent, before inverting to active-low).

#define PSB_SELECT 0x0001
#define PSB_L3 0x0002
#define PSB_R3 0x0004
#define PSB_START 0x0008
#define PSB_UP 0x0010
#define PSB_RIGHT 0x0020
#define PSB_DOWN 0x0040
#define PSB_LEFT 0x0080
#define PSB_L2 0x0100
#define PSB_R2 0x0200
#define PSB_L1 0x0400
#define PSB_R1 0x0800
#define PSB_TRIANGLE 0x1000
#define PSB_CIRCLE 0x2000
#define PSB_CROSS 0x4000
#define PSB_SQUARE 0x8000

// NeGcon reuses three of those positions for its own digital buttons.
#define NEGCON_R PSB_R1
#define NEGCON_B PSB_TRIANGLE
#define NEGCON_A PSB_CIRCLE

// Switch gamepad and PS4 arcade stick share HID Remapper's 14-button order
// (West, South, East, North, L1, R1, L2, R2, Select, Start, L3, R3, Home, extra).
// Home/PS (index 12) is the DualShock Analog button and is handled separately.
static const uint16_t PAD14_MAP[14] = {
    PSB_SQUARE, PSB_CROSS, PSB_CIRCLE, PSB_TRIANGLE,
    PSB_L1, PSB_R1, PSB_L2, PSB_R2,
    PSB_SELECT, PSB_START, PSB_L3, PSB_R3,
    0, 0,
};
#define PAD14_ANALOG_BUTTON (1 << 12)

// Stadia button bits as laid out in HID Remapper's Stadia report.
#define STADIA_L1 (1 << 10)
#define STADIA_R1 (1 << 9)
#define STADIA_MENU (1 << 5)
#define STADIA_B (1 << 13)
#define STADIA_A (1 << 14)

// Hat switch 0..7 (N, NE, E, SE, S, SW, W, NW); anything else is centred.
static const uint16_t HAT_MAP[8] = {
    PSB_UP, PSB_UP | PSB_RIGHT, PSB_RIGHT, PSB_DOWN | PSB_RIGHT,
    PSB_DOWN, PSB_DOWN | PSB_LEFT, PSB_LEFT, PSB_UP | PSB_LEFT,
};

static uint16_t hat_to_dpad(uint8_t hat) {
    return hat < 8 ? HAT_MAP[hat] : 0;
}

// ---------------------------------------------------------------------------
// Keyboard -> PS1 digital pad map (unchanged from the original, including its
// "KEYMAP>>" / "<<KEYMAP" markers, so the table can still be found in the
// binary).

static const uint16_t KEY_BUTTON_MAP[4 + 8 + 256 + 4] = {
    0x454b, 0x4d59, 0x5041, 0x3e3e, 0,      0,      0,      0,      0x0008,
    0x0001, 0,      0,      0,      0,      0,      0,      0x0080, 0,
    0,      0x0020, 0x0100, 0x4000, 0x2000, 0,      0x0200, 0,      0x8000,
    0x4000, 0,      0,      0x1000, 0x0800, 0x0400, 0x8000, 0x0040, 0x1000,
    0,      0,      0x0010, 0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0x0008, 0,
    0x0001, 0,      0,      0,      0,      0,      0,      0,      0,
    0x2000, 0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0x0020, 0x0080, 0x0040, 0x0010, 0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0,      0,
    0,      0,      0,      0,      0,      0,      0,      0x3c3c, 0x454b,
    0x4d59, 0x5041,
};

static const uint16_t* KEY_PTR = KEY_BUTTON_MAP + 4 + 8;
static const uint16_t* MOD_PTR = KEY_BUTTON_MAP + 4;

// ---------------------------------------------------------------------------
// State shared between core 0 (producer) and core 1 (PS1 protocol).

enum EProt : uint8_t {
    PROT_NONE = 0,
    PROT_KEYB = 1,
    PROT_MOUSE = 2,
    PROT_DUALSHOCK = 3,
    PROT_NEGCON = 4,
};

typedef struct {
    uint16_t buttons;  // PSB_* bits, 1 = pressed
    uint8_t lx, ly, rx, ry;   // DualShock sticks, 0x80 = centre
    uint8_t twist, i, ii, l;  // NeGcon
} PadState;

static const PadState PAD_NEUTRAL = { 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00 };

static critical_section_t shared_cs;

static EProt gContrProt = PS1_DEFAULT_MODE;
static int8_t gSumX = 0;
static int8_t gSumY = 0;
static bool gL = false;
static bool gR = false;
static uint16_t gButtons = 0;            // keyboard -> digital pad
static PadState gPad = PAD_NEUTRAL;       // gamepad output types
static uint8_t gAnalogPresses = 0;        // counts Analog button presses

// sum with saturation
static int8_t sumSat(int32_t a, int32_t b) {
    int32_t ret = a + b;
    if (ret < -128) ret = -128;
    if (ret > 127) ret = 127;
    return (int8_t) ret;
}

// ---------------------------------------------------------------------------
// Input from HID Remapper (core 0).

static uint8_t active_descriptor() {
    // The descriptor in use since boot. A type changed in the configurator
    // takes effect after reconnecting, same as for USB output.
    return our_descriptor->idx;
}

static EProt prot_for_descriptor(uint8_t idx) {
    switch (idx) {
        case DESC_MOUSE_KEYBOARD:
            return PS1_DEFAULT_MODE;
        case DESC_SWITCH:
        case DESC_PS4:
            return PROT_DUALSHOCK;
        case DESC_STADIA:
            return PROT_NEGCON;
        default:
            return PROT_NONE;
    }
}

static void feed_mouse_keyboard(const uint8_t* r, uint8_t len) {
    if (r[0] == REMAPPER_REPORT_ID_MOUSE && len >= MOUSE_REPORT_MIN_LEN) {
        uint8_t buttons = r[1];
        int32_t x = (int16_t) (r[2] | (r[3] << 8));
        int32_t y = (int16_t) (r[4] | (r[5] << 8));

        critical_section_enter_blocking(&shared_cs);
        gSumX = sumSat(gSumX, x);
        gSumY = sumSat(gSumY, y);
        gL = buttons & 1;
        gR = buttons & 2;
        gContrProt = PROT_MOUSE;
        critical_section_exit(&shared_cs);
    } else if (r[0] == REMAPPER_REPORT_ID_KEYBOARD && len >= KEYBOARD_REPORT_MIN_LEN) {
        uint16_t buttons = 0;
        for (int i = 0; i != 8; ++i) {
            if (r[1] & (1 << i)) {
                buttons |= MOD_PTR[i];
            }
        }
        for (int usage = KEYBOARD_FIRST_USAGE; usage <= KEYBOARD_LAST_USAGE; ++usage) {
            int bit = usage - KEYBOARD_FIRST_USAGE;
            if (r[2 + bit / 8] & (1 << (bit % 8))) {
                buttons |= KEY_PTR[usage];
            }
        }

        critical_section_enter_blocking(&shared_cs);
        gSumX = 0;
        gSumY = 0;
        gButtons = buttons;
        gContrProt = PROT_KEYB;
        critical_section_exit(&shared_cs);
    }
}

static uint16_t map_pad14(uint16_t b) {
    uint16_t out = 0;
    for (int i = 0; i < 14; i++) {
        if (b & (1 << i)) {
            out |= PAD14_MAP[i];
        }
    }
    return out;
}

static void store_pad(const PadState& pad, bool analog_button) {
    static bool prev_analog_button = false;

    critical_section_enter_blocking(&shared_cs);
    gPad = pad;
    if (analog_button && !prev_analog_button) {
        gAnalogPresses++;
    }
    critical_section_exit(&shared_cs);
    prev_analog_button = analog_button;
}

static void feed_switch(const uint8_t* r, uint8_t len) {
    if (r[0] != 0 || len < SWITCH_REPORT_MIN_LEN) {
        return;
    }
    uint16_t b = r[1] | (r[2] << 8);
    PadState pad = PAD_NEUTRAL;
    pad.buttons = map_pad14(b) | hat_to_dpad(r[3] & 0x0F);
    pad.lx = r[4];
    pad.ly = r[5];
    pad.rx = r[6];
    pad.ry = r[7];
    store_pad(pad, b & PAD14_ANALOG_BUTTON);
}

static void feed_ps4(const uint8_t* r, uint8_t len) {
    if (r[0] != REMAPPER_REPORT_ID_PS4 || len < PS4_REPORT_MIN_LEN) {
        return;
    }
    uint16_t b = (r[5] >> 4) | (r[6] << 4) | ((r[7] & 0x03) << 12);
    PadState pad = PAD_NEUTRAL;
    pad.buttons = map_pad14(b) | hat_to_dpad(r[5] & 0x0F);
    pad.lx = r[1];
    pad.ly = r[2];
    pad.rx = r[3];
    pad.ry = r[4];
    store_pad(pad, b & PAD14_ANALOG_BUTTON);
}

static void feed_stadia(const uint8_t* r, uint8_t len) {
    if (r[0] != REMAPPER_REPORT_ID_STADIA || len < STADIA_REPORT_MIN_LEN) {
        return;
    }
    uint16_t b = r[2] | (r[3] << 8);
    PadState pad = PAD_NEUTRAL;
    pad.buttons = hat_to_dpad(r[1] & 0x0F);
    if (b & STADIA_MENU) pad.buttons |= PSB_START;
    if (b & STADIA_R1) pad.buttons |= NEGCON_R;
    if (b & STADIA_A) pad.buttons |= NEGCON_A;
    if (b & STADIA_B) pad.buttons |= NEGCON_B;
#if PS1_NEGCON_INVERT_TWIST
    pad.twist = 0xFF - r[4];
#else
    pad.twist = r[4];
#endif
    pad.i = r[9];   // R2 axis (accelerator)
    pad.ii = r[8];  // L2 axis (brake)
    pad.l = (b & STADIA_L1) ? 0xFF : 0x00;
    store_pad(pad, false);
}

void ps1_port_feed_report(const uint8_t* r, uint8_t len) {
    if (len < 1) {
        return;
    }
    switch (active_descriptor()) {
        case DESC_MOUSE_KEYBOARD:
            feed_mouse_keyboard(r, len);
            break;
        case DESC_SWITCH:
            feed_switch(r, len);
            break;
        case DESC_PS4:
            feed_ps4(r, len);
            break;
        case DESC_STADIA:
            feed_stadia(r, len);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// PS1 protocol state machine (core 1).
//
// Bit-level handling is the original firmware's. What changed:
//   - responses are built per byte (byte_done), so the reply can depend on
//     the command byte (needed for DualShock config commands),
//   - a byte is only ACKed once we know the transaction continues, so we no
//     longer ACK bytes meant for something else (e.g. the memory card),
//   - sleep_us() is busy_wait_us_32() so timing doesn't depend on timer
//     interrupts serviced by the other core.

enum EState : uint8_t { SM_A0 = 0, SM_A1 = 1, SM_A0C1 = 2, SM_A0C0 = 3 };

typedef struct {
    enum EState state;
    uint8_t bitIndex;
    uint8_t byteIndex;
    uint8_t cmd[9];   // command bytes received in this transaction
    uint8_t data[10]; // reply bytes 1.. (data[0] = ID low byte, data[1] = 0x5A)
    uint8_t size;     // reply bytes after the address byte
    EProt prot;       // protocol for this transaction
    bool config;      // DualShock was in config mode at the start of it
} ConSM;

static ConSM gSM;

// DualShock state (core 1 only; read by the LED task).
static volatile bool ds_analog = PS1_DUALSHOCK_START_ANALOG;
static volatile bool ds_config = false;
static bool ds_locked = false;
static uint8_t ds_seen_presses = 0;
static EProt last_prot = PROT_NONE;

static void ds_reset() {
    ds_analog = PS1_DUALSHOCK_START_ANALOG;
    ds_config = false;
    ds_locked = false;
}

static void set_digital(uint8_t id, uint16_t buttons) {
    gSM.size = 4;
    gSM.data[0] = id;
    gSM.data[1] = 0x5A;
    gSM.data[2] = ~buttons;
    gSM.data[3] = ~(buttons >> 8);
}

static void set_analog(uint8_t id, const PadState& pad) {
    gSM.size = 8;
    gSM.data[0] = id;
    gSM.data[1] = 0x5A;
    gSM.data[2] = ~pad.buttons;
    gSM.data[3] = ~(pad.buttons >> 8);
    gSM.data[4] = pad.rx;
    gSM.data[5] = pad.ry;
    gSM.data[6] = pad.lx;
    gSM.data[7] = pad.ly;
}

// Byte 0 (0x01) received: snapshot inputs and prepare a normal poll reply.
static void begin_transaction() {
    critical_section_enter_blocking(&shared_cs);
    EProt prot = gContrProt;
    int8_t sumX = 0, sumY = 0;
    bool l = gL, r = gR;
    if (prot == PROT_MOUSE) {
        sumX = gSumX;
        sumY = gSumY;
        gSumX = 0;
        gSumY = 0;
    }
    uint16_t keyButtons = gButtons;
    PadState pad = gPad;
    uint8_t presses = gAnalogPresses;
    critical_section_exit(&shared_cs);

    if (prot != last_prot) {
        if (prot == PROT_DUALSHOCK) {
            ds_reset();
            ds_seen_presses = presses;
        }
        last_prot = prot;
    }

    gSM.prot = prot;
    gSM.config = false;

    switch (prot) {
        case PROT_MOUSE: {
            uint8_t buttons1 = 3;
            if (l) {
                buttons1 |= 8;
            }
            if (r) {
                buttons1 |= 4;
            }
            gSM.size = 6;
            gSM.data[0] = 0x12;
            gSM.data[1] = 0x5A;
            gSM.data[2] = 0xFF;
            gSM.data[3] = ~buttons1;
            gSM.data[4] = sumX;
            gSM.data[5] = sumY;
            break;
        }
        case PROT_KEYB:
            set_digital(0x41, keyButtons);
            break;
        case PROT_DUALSHOCK:
            if ((uint8_t) (presses - ds_seen_presses) & 1) {
                if (!ds_locked) {
                    ds_analog = !ds_analog;
                }
            }
            ds_seen_presses = presses;
            gSM.config = ds_config;
            if (ds_config) {
                set_analog(0xF3, pad);
            } else if (ds_analog) {
                set_analog(0x73, pad);
            } else {
                set_digital(0x41, pad.buttons);
            }
            break;
        case PROT_NEGCON:
            gSM.size = 8;
            gSM.data[0] = 0x23;
            gSM.data[1] = 0x5A;
            gSM.data[2] = ~pad.buttons;
            gSM.data[3] = ~(pad.buttons >> 8);
            gSM.data[4] = pad.twist;
            gSM.data[5] = pad.i;
            gSM.data[6] = pad.ii;
            gSM.data[7] = pad.l;
            break;
        default:
            set_digital(0x41, 0);
            break;
    }
}

// DualShock config-mode replies (bytes 3..8), per command.
static void ds_config_reply(uint8_t command) {
    uint8_t* d = gSM.data + 2;
    switch (command) {
        case 0x42:  // poll: keep the pad data prepared at byte 0
            return;
        case 0x45: {  // controller type: DualShock, LED state
            const uint8_t reply[6] = { 0x01, 0x02, (uint8_t) (ds_analog ? 0x01 : 0x00), 0x02, 0x01, 0x00 };
            memcpy(d, reply, 6);
            return;
        }
        case 0x46: {  // actuator info, parameter 0 (patched at byte 3 for parameter 1)
            const uint8_t reply[6] = { 0x00, 0x00, 0x01, 0x02, 0x00, 0x0A };
            memcpy(d, reply, 6);
            return;
        }
        case 0x47: {
            const uint8_t reply[6] = { 0x00, 0x00, 0x02, 0x00, 0x01, 0x00 };
            memcpy(d, reply, 6);
            return;
        }
        case 0x4C: {  // mode info, parameter 0 (patched at byte 3 for parameter 1)
            const uint8_t reply[6] = { 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 };
            memcpy(d, reply, 6);
            return;
        }
        case 0x4D:  // rumble mapping: report "unmapped" (no motors here)
            memset(d, 0xFF, 6);
            return;
        default:  // 0x43, 0x44 and anything else
            memset(d, 0x00, 6);
            return;
    }
}

// Command byte (byte 1) received. Returns false to stop answering.
static bool command_received(uint8_t command) {
    if (gSM.prot == PROT_DUALSHOCK) {
        if (gSM.config) {
            ds_config_reply(command);
            return true;
        }
        return command == 0x42 || command == 0x43;  // 0x43 = enter config mode
    }
    return command == 0x42;
}

// First parameter (byte 3) received.
static void param1_received(uint8_t p) {
    if (gSM.prot != PROT_DUALSHOCK) {
        return;
    }
    uint8_t command = gSM.cmd[1];
    if (command == 0x43) {
        ds_config = (p == 0x01);  // enter (1) or exit (0) config mode
    } else if (gSM.config) {
        if (command == 0x44) {
            ds_analog = (p == 0x01);
        } else if (command == 0x46 && p == 0x01) {
            const uint8_t tail[4] = { 0x01, 0x01, 0x01, 0x14 };
            memcpy(gSM.data + 4, tail, 4);
        } else if (command == 0x4C && p == 0x01) {
            gSM.data[5] = 0x07;
        }
    }
}

// Second parameter (byte 4) received.
static void param2_received(uint8_t p) {
    if (gSM.prot == PROT_DUALSHOCK && gSM.config && gSM.cmd[1] == 0x44) {
        ds_locked = (p == 0x03);  // 0x03 locks the mode against the Analog button
    }
}

// A whole byte received. Returns false to stop answering this transaction.
static bool byte_done(uint8_t index) {
    uint8_t c = gSM.cmd[index];
    switch (index) {
        case 0:
            if (c != 0x01) {  // not a controller access (0x81 = memory card)
                return false;
            }
            begin_transaction();
            return true;
        case 1:
            return command_received(c);
        case 3:
            param1_received(c);
            return true;
        case 4:
            param2_received(c);
            return true;
        default:
            return true;
    }
}

static void SM_init() {
    if (gpio_get(GP_ATT)) {
        gSM.state = SM_A1;
    } else {
        gSM.state = SM_A0;
    }
}

static void SM_task() {
    switch (gSM.state) {
        case SM_A0: {
            gpio_set_dir(GP_DAT, GPIO_IN);
            if (gpio_get(GP_ATT)) {
                gSM.state = SM_A1;
            }
            break;
        }
        case SM_A1: {
            gpio_set_dir(GP_DAT, GPIO_IN);
            if (!gpio_get(GP_ATT)) {
                gSM.bitIndex = gSM.byteIndex = 0;
                memset(gSM.cmd, 0, sizeof(gSM.cmd));
                gSM.size = 0;
                gSM.state = SM_A0C1;
            }
            break;
        }
        case SM_A0C1: {
            if (!gpio_get(GP_CLK)) {
                gSM.state = SM_A0C0;
                // falling edge clock
                if (gSM.byteIndex > 0 && gSM.byteIndex <= gSM.size) {
                    if (gSM.data[gSM.byteIndex - 1] & (1 << gSM.bitIndex)) {
                        gpio_set_dir(GP_DAT, GPIO_IN);
                    } else {
                        gpio_set_dir(GP_DAT, GPIO_OUT);
                    }
                }
            } else if (gpio_get(GP_ATT)) {
                gSM.state = SM_A1;
            }
            break;
        }
        case SM_A0C0: {
            if (gpio_get(GP_CLK)) {
                gSM.state = SM_A0C1;
                // rising edge clock
                if (gpio_get(GP_CMD)) {
                    if (gSM.byteIndex < sizeof(gSM.cmd)) {
                        gSM.cmd[gSM.byteIndex] |= 1 << (gSM.bitIndex);
                    }
                }
                ++gSM.bitIndex;
                if (gSM.bitIndex == 8) {
                    gSM.bitIndex = 0;
                    busy_wait_us_32(PS1_DAT_RELEASE_DELAY_US);
                    gpio_set_dir(GP_DAT, GPIO_IN);

                    if (gSM.byteIndex >= sizeof(gSM.cmd) || !byte_done(gSM.byteIndex)) {
                        gSM.state = SM_A0;
                        break;
                    }
                    if (gSM.byteIndex < gSM.size) {
                        gpio_set_dir(GP_ACK, GPIO_OUT);
                        busy_wait_us_32(PS1_ACK_PULSE_US);
                        gpio_set_dir(GP_ACK, GPIO_IN);
                    }
                    ++gSM.byteIndex;
                }
            } else if (gpio_get(GP_ATT)) {
                gSM.state = SM_A1;
            }
            break;
        }
    }
}

static void ps1_core1_main() {
    set_sys_clock_khz(120000, true);
    SM_init();
    while (true) {
        SM_task();
    }
}

// ---------------------------------------------------------------------------
// Status LED (core 0). Mouse/keyboard colours are the original firmware's.

#define COLOR_BLACK 0x000000
#define COLOR_FAINT_MOUSE_GREEN 0x020001
#define COLOR_FAINT_KEYBOARD_VIOLET 0x000202
#define COLOR_FAINT_RED 0x000300
#define COLOR_FAINT_BLUE 0x000003
#define COLOR_FAINT_YELLOW 0x020200
#define COLOR_FAINT_WARM_WHITE 0x020201

static bool led_ok = false;
static PIO led_pio;
static uint led_sm;
static uint64_t next_led_update = 0;
static uint8_t blinkI = 0;

static void led_init() {
#ifdef PS1_WS2812_PIN
    uint offset;
    led_ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ws2812_program, &led_pio, &led_sm, &offset, PS1_WS2812_PIN, 1, true);
    if (led_ok) {
        ws2812_program_init(led_pio, led_sm, offset, PS1_WS2812_PIN, 800000, false);
    }
#endif
}

static bool any_usb_device_mounted() {
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB; addr++) {
        if (tuh_mounted(addr)) {
            return true;
        }
    }
    return false;
}

static void led_task() {
    if (!led_ok) {
        return;
    }
    uint64_t now = time_us_64();
    if (now < next_led_update) {
        return;
    }
    next_led_update = now + 50000;  // 20 updates/s

    uint32_t pixGRB;
    if (!any_usb_device_mounted()) {
        pixGRB = (blinkI < 10) ? COLOR_FAINT_WARM_WHITE : COLOR_BLACK;
    } else {
        critical_section_enter_blocking(&shared_cs);
        EProt prot = gContrProt;
        bool pressed;
        switch (prot) {
            case PROT_KEYB:
                pressed = gButtons & 8;
                break;
            case PROT_MOUSE:
                pressed = gL;
                break;
            default:
                pressed = gPad.buttons != 0;
                break;
        }
        critical_section_exit(&shared_cs);

        if (pressed) {
            pixGRB = COLOR_FAINT_WARM_WHITE;
        } else {
            switch (prot) {
                case PROT_KEYB:
                    pixGRB = COLOR_FAINT_KEYBOARD_VIOLET;
                    break;
                case PROT_DUALSHOCK:  // red like the real Analog LED, blue when digital
                    pixGRB = ds_analog ? COLOR_FAINT_RED : COLOR_FAINT_BLUE;
                    break;
                case PROT_NEGCON:
                    pixGRB = COLOR_FAINT_YELLOW;
                    break;
                case PROT_MOUSE:
                    pixGRB = COLOR_FAINT_MOUSE_GREEN;
                    break;
                default:
                    pixGRB = COLOR_BLACK;
                    break;
            }
        }
    }
    if (++blinkI == 20) {
        blinkI = 0;
    }
    pio_sm_put(led_pio, led_sm, pixGRB << 8u);
}

// ---------------------------------------------------------------------------

void ps1_port_task() {
    led_task();
}

void ps1_port_init() {
    critical_section_init(&shared_cs);
    gContrProt = prot_for_descriptor(active_descriptor());
    gPad = PAD_NEUTRAL;

    gpio_init(GP_ATT);
    gpio_set_dir(GP_ATT, GPIO_IN);

    gpio_init(GP_CLK);
    gpio_set_dir(GP_CLK, GPIO_IN);

    gpio_init(GP_DAT);
    gpio_set_slew_rate(GP_DAT, GPIO_SLEW_RATE_SLOW);
    gpio_set_dir(GP_DAT, GPIO_IN);
    gpio_clr_mask((1 << GP_DAT));

    gpio_init(GP_CMD);
    gpio_set_dir(GP_CMD, GPIO_IN);

    gpio_init(GP_ACK);
    gpio_set_slew_rate(GP_ACK, GPIO_SLEW_RATE_SLOW);
    gpio_set_dir(GP_ACK, GPIO_IN);
    gpio_clr_mask((1 << GP_ACK));

    led_init();

    multicore_reset_core1();
    multicore_launch_core1(ps1_core1_main);
}
