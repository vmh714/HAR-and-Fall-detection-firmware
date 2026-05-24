#ifndef SVC_CLOUD_H
#define SVC_CLOUD_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t svc_cloud_init(const char *broker_uri, const char *client_id, const char *username, const char *password);
bool svc_cloud_is_connected(void);
int svc_cloud_publish(const char *topic, const char *data, int qos, int retain);
esp_err_t svc_cloud_enqueue_imu_batch(const void *batch_data);

#endif // SVC_CLOUD_H
