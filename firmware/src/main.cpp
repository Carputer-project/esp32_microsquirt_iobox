#include <Arduino.h>
#include <driver/twai.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>

static constexpr uint8_t PIN_CAN_TX = 5;
static constexpr uint8_t PIN_CAN_RX = 4;

static constexpr uint8_t PIN_IAC = 32;
static constexpr uint8_t PIN_O1  = 25;
static constexpr uint8_t PIN_O2  = 26;
static constexpr uint8_t PIN_O3  = 27;
static constexpr uint8_t PIN_O4  = 14;
static constexpr uint8_t PIN_O5  = 13;
static constexpr uint8_t PIN_O6  = 23;
static constexpr uint8_t PIN_LED = 2;

static constexpr uint8_t PIN_A1 = 36;
static constexpr uint8_t PIN_A2 = 39;
static constexpr uint8_t PIN_A3 = 34;
static constexpr uint8_t PIN_A4 = 35;

static constexpr uint8_t PIN_TFT_SCLK = 15;
static constexpr uint8_t PIN_TFT_MOSI = 21;
static constexpr uint8_t PIN_TFT_CS   = 22;
static constexpr uint8_t PIN_TFT_DC   = 19;

static constexpr uint32_t CAN_BASE_ID = 0x5F0;
static constexpr uint8_t  CAN_GROUP_COUNT = 9;
static constexpr uint32_t CAN_CMD_ID = 0x600;
static constexpr uint32_t CAN_DASH_ID = 0x710;
static constexpr uint16_t DASH_TX_MS = 100;
static constexpr uint32_t FAILSAFE_MS = 500;
static constexpr float    ADC_DIVIDER = 1.5f;

static constexpr uint8_t  MSG_CMD    = 0;
static constexpr uint8_t  MSG_REQ    = 1;
static constexpr uint8_t  MSG_RSP    = 2;
static constexpr uint8_t  MSG_BURN   = 4;
static constexpr uint8_t  MSG_REQX   = 12;
static constexpr uint8_t  MSG_BURNACK = 14;
static constexpr uint8_t  MSG_PROT   = 0x80;
static constexpr uint8_t  MSG_SPND   = 0x82;

static constexpr uint16_t RESP_TABLE_SIZE   = 256;
static constexpr uint16_t RESP_PORT_OFFSET  = 75;
static constexpr uint8_t  RESP_PORT_FAN_BIT = 0x40;

static Adafruit_GC9A01A s_tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_MOSI, PIN_TFT_SCLK, -1);

enum OutMode : uint8_t { OM_OFF = 0, OM_MAN = 1, OM_TEMP = 2, OM_RPM = 3 };

enum WarnBit : uint16_t {
    W_IDLE_LO = 1 << 0,
    W_IDLE_HI = 1 << 1,
    W_OVERREV = 1 << 2,
    W_OVERHEAT = 1 << 3,
    W_HOTAIR = 1 << 4,
    W_LOWBATT = 1 << 5,
    W_HIBATT = 1 << 6,
    W_OVERBOOST = 1 << 7,
    W_LEAN = 1 << 8,
    W_RICH = 1 << 9,
};

struct EngineProfile {
    bool    enabled = true;
    int16_t idleRpmMin = 700;
    int16_t idleRpmMax = 1100;
    int16_t maxRpm = 8000;
    int16_t cltMax = 2300;
    int16_t matMax = 1600;
    int16_t battMin = 110;
    int16_t battMax = 160;
    int16_t mapMax = 2800;
    int16_t afrLow = 100;    // below this = RICH
    int16_t afrHigh = 165;   // above this = LEAN
    uint16_t warnHoldMs = 3000;
    uint8_t  warnOut = 0;
};

static constexpr uint16_t CFG_MAGIC = 0x495A;

struct Cfg {
    uint16_t magic = CFG_MAGIC;
    int16_t fanOnTemp = 2000;
    int16_t fanOffTemp = 1900;
    bool    fanAuto = true;
    bool    fanManual = false;
    int16_t iacTargetRpm = 900;
    uint8_t iacFailDuty = 0;
    bool    iacAuto = true;
    bool    iacFollow = false;
    uint8_t iacManualDuty = 30;
    int16_t shiftRpm = 7000;
    uint8_t outMode[6] = { OM_RPM, OM_OFF, OM_OFF, OM_OFF, OM_OFF, OM_OFF };
    int16_t outTemp[6] = { 0, 0, 0, 0, 0, 0 };
    int16_t outRpm[6]  = { 7000, 0, 0, 0, 0, 0 };
    bool    outManual[6] = { false, false, false, false, false, false };
    bool    anEnable[4] = { false, false, false, false };
    uint16_t anThresh[4] = { 0, 0, 0, 0 };
    uint8_t anOut[4] = { 0, 0, 0, 0 };
    uint8_t fanOut = 6;
    bool    respEnable = false;
    uint8_t respId = 5;
    EngineProfile eng;
};

