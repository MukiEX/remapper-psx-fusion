// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// Board: usb-to-ps1-mouse-pro PCB (Waveshare RP2040-Zero), running
// HID Remapper on core 0 and the PS1 controller-port code on core 1.

#ifndef _BOARDS_USB_TO_PS1_MOUSE_PRO_H
#define _BOARDS_USB_TO_PS1_MOUSE_PRO_H

#define USB_TO_PS1_MOUSE_PRO_BOARD

// USB host port (PIO-USB): D+ = GP2, D- = GP3, as on the original PCB.
#define PICO_DEFAULT_PIO_USB_DP_PIN 2

// PlayStation controller port, as on the original PCB.
#define PS1_GP_ATT 7
#define PS1_GP_CLK 10
#define PS1_GP_DAT 11
#define PS1_GP_CMD 14
#define PS1_GP_ACK 15

// Status LED (WS2812 on the RP2040-Zero).
#define PS1_WS2812_PIN 16

// HID Remapper's GPIO input/output feature is disabled on this board:
// the free pins aren't broken out on the PCB, and keeping the remapper
// away from the PS1 and USB pins is what matters. If you add buttons,
// set bits here for those pins only (never 2, 3, 7, 10, 11, 14, 15, 16).
#define GPIO_VALID_PINS_BASE 0

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

#endif
