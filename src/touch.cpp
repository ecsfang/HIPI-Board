#include <stdio.h>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

int touchTest() {
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(16, GPIO_FUNC_I2C);
    gpio_set_function(17, GPIO_FUNC_I2C);
    gpio_pull_up(16);
    gpio_pull_up(17);

    printf("\n\tScanning I2C0 (SDA=GP16, SCL=GP17)...\n");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t rx;
        int ret = i2c_read_blocking(i2c0, addr, &rx, 1, false);
        if (ret >= 0) {
            printf("  Found device at 0x%02X\n", addr);
        }
    }
    printf("\tDone.\n");
    return 0;
}