static constexpr uint8_t kIacTempF[8] = { 50, 80, 100, 120, 140, 160, 180, 200 };
static constexpr uint8_t kIacDuty[8]  = { 60, 55, 50, 45, 40, 35, 30, 28 };

static Cfg        g_cfg;
static Preferences g_prefs;
static const char* const kPrefsName = "iobox";

static uint8_t  s_outpc[72];
static bool     s_groupSeen[9] = { false, false, false, false, false, false, false, false, false };
static uint32_t s_lastFrameMs = 0;
static bool     s_canFresh = false;
static bool     s_anyGroupSeen = false;

static uint8_t  s_respTable[RESP_TABLE_SIZE];
static uint8_t  s_respPort[3] = { 0, 0, 0 };
static uint16_t s_respPortBase = RESP_PORT_OFFSET;
static bool     s_respPortBaseSet = false;
static bool     s_respPortsFresh = false;
static uint32_t s_respLastFrameMs = 0;

static constexpr uint8_t OUT_PINS[6] = { PIN_O1, PIN_O2, PIN_O3, PIN_O4, PIN_O5, PIN_O6 };
static constexpr uint8_t ADC_PINS[4] = { PIN_A1, PIN_A2, PIN_A3, PIN_A4 };

static uint16_t rdU16(uint8_t off) { return (uint16_t)((s_outpc[off] << 8) | s_outpc[off + 1]); }
static int16_t  rdS16(uint8_t off) { return (int16_t)rdU16(off); }

static uint32_t g_rpm = 0;
static int16_t  g_map = 0, g_mat = 0, g_clt = 0, g_tps = 0, g_batt = 0, g_afr = 0;
static int16_t  g_iacStep = 0;

static uint8_t interpolateIac(int16_t cltF) {
    if (cltF <= kIacTempF[0]) return kIacDuty[0];
    for (uint8_t i = 1; i < 8; i++) {
        if (cltF <= kIacTempF[i]) {
            int16_t x0 = kIacTempF[i - 1], x1 = kIacTempF[i];
            int16_t y0 = kIacDuty[i - 1], y1 = kIacDuty[i];
            return (uint8_t)(y0 + (y1 - y0) * (cltF - x0) / (x1 - x0));
        }
    }
    return kIacDuty[7];
}

static void setFan(bool on) {
    if (g_cfg.fanOut >= 1 && g_cfg.fanOut <= 6) digitalWrite(OUT_PINS[g_cfg.fanOut - 1], on ? HIGH : LOW);
}
static void setOut(uint8_t i, bool on) { digitalWrite(OUT_PINS[i], on ? HIGH : LOW); }
static void setIac(uint8_t duty) { ledcWrite(0, (uint32_t)duty * 1023 / 100); }

static uint16_t readAnalogMv(uint8_t i) {
    return (uint16_t)(analogRead(ADC_PINS[i]) * 3300.0f / 4095.0f * ADC_DIVIDER);
}

static const char* tgtName(uint8_t t) {
    static char buf[8];
    if (t == 0) return "-";
    if (t == 7) return "fan";
    snprintf(buf, sizeof buf, "O%u", t);
    return buf;
}

static bool s_anLatch[4] = { false, false, false, false };

static void updateAnalogLatch() {
    for (uint8_t i = 0; i < 4; i++) {
        if (!g_cfg.anEnable[i]) { s_anLatch[i] = false; continue; }
        int16_t t = (int16_t)g_cfg.anThresh[i];
        int16_t mv = (int16_t)readAnalogMv(i);
        int16_t lo = t > 150 ? t - 150 : 0;
        if (!s_anLatch[i] && mv >= t) s_anLatch[i] = true;
        else if (s_anLatch[i] && mv <= lo) s_anLatch[i] = false;
    }
}

static bool inputForces(uint8_t target) {
    if (target == 0) return false;
    for (uint8_t i = 0; i < 4; i++) {
        if (g_cfg.anOut[i] == target && g_cfg.anEnable[i] && s_anLatch[i]) return true;
    }
    return false;
}

static void outputsOff() {
    setFan(false);
    for (uint8_t i = 0; i < 6; i++) setOut(i, false);
}

static void handleCommand(const String& line);

struct RespHeader {
    uint16_t offset;
    uint8_t  type;
    uint8_t  from;
    uint8_t  to;
    uint8_t  table;
};

static uint32_t respMakeId(uint16_t offset, uint8_t type, uint8_t from, uint8_t to, uint8_t table) {
    return ((uint32_t)(offset & 0x7FF) << 18)
         | ((uint32_t)(type & 0x07)   << 15)
         | ((uint32_t)(from & 0x0F)   << 11)
         | ((uint32_t)(to & 0x0F)     << 7)
         | ((uint32_t)((table & 0x0F) << 3))
         | ((uint32_t)((table & 0x10) >> 2));
}

