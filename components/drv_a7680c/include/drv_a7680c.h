#ifndef DRV_A7680C_H
#define DRV_A7680C_H

#include "esp_err.h"
#include "driver/gpio.h"

#define A7680C_RESET_PIN GPIO_NUM_18

void drv_a7680c_init(void);
void drv_a7680c_reset(void);
void drv_a7680c_power_on(void);

#endif
