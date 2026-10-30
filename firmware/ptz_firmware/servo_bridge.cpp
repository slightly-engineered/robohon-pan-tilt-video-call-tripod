#include "servo_bridge.h"
#include "wifi_ws.h"
#include "config.h"
#include <Arduino.h>
#include <HardwareSerial.h>

// UART0 is only free if Serial is on USB CDC.
#if (PAN_UART_NUM == 0 || TILT_UART_NUM == 0) && !ARDUINO_USB_CDC_ON_BOOT
#error "Enable Tools > USB CDC On Boot so UART0 (Serial0) is free"
#endif
#if PAN_UART_NUM == TILT_UART_NUM
#error "PAN_UART_NUM and TILT_UART_NUM must be different"
#endif

// ============================================================
// Sharp/RoBoHoN servo frame: 30 bytes
//   77 80 00 03 06 00 | 5 x [pos_lo pos_hi flag sid] | trailer(3) | checksum
//   checksum = (256 - sum(bytes 6..28)) & 0xFF
// ============================================================
#define FRAME_LEN   30
#define SLOTS       5
#define FLAG_POS    0x40
#define FLAG_RELAX  0xFF

enum FrameKind : uint8_t {
    FRAME_POSITION,       // driven SID = setpoint, other SIDs = home, trailer = home
    FRAME_RELAX_STAGE1,   // SID1-5 relaxed, trailer (SID6) held at home
    FRAME_RELAX_FULL      // SID1-5 and SID6 relaxed
};

struct SharpBus {
    const char*     name;
    HardwareSerial* uart;
    int8_t          txPin;
    uint8_t         driveSid;      // 1..5, or 0 = drive ALL five slots (ID-discovery test)
    int16_t         home[SLOTS];   // captured home positions for SID1..5
    int16_t         trim;          // center trim (counts)
    int16_t         range;         // +/- travel around center (counts)
    float           pos;           // current commanded setpoint (counts)
    uint8_t         uartNum;       // 0, 1 or 2
    uint32_t        bytesWritten;  // diagnostics: bytes accepted by the UART driver
    uint32_t        framesSent;
};

// Home positions from the logic-analyzer captures
static SharpBus g_pan  = { "PAN/leg",   nullptr, PAN_TX_PIN,  PAN_SID,
                           { 1439, -2330,  -36, 1565, -1256 }, PAN_CENTER_TRIM,  PAN_RANGE,  0,
                           PAN_UART_NUM, 0, 0 };
static SharpBus g_tilt = { "TILT/head", nullptr, TILT_TX_PIN, TILT_SID,
                           {  239, -2530, 2270, 1565, -1256 }, TILT_CENTER_TRIM, TILT_RANGE, 0,
                           TILT_UART_NUM, 0, 0 };

// ============================================================
// State (Enable/Disable/Drive run on the AsyncTCP task,
// UART work only ever happens in servoBridgePoll on the loop task)
// ============================================================
enum BridgeState : uint8_t { ST_OFF, ST_ACTIVE, ST_HOMING, ST_RELAX1, ST_RELAX2, ST_RELAXED };

static volatile bool     g_wantActive  = false;
static volatile float    g_cmdPan      = 0;      // -1..1
static volatile float    g_cmdTilt     = 0;
static volatile uint32_t g_lastCmdMs   = 0;      // last JOY message
static volatile uint32_t g_lastTouchMs = 0;      // last JOY message with stick outside deadband

static BridgeState g_state         = ST_OFF;
static bool        g_wasActive     = false;
static uint32_t    g_stateMs       = 0;
static uint32_t    g_lastFrameMs   = 0;
static uint32_t    g_lastMotionMs  = 0;          // last time a setpoint changed
static uint32_t    g_lastRelaxTxMs = 0;
static bool        g_uartsStarted  = false;

