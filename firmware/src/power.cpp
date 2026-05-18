#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll intervals
#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;
#define PWR_POLL_MS 50

// LiPo OCV → SOC lookup table (resting voltage, room temp, low current).
// Linear interpolation between points.
static int voltage_to_pct(uint16_t mv) {
    if (mv == 0) return -1;
    static const struct { uint16_t mv; int8_t pct; } tbl[] = {
        {4200,100},{4150,95},{4110,90},{4080,85},
        {4020,80}, {3980,75},{3950,70},{3910,65},
        {3870,60}, {3830,55},{3790,50},{3750,45},
        {3710,40}, {3670,35},{3630,30},{3590,25},
        {3550,20}, {3510,15},{3450,10},{3270,5},
        {3000,0}
    };
    const int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
    if (mv >= tbl[0].mv)   return 100;
    if (mv <= tbl[n-1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (mv >= tbl[i+1].mv) {
            int dv = tbl[i].mv  - tbl[i+1].mv;
            int dp = tbl[i].pct - tbl[i+1].pct;
            return tbl[i+1].pct + (int)(mv - tbl[i+1].mv) * dp / dv;
        }
    }
    return 0;
}

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    delay(200);  // AXP2101 needs time after enabling battery detection

    // Enable PWR button short-press IRQ (mid button for cycling screens)
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    cached_charging = pmu.isCharging();
    cached_pct = voltage_to_pct(pmu.getBattVoltage());

    // Dump all LDO/DC states — needed to diagnose SD card power rail
    Serial.printf("AXP DC:   DC1=%s/%dmV  DC2=%s/%dmV  DC3=%s/%dmV  DC4=%s/%dmV\n",
        pmu.isEnableDC1()?"ON":"--", pmu.getDC1Voltage(),
        pmu.isEnableDC2()?"ON":"--", pmu.getDC2Voltage(),
        pmu.isEnableDC3()?"ON":"--", pmu.getDC3Voltage(),
        pmu.isEnableDC4()?"ON":"--", pmu.getDC4Voltage());
    Serial.printf("AXP ALDO: 1=%s/%d  2=%s/%d  3=%s/%d  4=%s/%d mV\n",
        pmu.isEnableALDO1()?"ON":"--", pmu.getALDO1Voltage(),
        pmu.isEnableALDO2()?"ON":"--", pmu.getALDO2Voltage(),
        pmu.isEnableALDO3()?"ON":"--", pmu.getALDO3Voltage(),
        pmu.isEnableALDO4()?"ON":"--", pmu.getALDO4Voltage());
    Serial.printf("AXP BLDO: 1=%s/%d  2=%s/%d  DLDO: 1=%s/%d  2=%s/%d mV\n",
        pmu.isEnableBLDO1()?"ON":"--", pmu.getBLDO1Voltage(),
        pmu.isEnableBLDO2()?"ON":"--", pmu.getBLDO2Voltage(),
        pmu.isEnableDLDO1()?"ON":"--", pmu.getDLDO1Voltage(),
        pmu.isEnableDLDO2()?"ON":"--", pmu.getDLDO2Voltage());
}

void power_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
    }

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = voltage_to_pct(pmu.getBattVoltage());
    }

    // Poll PWR button (AXP2101 short-press IRQ)
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) {
            pwr_pressed_flag = true;
        }
        pmu.clearIrqStatus();
    }
}

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    return cached_charging;
}

bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