static RespHeader respParseHeader(uint32_t id) {
    RespHeader h;
    h.offset = (uint16_t)((id >> 18) & 0x7FF);
    h.type   = (uint8_t)((id >> 15) & 0x07);
    h.from   = (uint8_t)((id >> 11) & 0x0F);
    h.to     = (uint8_t)((id >> 7)  & 0x0F);
    h.table  = (uint8_t)(((id >> 3) & 0x0F) | (((id >> 2) & 0x01) << 4));
    return h;
}

static uint16_t readAnalogMv(uint8_t i);

static void respSendReply(const RespHeader& req, uint16_t remOff, uint8_t remTable, uint8_t remBy) {
    if (remBy == 0 || remBy > 8) return;
    if ((uint16_t)(req.offset + remBy) > RESP_TABLE_SIZE) return;
    twai_message_t r = {};
    r.identifier = respMakeId(remOff, MSG_RSP, g_cfg.respId, req.from, remTable);
    r.extd = 1;
    r.data_length_code = remBy;
    for (uint8_t k = 0; k < remBy; k++) r.data[k] = s_respTable[req.offset + k];
    twai_transmit(&r, 0);
}

static void respPopulateAdc() {
    if (!g_cfg.respEnable) return;
    for (uint8_t i = 0; i < 4; i++) {
        uint16_t v = readAnalogMv(i);
        s_respTable[i * 2]     = (uint8_t)(v >> 8);
        s_respTable[i * 2 + 1] = (uint8_t)(v & 0xFF);
    }
}

static void readCan() {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        if (msg.extd) {
            if (!g_cfg.respEnable) continue;
            RespHeader h = respParseHeader(msg.identifier);
            if (h.to != g_cfg.respId && h.to != 15) continue;
            switch (h.type) {
                case MSG_REQ: {
                    if (msg.data_length_code < 3) break;
                    uint8_t remTable = msg.data[0];
                    uint16_t remOff  = ((uint16_t)msg.data[1] << 3) | ((msg.data[2] & 0xE0) >> 5);
                    uint8_t  remBy   = msg.data[2] & 0x1F;
                    respSendReply(h, remOff, remTable, remBy);
                    break;
                }
                case MSG_CMD: {
                    uint8_t dlc = msg.data_length_code > 8 ? 8 : msg.data_length_code;
                    if ((uint16_t)(h.offset + dlc) <= RESP_TABLE_SIZE) {
                        memcpy(&s_respTable[h.offset], msg.data, dlc);
                    }
                    if (!s_respPortBaseSet && dlc >= 1 && dlc <= 3) {
                        s_respPortBase = h.offset;
                        s_respPortBaseSet = true;
                    }
                    if (dlc <= 3 && h.offset == s_respPortBase) {
                        memcpy(s_respPort, msg.data, dlc);
                        if (dlc < 3) memset(&s_respPort[dlc], 0, 3 - dlc);
                        s_respPortsFresh = true;
                        s_respLastFrameMs = millis();
                    }
                    break;
                }
                default:
                    break;
            }
            continue;
        }
        if (msg.identifier == CAN_CMD_ID) {
            String line;
            uint8_t n = msg.data_length_code > 8 ? 8 : msg.data_length_code;
            for (uint8_t i = 0; i < n; i++) {
                char ch = (char)msg.data[i];
                if (ch == '\0' || ch == '\n' || ch == '\r') break;
                line += ch;
            }
            handleCommand(line);
            continue;
        }
        if (msg.identifier < CAN_BASE_ID ||
            msg.identifier >= CAN_BASE_ID + CAN_GROUP_COUNT) continue;
        uint8_t grp = (uint8_t)(msg.identifier - CAN_BASE_ID);
        uint8_t dlc = msg.data_length_code > 8 ? 8 : msg.data_length_code;
        memcpy(&s_outpc[grp * 8], msg.data, dlc);
        if (dlc < 8) memset(&s_outpc[grp * 8 + dlc], 0, 8 - dlc);
        s_groupSeen[grp] = true;
        s_anyGroupSeen = true;
        s_lastFrameMs = millis();
    }
}

static void decodeOutpc() {
    if (s_groupSeen[0]) g_rpm  = rdU16(6);
    if (s_groupSeen[2]) {
        g_map  = rdS16(18);
        g_mat  = rdS16(20);
        g_clt  = rdS16(22);
    }
    if (s_groupSeen[3]) {
        g_tps  = rdS16(24);
        g_batt = rdS16(26);
        g_afr  = rdS16(28);
    }
    if (s_groupSeen[6]) g_iacStep = rdS16(54);
}

static uint16_t s_warnRaw = 0;
static uint16_t s_warnLatched = 0;
static uint32_t s_warnFirstMs = 0;

static const char* kTopWarnOrder[] = {
    "OVERHEAT", "OVERBOOST", "OVERREV", "LOWBATT", "HIBATT",
    "LEAN", "RICH", "HOTAIR", "IDLEHI", "IDLELO"
};