// ============================================================
// Helpers
// ============================================================
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float centerOf(const SharpBus& b) {
    // SID 0 (all-slots test) centers on slot 2's home value
    uint8_t idx = b.driveSid ? b.driveSid - 1 : 1;
    return clampf((float)b.home[idx] + b.trim, -32768, 32767);
}

// ============================================================
// Frame building
// ============================================================
static uint8_t calcChecksum(const uint8_t* f) {
    uint16_t sum = 0;
    for (int i = 6; i <= 28; i++) sum += f[i];
    return (uint8_t)(256 - sum);
}

static void buildFrame(const SharpBus& b, FrameKind kind, int16_t drivePos, uint8_t* f) {
    f[0] = 0x77; f[1] = 0x80; f[2] = 0x00; f[3] = 0x03; f[4] = 0x06; f[5] = 0x00;

    int idx = 6;
    for (uint8_t sid = 1; sid <= SLOTS; sid++) {
        if (kind == FRAME_POSITION) {
            int16_t p = (b.driveSid == 0 || sid == b.driveSid) ? drivePos : b.home[sid - 1];
            f[idx++] = (uint8_t)(p & 0xFF);
            f[idx++] = (uint8_t)(((uint16_t)p >> 8) & 0xFF);
            f[idx++] = FLAG_POS;
            f[idx++] = sid;
        } else {
            f[idx++] = 0x00;
            f[idx++] = 0x00;
            f[idx++] = FLAG_RELAX;
            f[idx++] = sid;
        }
    }

    // Trailer = SID6 channel (positional, no ID byte)
    if (kind == FRAME_RELAX_FULL) {
        f[26] = 0x00; f[27] = 0x00; f[28] = FLAG_RELAX;
    } else {
        f[26] = 0x6D; f[27] = 0x00; f[28] = FLAG_POS;   // home (109)
    }

    f[29] = calcChecksum(f);
}

static void sendFrame(SharpBus& b, FrameKind kind, int16_t drivePos) {
    uint8_t f[FRAME_LEN];
    buildFrame(b, kind, drivePos, f);
    size_t n = b.uart->write(f, FRAME_LEN);
    b.bytesWritten += n;
    b.framesSent++;
}

static void printCenterFrame(SharpBus& b) {
    uint8_t f[FRAME_LEN];
    buildFrame(b, FRAME_POSITION, (int16_t)lroundf(centerOf(b)), f);
    Serial.printf("[BUS] %s center frame: ", b.name);
    for (int i = 0; i < FRAME_LEN; i++) Serial.printf("%02X ", f[i]);
    Serial.println();
}

static HardwareSerial* uartByNum(uint8_t n) {
    switch (n) {
        case 0: return &Serial0;
        case 1: return &Serial1;
#if (defined(SOC_UART_HP_NUM) && SOC_UART_HP_NUM > 2) || (defined(SOC_UART_NUM) && SOC_UART_NUM > 2)
        case 2: return &Serial2;
#endif
        default: return nullptr;
    }
}

static bool startUarts() {
    if (g_uartsStarted) return true;

    g_pan.uart  = uartByNum(g_pan.uartNum);
    g_tilt.uart = uartByNum(g_tilt.uartNum);
    if (!g_pan.uart || !g_tilt.uart) {
        Serial.printf("[BUS] ERROR: UART%d or UART%d not available on this chip\n",
                      g_pan.uartNum, g_tilt.uartNum);
        return false;
    }

    // TX only (rx = -1)
    g_pan.uart->begin(BUS_BAUD, BUS_SERIAL_CFG, -1, g_pan.txPin);
    g_tilt.uart->begin(BUS_BAUD, BUS_SERIAL_CFG, -1, g_tilt.txPin);

    g_uartsStarted = true;
    Serial.printf("[BUS] PAN=UART%d TX GPIO%d, TILT=UART%d TX GPIO%d, %d baud\n",
                  g_pan.uartNum, g_pan.txPin, g_tilt.uartNum, g_tilt.txPin, BUS_BAUD);
    return true;
}

