#ifndef SVC_NETWORK_H
#define SVC_NETWORK_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t svc_network_init(const char *ssid, const char *pass);
bool svc_network_is_connected(void);

#endif // SVC_NETWORK_H