static const char* topWarnName(uint16_t raw) {
    static const uint16_t kPrio[] = { W_OVERHEAT, W_OVERBOOST, W_OVERREV, W_LOWBATT, W_HIBATT,
                                      W_LEAN, W_RICH, W_HOTAIR, W_IDLE_HI, W_IDLE_LO };
    for (uint8_t i = 0; i < 10; i++) {
        if (raw & kPrio[i]) return kTopWarnOrder[i];
    }
    return "";
}

static uint16_t engineWarnFlags() {
    uint16_t raw = 0;
    if (!s_canFresh) return 0;
    bool cltOk = s_groupSeen[2] && g_clt > 100 && g_clt < 3500;
    bool matOk = s_groupSeen[2] && g_mat > 0 && g_mat < 3000;
    bool onThrottle = s_groupSeen[3] && g_tps >= 50;
    bool afrOk = onThrottle && g_afr >= 100 && g_afr <= 250;

    if (g_rpm >= g_cfg.eng.maxRpm) raw |= W_OVERREV;
    if (g_rpm > 0 && g_rpm < g_cfg.eng.idleRpmMin && g_tps < 200) raw |= W_IDLE_LO;
    if (g_rpm > 0 && g_rpm > g_cfg.eng.idleRpmMax && g_tps < 200) raw |= W_IDLE_HI;
    if (cltOk && g_clt > g_cfg.eng.cltMax) raw |= W_OVERHEAT;
    if (matOk && g_mat > g_cfg.eng.matMax) raw |= W_HOTAIR;
    if (g_batt > 0 && g_batt < g_cfg.eng.battMin) raw |= W_LOWBATT;
    if (g_batt > 0 && g_batt > g_cfg.eng.battMax) raw |= W_HIBATT;
    if (s_groupSeen[2] && g_map > g_cfg.eng.mapMax) raw |= W_OVERBOOST;
    if (afrOk && g_afr > g_cfg.eng.afrHigh) raw |= W_LEAN;
    if (afrOk && g_afr < g_cfg.eng.afrLow) raw |= W_RICH;
    return raw;
}

static void updateEngineProfile() {
    s_warnRaw = engineWarnFlags();
    if (s_warnRaw) {
        if (!s_warnLatched) s_warnFirstMs = millis();
        s_warnLatched = s_warnRaw;
    } else if (s_warnLatched && (millis() - s_warnFirstMs) >= g_cfg.eng.warnHoldMs) {
        s_warnLatched = 0;
    }
}

static void updateOutputs() {
    bool respActive = g_cfg.respEnable && s_respPortsFresh &&
                      (millis() - s_respLastFrameMs) < FAILSAFE_MS;
    if (!s_canFresh && !respActive) {
        outputsOff();
        setIac(g_cfg.iacFailDuty);
        return;
    }
    int16_t clt = g_clt;
    bool cltOk = s_groupSeen[2] && clt > 100 && clt < 3500;

    bool fanOn = false;
    if (respActive) {
        fanOn = (s_respPort[1] & RESP_PORT_FAN_BIT) != 0;
    } else if (g_cfg.fanAuto) {
        if (cltOk) {
            static bool fanState = false;
            if (!fanState && clt >= g_cfg.fanOnTemp) fanState = true;
            else if (fanState && clt <= g_cfg.fanOffTemp) fanState = false;
            fanOn = fanState;
        }
    } else {
        fanOn = g_cfg.fanManual;
    }
    fanOn = fanOn || inputForces(7);
    setFan(fanOn);

    for (uint8_t i = 0; i < 6; i++) {
        if ((i + 1) == g_cfg.fanOut) continue;
        bool on;
        if (respActive) {
            on = (s_respPort[1] & (1u << i)) != 0;
        } else {
            on = false;
            switch (g_cfg.outMode[i]) {
                case OM_OFF:  on = false; break;
                case OM_MAN:  on = g_cfg.outManual[i]; break;
                case OM_TEMP: on = cltOk && clt >= g_cfg.outTemp[i]; break;
                case OM_RPM:  on = s_groupSeen[0] && g_rpm >= g_cfg.outRpm[i]; break;
            }
        }
        on = on || inputForces(i + 1);
        setOut(i, on);
    }

    if (g_cfg.eng.warnOut >= 1 && g_cfg.eng.warnOut <= 6 &&
        g_cfg.eng.warnOut != g_cfg.fanOut && s_warnLatched) {
        bool blink = ((millis() / 500) & 1) == 0;
        setOut(g_cfg.eng.warnOut - 1, blink);
    }

    uint8_t duty;
    if (respActive) {
        duty = (uint8_t)constrain((int16_t)(s_respPort[0] * 100 / 255), 0, 100);
    } else if (g_cfg.iacFollow && s_groupSeen[6]) {
        duty = (uint8_t)constrain((int16_t)(g_iacStep * 100 / 255), 0, 100);
    } else if (g_cfg.iacAuto || (g_cfg.iacFollow && !s_groupSeen[6])) {
        if (!cltOk) {
            duty = interpolateIac(120);
        } else {
            duty = interpolateIac(clt / 10);
            int16_t err = g_cfg.iacTargetRpm - (int16_t)g_rpm;
            int16_t trim = constrain((int16_t)(err / 20), -5, 8);
            duty = constrain((int16_t)duty + trim, 5, 95);
        }
    } else {
        duty = g_cfg.iacManualDuty;
    }
    setIac(duty);
}