static const char* stateName(BridgeState st) {
    switch (st) {
        case ST_OFF:     return "OFF";
        case ST_ACTIVE:  return "ACTIVE";
        case ST_HOMING:  return "HOMING";
        case ST_RELAX1:  return "RELAX stage 1";
        case ST_RELAX2:  return "RELAX stage 2";
        case ST_RELAXED: return "RELAXED";
    }
    return "?";
}

static void setState(BridgeState st, uint32_t now) {
    if (st == g_state) return;
    g_state   = st;
    g_stateMs = now;
    Serial.printf("[BUS] -> %s\n", stateName(st));
}

static bool atCenter(const SharpBus& b) {
    return fabsf(b.pos - centerOf(b)) < 0.5f;
}

static void enterActive(uint32_t now) {
    if (!startUarts()) return;      // stays OFF; retried on the next connect

    g_pan.pos  = centerOf(g_pan);
    g_tilt.pos = centerOf(g_tilt);
    g_cmdPan  = 0;
    g_cmdTilt = 0;
    g_lastCmdMs    = now;
    g_lastTouchMs  = now;
    g_lastMotionMs = now;
    g_lastFrameMs  = now - FRAME_PERIOD_MS;   // send first frame immediately

    printCenterFrame(g_pan);
    printCenterFrame(g_tilt);
    g_state = ST_OFF;                          // force the transition log
    setState(ST_ACTIVE, now);
}

// ============================================================
// Enable / Disable
// ============================================================
void servoBridgeEnable() {
    g_wantActive = true;
    Serial.println("[BUS] Bridge ENABLED");
}

void servoBridgeDisable() {
    g_wantActive = false;
    g_cmdPan  = 0;
    g_cmdTilt = 0;
    Serial.println("[BUS] Bridge DISABLED (idle timers take over: relax, home, relax)");
}

bool servoBridgeActive() {
    return g_wantActive;
}

// ============================================================
// Joystick -> pan/tilt command (no differential mix)
// ============================================================
void servoBridgeDrive(float x, float y) {
    if (!g_wantActive) return;

    x = clampf(x, -1, 1);
    y = clampf(y, -1, 1);

    if (fabsf(x) < JOY_DEADBAND) x = 0;
    if (fabsf(y) < JOY_DEADBAND) y = 0;

    // Page sends right = -X, up = -Y; normalize to right = +, up = +
    if (JOY_INVERT_X) x = 0.0f - x;
    if (JOY_INVERT_Y) y = 0.0f - y;

    float pan  = SWAP_AXES ? y : x;
    float tilt = SWAP_AXES ? x : y;

    if (PAN_REVERSED)  pan  = 0.0f - pan;
    if (TILT_REVERSED) tilt = 0.0f - tilt;

    uint32_t now = millis();
    g_cmdPan  = pan;
    g_cmdTilt = tilt;
    g_lastCmdMs = now;
    if (x != 0 || y != 0) g_lastTouchMs = now;
}

// ============================================================
// Poll: session edge, idle timers, setpoint update, frame output
// ============================================================
static float nextSetpoint(const SharpBus& b, float cmd, float dt) {
    float c  = centerOf(b);
    float lo = clampf(c - b.range, -32768, 32767);
    float hi = clampf(c + b.range, -32768, 32767);
#if DRIVE_MODE == DRIVE_MODE_VELOCITY
    return clampf(b.pos + cmd * MAX_COUNTS_PER_SEC * dt, lo, hi);
#else
    return clampf(c + cmd * b.range, lo, hi);
#endif
}

static void sendPosition() {
    sendFrame(g_pan,  FRAME_POSITION, (int16_t)lroundf(g_pan.pos));
    sendFrame(g_tilt, FRAME_POSITION, (int16_t)lroundf(g_tilt.pos));
}

static void sendRelax(FrameKind k) {
    sendFrame(g_pan,  k, 0);
    sendFrame(g_tilt, k, 0);
}

