#include "usb_msc.h"
#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

// Onboard TF card uses SDMMC 1-bit mode. Must match sd_card.cpp.
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3

// TinyUSB PHY control: forces host to re-enumerate after media is ready.
// Without this, Windows may have sent READ_CAPACITY before sd_msc_init()
// completed and cached the "no media" response permanently.
extern "C" {
    void tud_disconnect(void);
    void tud_connect(void);
}

// RTC_NOINIT_ATTR survives esp_restart() without being zeroed by startup code.
// Use a magic value so random RTC contents after cold boot are ignored.
#define MSC_BOOT_MAGIC 0x434D5343UL
RTC_NOINIT_ATTR static uint32_t s_msc_request_magic;

static bool          s_msc_active = false;
static sdmmc_card_t* s_card       = nullptr;

// Global USBMSC object: its constructor calls tinyusb_enable_interface() at
// static-init time (before app_main), so the MSC interface is included in
// the USB descriptor from the very first enumeration.
static USBMSC s_msc;

static void usb_event_cb(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base != ARDUINO_USB_EVENTS) return;
    arduino_usb_event_data_t* data = (arduino_usb_event_data_t*)event_data;
    switch (event_id) {
    case ARDUINO_USB_STARTED_EVENT:
        Serial.println("[USB] configured");
        break;
    case ARDUINO_USB_STOPPED_EVENT:
        Serial.println("[USB] stopped");
        break;
    case ARDUINO_USB_SUSPEND_EVENT:
        Serial.printf("[USB] suspend remote_wakeup=%u\n", data->suspend.remote_wakeup_en);
        break;
    case ARDUINO_USB_RESUME_EVENT:
        Serial.println("[USB] resume");
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// TinyUSB MSC callbacks
// ---------------------------------------------------------------------------

static int32_t sd_on_read(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    (void)offset;
    if (!s_card) return -1;
    esp_err_t ret = sdmmc_read_sectors(s_card, buf, lba, bufsize / 512);
    if (lba < 4 || ret != ESP_OK) {
        Serial.printf("[MSC] READ lba=%lu bytes=%lu ret=0x%x\n",
            (unsigned long)lba, (unsigned long)bufsize, ret);
    }
    return ret == ESP_OK ? (int32_t)bufsize : -1;
}

static int32_t sd_on_write(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    (void)offset;
    if (!s_card) return -1;
    esp_err_t ret = sdmmc_write_sectors(s_card, buf, lba, bufsize / 512);
    Serial.printf("[MSC] WRITE lba=%lu bytes=%lu ret=0x%x\n",
        (unsigned long)lba, (unsigned long)bufsize, ret);
    return ret == ESP_OK ? (int32_t)bufsize : -1;
}

static bool sd_on_start_stop(uint8_t power, bool start, bool load_eject) {
    Serial.printf("[MSC] START_STOP power=%u start=%u eject=%u\n", power, start, load_eject);
    return true;
}

// ---------------------------------------------------------------------------
// SD init via ESP-IDF SDMMC, without mounting FATFS in MSC mode
// ---------------------------------------------------------------------------

static bool sd_msc_init(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_CLK;
    slot.cmd = (gpio_num_t)SD_CMD;
    slot.d0 = (gpio_num_t)SD_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;

    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        Serial.printf("[MSC] sdmmc_host_init failed: 0x%x\n", ret);
        return false;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        Serial.printf("[MSC] sdmmc_host_init_slot failed: 0x%x\n", ret);
        sdmmc_host_deinit();
        return false;
    }

    static sdmmc_card_t card;
    ret = sdmmc_card_init(&host, &card);
    if (ret != ESP_OK) {
        Serial.printf("[MSC] sdmmc_card_init failed: 0x%x - check card & pins\n", ret);
        sdmmc_host_deinit();
        return false;
    }

    s_card = &card;
    Serial.printf("[MSC] SD card OK: %lu sectors (%lu MB)\n",
        (unsigned long)card.csd.capacity,
        (unsigned long)(card.csd.capacity / 2048UL));
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool usb_msc_try_activate(bool btn_held) {
    // First press: store flag in RTC memory and reboot.
    // On the reboot s_msc_request_magic will be set; btn_held will be false
    // (user released the button) and we activate MSC cleanly.
    if (btn_held && s_msc_request_magic != MSC_BOOT_MAGIC) {
        Serial.println("[MSC] button held - saving flag and rebooting into MSC mode");
        s_msc_request_magic = MSC_BOOT_MAGIC;
        delay(50);
        esp_restart();
        // never returns
    }

    if (s_msc_request_magic != MSC_BOOT_MAGIC) return false;
    s_msc_request_magic = 0;

    Serial.println("[MSC] boot flag set - entering USB drive mode");

    if (!sd_msc_init()) {
        Serial.println("[MSC] SD init failed - aborting MSC mode");
        return false;
    }

    s_msc.vendorID("ESP32");
    s_msc.productID("ClawSD");
    s_msc.productRevision("1.0");
    s_msc.onRead(sd_on_read);
    s_msc.onWrite(sd_on_write);
    s_msc.onStartStop(sd_on_start_stop);
    s_msc.isWritable(true);
    if (!s_msc.begin(s_card->csd.capacity, 512)) {
        Serial.println("[MSC] USBMSC.begin() failed");
        return false;
    }
    s_msc.mediaPresent(true);
    s_msc_active = true;

    USB.onEvent(usb_event_cb);
    USB.begin();

    // USB was already running before sd_msc_init() completed. Windows may
    // have sent READ_CAPACITY while s_card was still NULL and cached the
    // resulting error as "no media". A brief disconnect forces the host to
    // re-enumerate and retry with the SD card fully ready.
    Serial.println("[MSC] forcing USB re-enumeration");
    Serial.flush();
    tud_disconnect();
    delay(200);
    tud_connect();

    return true;
}

void usb_msc_request(void) {
    s_msc_request_magic = MSC_BOOT_MAGIC;
    esp_restart();
}

bool usb_msc_is_active(void) {
    return s_msc_active;
}
