#pragma once
#include <stdbool.h>

// Call from setup() AFTER power_init() (SD card needs PMU power).
// btn_held: pass true if GPIO18 (BTN_MID) was already sampled LOW at the very
//           start of setup() before slow init; avoids missing a short button press.
// Returns true if USB MSC mode was activated; caller should loop forever.
bool usb_msc_try_activate(bool btn_held);

// Request MSC mode on next boot: stores flag in RTC memory and reboots.
void usb_msc_request(void);

// True when the current boot is running in USB MSC (U-disk) mode.
bool usb_msc_is_active(void);