void servoBridgePoll() {
    uint32_t now = millis();

    // New WebSocket session: engage at center
    if (g_wantActive && !g_wasActive) enterActive(now);
    g_wasActive = g_wantActive;

    if (g_state == ST_OFF) return;
    if ((uint32_t)(now - g_lastFrameMs) < FRAME_PERIOD_MS) return;

    float dt = (now - g_lastFrameMs) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_lastFrameMs = now;

    // ---- What would the stick do this tick?
    bool stale = (uint32_t)(now - g_lastCmdMs) > JOY_TIMEOUT_MS;
    float np = nextSetpoint(g_pan,  stale ? 0 : g_cmdPan,  dt);
    float nt = nextSetpoint(g_tilt, stale ? 0 : g_cmdTilt, dt);
    bool moved = fabsf(np - g_pan.pos) >= 0.5f || fabsf(nt - g_tilt.pos) >= 0.5f;

    // ---- Motion wakes/keeps the actuators engaged
    if (moved) {
        g_pan.pos  = np;
        g_tilt.pos = nt;
        g_lastMotionMs = now;
        setState(ST_ACTIVE, now);
    }

    // ---- Idle timers
    uint32_t idleMotion = (uint32_t)(now - g_lastMotionMs);
    uint32_t idleTouch  = (uint32_t)(now - g_lastTouchMs);
    bool needHome = idleTouch >= HOME_IDLE_MS && !(atCenter(g_pan) && atCenter(g_tilt));

    if (g_state != ST_HOMING && needHome) {
        g_pan.pos  = centerOf(g_pan);
        g_tilt.pos = centerOf(g_tilt);
        setState(ST_HOMING, now);
    } else if (g_state == ST_ACTIVE && idleMotion > RELAX_IDLE_MS) {
        setState(ST_RELAX1, now);
    } else if (g_state == ST_HOMING && (uint32_t)(now - g_stateMs) >= HOME_SETTLE_MS) {
        setState(ST_RELAX1, now);
    }

    // ---- Output
    switch (g_state) {
    case ST_ACTIVE:
    case ST_HOMING:
        sendPosition();
        break;
    case ST_RELAX1:
        sendRelax(FRAME_RELAX_STAGE1);
        if ((uint32_t)(now - g_stateMs) >= RELAX_STAGE_MS) setState(ST_RELAX2, now);
        break;
    case ST_RELAX2:
        sendRelax(FRAME_RELAX_FULL);
        g_lastRelaxTxMs = now;
        if ((uint32_t)(now - g_stateMs) >= RELAX_STAGE_MS) setState(ST_RELAXED, now);
        break;
    case ST_RELAXED:
        if (RELAXED_FRAME_PERIOD_MS > 0 &&
            (uint32_t)(now - g_lastRelaxTxMs) >= RELAXED_FRAME_PERIOD_MS) {
            sendRelax(FRAME_RELAX_FULL);
            g_lastRelaxTxMs = now;
        }
        break;
    default:
        break;
    }

#if BUS_DEBUG
    static uint32_t lastDbgMs = 0;
    if ((g_cmdPan != 0 || g_cmdTilt != 0) && !stale && (uint32_t)(now - lastDbgMs) >= 500) {
        lastDbgMs = now;
        Serial.printf("[BUS] %s cmd P=%+.2f T=%+.2f pos P=%ld T=%ld | UART%d(pan) wrote %lu B/%lu fr, UART%d(tilt) wrote %lu B/%lu fr\n",
                      stateName(g_state), (double)g_cmdPan, (double)g_cmdTilt,
                      lroundf(g_pan.pos), lroundf(g_tilt.pos),
                      g_pan.uartNum,  (unsigned long)g_pan.bytesWritten,  (unsigned long)g_pan.framesSent,
                      g_tilt.uartNum, (unsigned long)g_tilt.bytesWritten, (unsigned long)g_tilt.framesSent);
    }
#endif
}