static void checkTwai() {
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery();
    }
}

static void dashBroadcast() {
    twai_message_t m = {};
    m.identifier = CAN_DASH_ID;
    m.extd = 0;
    m.data_length_code = 8;
    m.data[0] = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (s_anLatch[i]) m.data[0] |= (uint8_t)(1u << i);
    }
    m.data[1] = 0;
    static uint8_t seq = 0;
    m.data[2] = ++seq;
    m.data[3] = (uint8_t)(s_warnLatched & 0xFF);
    twai_transmit(&m, 0);
}

static void drawGaugeFrame() {
    s_tft.fillScreen(GC9A01A_BLACK);
    s_tft.drawCircle(120, 120, 119, GC9A01A_NAVY);
    s_tft.drawCircle(120, 120, 118, GC9A01A_DARKGREY);
    s_tft.setTextColor(GC9A01A_CYAN);
    s_tft.setTextSize(2);
    s_tft.setCursor(96, 20);
    s_tft.print("IDLE");
    s_tft.setTextColor(GC9A01A_LIGHTGREY);
    s_tft.setTextSize(1);
    s_tft.setCursor(20, 130); s_tft.print("RPM");
    s_tft.setCursor(128, 130); s_tft.print("TGT");
    s_tft.setCursor(20, 168); s_tft.print("CLT");
    s_tft.setCursor(128, 168); s_tft.print("MODE");
}

static char s_lastDuty[8] = "";
static char s_lastRpm[8] = "";
static char s_lastTgt[8] = "";
static char s_lastClt[8] = "";
static char s_lastMode[8] = "";
static char s_lastStat[24] = "";
static char s_lastWarn[12] = "";

static void drawValue(int16_t x, int16_t y, uint8_t size, uint16_t color,
                      uint16_t clearW, char* last, const char* s) {
    if (strcmp(last, s) == 0) return;
    strcpy(last, s);
    s_tft.fillRect(x, y - 2, clearW, size * 8 + 4, GC9A01A_BLACK);
    s_tft.setCursor(x, y);
    s_tft.setTextSize(size);
    s_tft.setTextColor(color);
    s_tft.print(s);
}

static void updateDisplay() {
    char buf[12];

    uint8_t duty = (uint8_t)(ledcRead(0) * 100 / 1023);
    snprintf(buf, sizeof buf, "%u%%", duty);
    drawValue(48, 48, 6, GC9A01A_CYAN, 144, s_lastDuty, buf);

    snprintf(buf, sizeof buf, "%u", s_canFresh ? g_rpm : 0);
    drawValue(20, 146, 2, s_canFresh ? GC9A01A_WHITE : GC9A01A_DARKGREY, 100, s_lastRpm, buf);

    snprintf(buf, sizeof buf, "%d", (int)g_cfg.iacTargetRpm);
    drawValue(128, 146, 2, GC9A01A_YELLOW, 100, s_lastTgt, buf);

    bool cltOk = s_groupSeen[2] && g_clt > 100 && g_clt < 3500;
    snprintf(buf, sizeof buf, cltOk ? "%dF" : "--", g_clt / 10);
    drawValue(20, 184, 2, cltOk ? GC9A01A_YELLOW : GC9A01A_DARKGREY, 100, s_lastClt, buf);

    bool respActive = g_cfg.respEnable && s_respPortsFresh &&
                      (millis() - s_respLastFrameMs) < FAILSAFE_MS;
    const char* mode = respActive ? "RMT" : (g_cfg.iacFollow ? "FOLLOW" : (g_cfg.iacAuto ? "AUTO" : "MAN"));
    snprintf(buf, sizeof buf, "%s", mode);
    drawValue(128, 184, 2, respActive ? GC9A01A_MAGENTA : GC9A01A_GREEN, 100, s_lastMode, buf);

    const char* warn = s_warnLatched ? topWarnName(s_warnLatched) : "";
    if (strcmp(s_lastWarn, warn)) {
        strcpy(s_lastWarn, warn);
        s_tft.fillRect(10, 202, 220, 10, GC9A01A_BLACK);
        if (s_warnLatched) {
            bool blink = ((millis() / 500) & 1) == 0;
            s_tft.setCursor(10, 202);
            s_tft.setTextSize(1);
            s_tft.setTextColor(blink ? GC9A01A_RED : GC9A01A_DARKGREY);
            s_tft.print(warn);
        }
    }

    uint16_t sc;
    if (!s_canFresh) {
        sc = GC9A01A_RED;
        snprintf(buf, sizeof buf, "CAN LOST");
    } else if (g_cfg.fanOut >= 1 && g_cfg.fanOut <= 6 && digitalRead(OUT_PINS[g_cfg.fanOut - 1])) {
        sc = GC9A01A_GREEN;
        snprintf(buf, sizeof buf, "FAN ON  CAN OK");
    } else {
        sc = GC9A01A_LIGHTGREY;
        snprintf(buf, sizeof buf, "FAN OFF  CAN OK");
    }
    if (strcmp(s_lastStat, buf)) {
        strcpy(s_lastStat, buf);
        s_tft.fillRect(60, 212, 120, 10, GC9A01A_BLACK);
        s_tft.setCursor(60, 212);
        s_tft.setTextSize(1);
        s_tft.setTextColor(sc);
        s_tft.print(buf);
    }
}

