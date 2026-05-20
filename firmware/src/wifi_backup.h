#pragma once
#include <stdbool.h>

void wifi_backup_init(void);
void wifi_backup_tick(void);
void wifi_backup_request_upload(void);
void wifi_backup_print_status(void);
const char* wifi_backup_device_id(void);
bool wifi_backup_configured(void);
bool wifi_backup_is_uploading(void);
