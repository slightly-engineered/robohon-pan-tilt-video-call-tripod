#pragma once

// Wi-Fi credentials live in secrets.h (git-ignored). Copy secrets.example.h to secrets.h.
#include "secrets.h"

// WS2812 LED settings
#define STATUS_LED_PIN   10
#define STATUS_LED_COUNT 1

// ============================================================
// Pan/tilt on two RoBoHoN actuators (replaces the old PWM servo pins)
// ============================================================
// Both buses are TX-only, 30-byte 0x77 0x80 frames.
// Board: ESP32-S3 (3 hardware UARTs: 0, 1, 2). Pins 4 and 6 are plain GPIO4/GPIO6.
// If any axis uses UART0: Tools > USB CDC On Boot > Enabled
// (keeps Serial on USB so UART0 is free).

// PAN  = LEG bus,  actuator wired as SID 2 (L. Foot)  <- joystick X
// SID 0 = ID-discovery test: the setpoint is written into ALL FIVE slots, so a lone
// actuator moves whatever its ID is. If SID 0 moves it but SID 2 doesn't, the
// actuator's ID isn't 2; then try 1, 3, 4, 5 to find it.
#define PAN_TX_PIN       4
#define PAN_SID          0

// TILT = HEAD bus, actuator wired as SID 2 (Head Nod) <- joystick Y
#define TILT_TX_PIN      6
#define TILT_SID         2

// Which hardware UART drives each axis (0, 1 or 2 on ESP32-S3).
// TILT on UART0 is confirmed working. Change PAN_UART_NUM to isolate a UART problem.
#define PAN_UART_NUM     2
#define TILT_UART_NUM    0

// 1 = print a status line every 500 ms while a stick is deflected:
//     state, stick command, setpoint, bytes written per bus
#define BUS_DEBUG        1

// true = joystick X drives TILT and Y drives PAN
#define SWAP_AXES        false

// The served web page sends X = -1 for stick RIGHT and Y = -1 for stick UP.
// These flags undo that, so inside the firmware +X = right and +Y = up
// (pan right / tilt up = +counts before PAN_REVERSED / TILT_REVERSED).
#define JOY_INVERT_X     true
#define JOY_INVERT_Y     true

// Flip if an axis physically moves the wrong way (empirical).
#define PAN_REVERSED     true
#define TILT_REVERSED    false

#define BUS_BAUD         115200
#define BUS_SERIAL_CFG   SERIAL_8N1     // match your RoBoHoN firmware if it differs

// Frame refresh rate to each bus while engaged
#define FRAME_PERIOD_MS  20

// ============================================================
// Travel (counts). Bench sweep: ~64000 counts ~= 330 deg -> ~190 counts/deg
// ============================================================
// Center = captured home value of the driven SID + trim.
// Travel is +/- RANGE around center. Tune RANGE to the mechanical limits.
#define PAN_RANGE        15000          // ~ +/-77 deg
#define TILT_RANGE        8000          // ~ +/-41 deg
#define PAN_CENTER_TRIM      0
#define TILT_CENTER_TRIM     0

// ============================================================
// Joystick mapping
// ============================================================
//   DRIVE_MODE_VELOCITY : stick = slew speed, position HELD on release
//   DRIVE_MODE_POSITION : stick = offset from center, returns to center on release
#define DRIVE_MODE_VELOCITY 1
#define DRIVE_MODE_POSITION 2
#define DRIVE_MODE          DRIVE_MODE_VELOCITY

#define MAX_COUNTS_PER_SEC  8000        // full-stick slew, VELOCITY mode (~40 deg/s)

#define JOY_DEADBAND        0.05f
#define JOY_TIMEOUT_MS      400         // no JOY message for this long -> stick treated as 0

// ============================================================
// Idle behavior (also applies after a WebSocket disconnect)
// ============================================================
// 1) No setpoint change for RELAX_IDLE_MS      -> relax (torque off)
// 2) No stick touch for HOME_IDLE_MS           -> re-engage, go to center,
//    wait HOME_SETTLE_MS for the actuators to arrive, then relax again
//    (skipped if both axes are already at center)
// Any stick input that would move an axis re-engages it immediately.
#define RELAX_IDLE_MS       2500
#define HOME_IDLE_MS        5000
#define HOME_SETTLE_MS      1500        // tune: time for the slowest axis to reach center

// Relax sequence (your two-stage behavior): SID1-5 first, then SID6
#define RELAX_STAGE_MS      150         // per stage
// While relaxed, keep sending full-relax frames at this period (0 = go silent)
#define RELAXED_FRAME_PERIOD_MS 100