static void saveCfg() {
    g_prefs.putBytes("cfg", &g_cfg, sizeof(g_cfg));
}

static void loadCfg() {
    size_t len = g_prefs.getBytes("cfg", &g_cfg, sizeof(g_cfg));
    if (len != sizeof(g_cfg) || g_cfg.magic != CFG_MAGIC) {
        g_cfg = Cfg{};
        saveCfg();
    }
}

static void reportStatus() {
    Serial.printf("can_fresh=%d rpm=%u map=%.1f mat=%.1fF clt=%.1fF tps=%.1f%% batt=%.1fV afr=%.1f\n",
                  s_canFresh ? 1 : 0, g_rpm,
                  g_map / 10.0f, g_mat / 10.0f, g_clt / 10.0f,
                  g_tps / 10.0f, g_batt / 10.0f, g_afr / 10.0f);
    Serial.printf("fan=%d iac=%s duty=%d", g_cfg.fanOut >= 1 && g_cfg.fanOut <= 6 && digitalRead(OUT_PINS[g_cfg.fanOut - 1]) ? 1 : 0,
                  g_cfg.iacFollow ? "follow" : (g_cfg.iacAuto ? "auto" : "manual"),
                  (uint8_t)(ledcRead(0) * 100 / 1023));
    if (g_cfg.iacFollow) Serial.printf(" iacstep=%d", g_iacStep);
    Serial.printf(" resp=%s id=%u", g_cfg.respEnable ? "on" : "off", g_cfg.respId);
    if (g_cfg.respEnable) {
        bool fresh = s_respPortsFresh && (millis() - s_respLastFrameMs) < FAILSAFE_MS;
        Serial.printf(" ports=%s base=%u", fresh ? "fresh" : (s_respPortsFresh ? "stale" : "idle"), s_respPortBase);
    }
    for (uint8_t i = 0; i < 6; i++) Serial.printf(" o%u=%d", i + 1, digitalRead(OUT_PINS[i]) ? 1 : 0);
    Serial.println();
    for (uint8_t i = 0; i < 4; i++) {
        Serial.printf("a%u=%.2fV(%s) ", i + 1, readAnalogMv(i) / 1000.0f, tgtName(g_cfg.anOut[i]));
    }
    Serial.println();
    if (g_cfg.eng.enabled) {
        const char* w = topWarnName(s_warnLatched);
        Serial.printf("eng=on idle[%d-%d] maxrpm=%d clt<=%dF mat<=%dF batt[%d-%d]V map<=%dkPa afr lean>%d rich<%d warnout=%u hold=%ums warn=%s\n",
                      g_cfg.eng.idleRpmMin, g_cfg.eng.idleRpmMax, g_cfg.eng.maxRpm,
                      g_cfg.eng.cltMax / 10, g_cfg.eng.matMax / 10,
                      g_cfg.eng.battMin / 10, g_cfg.eng.battMax / 10,
                      g_cfg.eng.mapMax / 10, g_cfg.eng.afrHigh / 10, g_cfg.eng.afrLow / 10,
                      g_cfg.eng.warnOut, g_cfg.eng.warnHoldMs,
                      s_warnLatched ? w : "none");
    } else {
        Serial.printf("eng=off\n");
    }
}

