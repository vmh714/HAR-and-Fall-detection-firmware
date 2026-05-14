#ifndef SYS_MANAGER_H
#define SYS_MANAGER_H

#include "esp_event.h"

// System States (FSM)
typedef enum {
    STATE_INIT,
    STATE_CONNECTING,
    STATE_NORMAL,
    STATE_STREAMING,
    STATE_OTA,
    STATE_ERROR
} system_state_t;

// Event Bases
ESP_EVENT_DECLARE_BASE(SYS_EVENT);
ESP_EVENT_DECLARE_BASE(NET_EVENT);
ESP_EVENT_DECLARE_BASE(CLOUD_EVENT);
ESP_EVENT_DECLARE_BASE(IMU_EVENT);
ESP_EVENT_DECLARE_BASE(AI_EVENT);

// System Events
typedef enum {
    SYS_EVT_READY,
    SYS_EVT_ENTER_STREAM_MODE,
    SYS_EVT_ENTER_NORMAL_MODE
} sys_event_id_t;

// Network Events
typedef enum {
    NET_EVT_WIFI_CONNECTED,
    NET_EVT_CELLULAR_CONNECTED,
    NET_EVT_DISCONNECTED
} net_event_id_t;

// Cloud Events
typedef enum {
    CLOUD_EVT_MQTT_CONNECTED,
    CLOUD_CMD_START_STREAM,
    CLOUD_CMD_STOP_STREAM
} cloud_event_id_t;

// IMU Events
typedef enum {
    IMU_EVT_BATCH_READY,
    IMU_EVT_WINDOW_READY
} imu_event_id_t;

// AI Events
typedef enum {
    AI_EVT_FALL_DETECTED
} ai_event_id_t;

void sys_manager_init(void);
system_state_t sys_manager_get_state(void);
void sys_manager_set_state(system_state_t new_state);

#endif // SYS_MANAGER_H
