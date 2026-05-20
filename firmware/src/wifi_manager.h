#pragma once
#include <stdbool.h>

void wifi_manager_init(void);
void wifi_manager_tick(void);
bool wifi_manager_configured(void);
bool wifi_manager_connected(void);
int wifi_manager_rssi(void);
void wifi_manager_pause(bool paused);
const char* wifi_manager_ssid(void);
const char* wifi_manager_device_id(void);
void wifi_manager_print_status(void);