static void handleCommand(const String& line) {
    String c = line;
    c.trim();
    if (c.length() == 0) return;
    if (c == "?") { reportStatus(); return; }

    char key = c[0];
    String val = c.substring(1);
    val.trim();

    switch (key) {
        case 'F':
            if (val == "A") { g_cfg.fanAuto = true; }
            else if (val == "1" || val == "0") {
                g_cfg.fanAuto = false;
                g_cfg.fanManual = (val == "1");
            } else if (val.length() > 0) {
                g_cfg.fanAuto = true;
                g_cfg.fanOnTemp = (int16_t)(val.toFloat() * 10.0f);
            }
            saveCfg();
            break;
        case 'E': {
            float t = val.toFloat();
            if (t > 0) g_cfg.fanOffTemp = (int16_t)(t * 10.0f);
            saveCfg();
            break;
        }
        case 'I':
            if (val == "F") {
                g_cfg.iacAuto = false;
                g_cfg.iacFollow = true;
            } else if (val == "A") {
                g_cfg.iacFollow = false;
                g_cfg.iacAuto = true;
            } else if (val.length() > 0) {
                g_cfg.iacFollow = false;
                g_cfg.iacAuto = false;
                g_cfg.iacManualDuty = (uint8_t)constrain((int)val.toInt(), 0, 100);
            }
            saveCfg();
            break;
        case 'T': {
            float t = val.toFloat();
            if (t >= 500) g_cfg.iacTargetRpm = (int16_t)t;
            saveCfg();
            break;
        }
        case 'Y': {
            int k = val.toInt();
            if (k >= 0 && k <= 6) {
                g_cfg.fanOut = (uint8_t)k;
                if (k > 0) setFan(false);
            }
            saveCfg();
            break;
        }
        case 'R': {
            if (val == "1") g_cfg.respEnable = true;
            else if (val == "0") g_cfg.respEnable = false;
            else if (val.length() >= 2 && val[0] == 'B') {
                int id = val.substring(1).toInt();
                if (id >= 1 && id <= 14) g_cfg.respId = (uint8_t)id;
            }
            saveCfg();
            break;
        }
        case 'S': {
            int rpm = val.toInt();
            if (rpm > 0) {
                g_cfg.shiftRpm = rpm;
                g_cfg.outMode[0] = OM_RPM;
                g_cfg.outRpm[0] = rpm;
            }
            saveCfg();
            break;
        }
        case 'O': {
            if (val.length() < 2) return;
            uint8_t n = (uint8_t)(val[0] - '1');
            if (n > 5) return;
            String mode = val.substring(1);
            mode.trim();
            if (mode == "0") { g_cfg.outMode[n] = OM_OFF; }
            else if (mode == "1") { g_cfg.outMode[n] = OM_MAN; g_cfg.outManual[n] = true; }
            else if (mode == "A") { g_cfg.outMode[n] = OM_OFF; }
            else if (mode.startsWith("T")) {
                g_cfg.outMode[n] = OM_TEMP;
                g_cfg.outTemp[n] = (int16_t)(mode.substring(1).toFloat() * 10.0f);
            } else if (mode.startsWith("R")) {
                g_cfg.outMode[n] = OM_RPM;
                g_cfg.outRpm[n] = (int16_t)mode.substring(1).toInt();
            } else {
                g_cfg.outMode[n] = OM_MAN;
                g_cfg.outManual[n] = (mode.toInt() != 0);
            }
            saveCfg();
            break;
        }
        case 'A': {
            if (val.length() < 2) return;
            uint8_t n = (uint8_t)(val[0] - '1');
            if (n > 3) return;
            String m = val.substring(1);
            m.trim();
            if (m == "0") {
                g_cfg.anEnable[n] = false;
            } else if (m.startsWith("O")) {
                uint8_t o = (uint8_t)(m[1] - '1');
                if (o <= 5) {
                    g_cfg.anOut[n] = o + 1;
                    g_cfg.anEnable[n] = true;
                    float v = m.substring(2).toFloat();
                    if (v >= 0.1f && v <= 5.0f) g_cfg.anThresh[n] = (uint16_t)(v * 1000.0f);
                }
            } else if (m.startsWith("F")) {
                g_cfg.anOut[n] = 7;
                g_cfg.anEnable[n] = true;
                float v = m.substring(1).toFloat();
                if (v >= 0.1f && v <= 5.0f) g_cfg.anThresh[n] = (uint16_t)(v * 1000.0f);
            } else {
                float v = m.toFloat();
                if (v >= 0.1f && v <= 5.0f) {
                    g_cfg.anEnable[n] = true;
                    g_cfg.anThresh[n] = (uint16_t)(v * 1000.0f);
                }
            }
            saveCfg();
            break;
        }
        case 'W': {
            if (val == "0") { g_cfg.eng.enabled = false; saveCfg(); break; }
            if (val == "1") { g_cfg.eng.enabled = true; saveCfg(); break; }
            int sp = val.indexOf(' ');
            String k = sp > 0 ? val.substring(0, sp) : val;
            String v = sp > 0 ? val.substring(sp + 1) : "";
            v.trim();
            int a = v.indexOf(' ');
            String p1 = a > 0 ? v.substring(0, a) : v;
            String p2 = a > 0 ? v.substring(a + 1) : "";
            p2.trim();
            if (k == "idle" && a > 0) {
                g_cfg.eng.idleRpmMin = (int16_t)constrain(p1.toInt(), 300, 3000);
                g_cfg.eng.idleRpmMax = (int16_t)constrain(p2.toInt(), 300, 3000);
            } else if (k == "maxrpm" && p1.length() > 0) {
                g_cfg.eng.maxRpm = (int16_t)constrain(p1.toInt(), 1000, 20000);
            } else if (k == "clt" && p1.length() > 0) {
                g_cfg.eng.cltMax = (int16_t)(p1.toFloat() * 10.0f);
            } else if (k == "mat" && p1.length() > 0) {
                g_cfg.eng.matMax = (int16_t)(p1.toFloat() * 10.0f);
            } else if (k == "batt" && a > 0) {
                g_cfg.eng.battMin = (int16_t)(p1.toFloat() * 10.0f);
                g_cfg.eng.battMax = (int16_t)(p2.toFloat() * 10.0f);
            } else if (k == "map" && p1.length() > 0) {
                g_cfg.eng.mapMax = (int16_t)(p1.toFloat() * 10.0f);
            } else if (k == "afr" && a > 0) {
                g_cfg.eng.afrLow = (int16_t)(p1.toFloat() * 10.0f);
                g_cfg.eng.afrHigh = (int16_t)(p2.toFloat() * 10.0f);
            } else if (k == "hold" && p1.length() > 0) {
                g_cfg.eng.warnHoldMs = (uint16_t)constrain(p1.toInt(), 0, 60000);
            } else if (k == "warnout" && p1.length() > 0) {
                g_cfg.eng.warnOut = (uint8_t)constrain(p1.toInt(), 0, 6);
            } else if (k == "help") {
                Serial.println("W[0|1] | W idle <min> <max> | W maxrpm <rpm> | W clt <F> | W mat <F> | W batt <min> <max> | W map <kPa> | W afr <min> <max> | W hold <ms> | W warnout <0-6>");
                break;
            } else {
                Serial.println("W[0|1] | W idle <min> <max> | W maxrpm <rpm> | W clt <F> | W mat <F> | W batt <min> <max> | W map <kPa> | W afr <min> <max> | W hold <ms> | W warnout <0-6>");
                break;
            }
            saveCfg();
            Serial.println("eng profile updated");
            break;
        }
        default:
            Serial.println("commands: ? | F[onTempF|A|1|0] | E[offTempF] | I[duty|A|F] | T[targetRpm] | Y[fanOut 1-6|0] | S[shiftRpm] | O<n>[0|1|T<f>|R<rpm>] | A<n>[0|O<k> <v>|F <v>|<v>] | R[0|1|B<id>] | W[0|1|idle|maxrpm|clt|mat|batt|map|afr|hold|warnout|help]");
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    s_tft.begin();
    s_tft.setRotation(0);
    s_tft.fillScreen(GC9A01A_BLACK);
    drawGaugeFrame();

    g_prefs.begin(kPrefsName, false);
    loadCfg();

    for (uint8_t i = 0; i < 6; i++) pinMode(OUT_PINS[i], OUTPUT);
    pinMode(PIN_LED, OUTPUT);
    outputsOff();
    setIac(0);

    for (uint8_t i = 0; i < 4; i++) {
        pinMode(ADC_PINS[i], INPUT);
        analogSetPinAttenuation(ADC_PINS[i], ADC_11db);
    }

    ledcSetup(0, 30, 10);
    ledcAttachPin(PIN_IAC, 0);

    twai_general_config_t gen = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t  tim = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  flt;
    flt.acceptance_code = 0;
    flt.acceptance_mask = 0xFFFFFFFFu;
    flt.single_filter = true;

    if (twai_driver_install(&gen, &tim, &flt) == ESP_OK) {
        twai_start();
        Serial.println("TWAI started: 500kbps, broadcast 0x5F0-0x5F8 + cmds 0x600 + 29-bit responder + dash 0x710");
    } else {
        Serial.println("TWAI install FAILED");
    }

    Serial.println("MS2/Extra I/O box ready. Type ? for status.");
}

void loop() {
    respPopulateAdc();
    readCan();
    s_canFresh = s_anyGroupSeen && (millis() - s_lastFrameMs) < FAILSAFE_MS;
    decodeOutpc();
    updateAnalogLatch();
    updateEngineProfile();
    updateOutputs();
    checkTwai();

    static uint32_t ledLast = 0;
    uint32_t now = millis();
    uint32_t period = s_canFresh ? 1000 : 120;
    if (now - ledLast >= period) {
        ledLast = now;
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    static uint32_t tftLast = 0;
    if (now - tftLast >= 100) {
        tftLast = now;
        updateDisplay();
    }

    static uint32_t dashLast = 0;
    if (now - dashLast >= DASH_TX_MS) {
        dashLast = now;
        dashBroadcast();
    }

    if (Serial.available()) {
        static String line;
        while (Serial.available()) {
            char ch = (char)Serial.read();
            if (ch == '\n') { handleCommand(line); line = ""; }
            else if (ch != '\r') line += ch;
        }
    }

    delay(2);
}
