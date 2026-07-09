#include <stdio.h>
#include <cstring>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/i2c.h"
#include "gslX680fw.h"

#include "usb_serial.h"

/*
 * Driver for the touch controller GSL1680
 * Description: https://www.buydisplay.com/download/ic/GSL1680.pdf
 * https://weatherhelge.wordpress.com/2015/02/09/5-capacitive-touch-panel-with-gsl1680-upn-running-with-arduino/
 * https://linux-sunxi.org/GSL1680
 * https://github.com/hellange/GSL1680
 * 
*/

#define printf cdc0_printf

#define TOUCH_SDA   16
#define TOUCH_SCL   17
#define WAKE_PIN    18
#define IRQ_PIN     19

#define GSLX680_I2C_ADDR    0x40


#define GSL_DATA_REG		0x80
#define GSL_STATUS_REG		0xe0
#define GSL_PAGE_REG		0xf0

i2c_inst_t *touch_i2c = i2c0;

// I2C reserves some addresses for special purposes. We exclude these from the scan.
// These are any addresses of the form 000 0xxx or 111 1xxx
bool reserved_addr(uint8_t addr) {
    return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

//void i2c_write_byte(uint8_t val) {
//    i2c_write_blocking(touch_i2c, GSLX680_I2C_ADDR, &val, 1, false);
//}

void i2c_write(uint8_t val, uint8_t *buf, uint8_t len) {
    uint8_t wBuf[16];
    wBuf[0] = val;
    memcpy(&wBuf[1], buf, len);
    int n = i2c_write_blocking(touch_i2c, GSLX680_I2C_ADDR, wBuf, len+1, false);
}

void clr_reg(void)
{
	uint8_t buf[4];

	buf[0] = 0x88;
	i2c_write(GSL_STATUS_REG, buf, 1);
	sleep_ms(20);

	buf[0] = 0x01;
	i2c_write(GSL_DATA_REG, buf, 1);
	sleep_ms(5);

	buf[0] = 0x04;
	i2c_write(0xe4, buf, 1);
	sleep_ms(5);

	buf[0] = 0x00;
	i2c_write(GSL_STATUS_REG, buf, 1);
	sleep_ms(20);
}

void reset_chip()
{
	uint8_t buf[4];

	buf[0] = 0x88;
    i2c_write(GSL_STATUS_REG, buf, 1);
	sleep_ms(20);

	buf[0] = 0x04;
    i2c_write(0xe4,buf, 1);
	sleep_ms(10);

	buf[0] = 0x00;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
    i2c_write(0xbc,buf, 4);
	sleep_ms(10);
}

void load_fw(void)
{
	uint8_t buf[32];
	size_t source_len = sizeof(GSLX680_FW);
    int blockstart= 1;
	int reg= 0;
	int off= 0;
	size_t source_line;
    int nBlk = 0;
	printf("Firmware length: %d\r\n", source_len);

    for(int i=0; i<sizeof(GSLX680_FW)/sizeof(struct fw_data); i++) {
        if( GSLX680_FW[i].offset == 0xf0 ) {
			buf[0] = GSLX680_FW[i].val & 0xFF; //pgm_read_byte_far(gslx680_fw + source_line); // gslx680_fw[source_line];
			buf[1] = 0;
			buf[2] = 0;
			buf[3] = 0;
			//i2c_write(GSL_PAGE_REG, buf, 4);
        	printf("i2c_write(%X, %X %X %X %X, 4)\r\n", reg,
                buf[0], buf[1], buf[2], buf[3] );
        } else {
            int reg = GSLX680_FW[i].offset;
            int of = reg & 0x1F;
            memcpy(&buf[of], &GSLX680_FW[i].val, 4);
            if( of == 0x1C ) {
        	printf("i2c_write(%X, %X %X %X %X %X ..., 32)\r\n", reg,
                buf[0], buf[1], buf[2], buf[3], buf[4] );
            }
        }
    }
#if 0
    //    for (source_line=0; source_line < source_len; source_line++) {
    for (source_line=0; source_line < 332; source_line++) {
		if(off == 32){
			//i2c_write(reg, buf, 32); // write accumulated block
        	printf("i2c_write(%X, %X %X %X %X %X ..., 32)\r\n", reg,
                buf[0], buf[1], buf[2], buf[3], buf[4] );
			reg += 32;
			off= 0;
			if(reg >= 128) {
                blockstart= 1;
                nBlk++;
            }
		}

		if(blockstart) {
			blockstart= 0;
			buf[0] = GSLX680_FW[nBlk*33].val & 0xFF; //pgm_read_byte_far(gslx680_fw + source_line); // gslx680_fw[source_line];
			buf[1] = 0;
			buf[2] = 0;
			buf[3] = 0;
			//i2c_write(GSL_PAGE_REG, buf, 4);
        	printf("i2c_write(%X, %X %X %X %X, 4)\r\n", reg,
                buf[0], buf[1], buf[2], buf[3] );
			reg= 0;
		}else{
			//buf[off++] = pgm_read_byte_far(gslx680_fw + source_line); // gslx680_fw[source_line];
        	printf("buf[%d] = val[%d] %X >> %d\r\n", off, (nBlk*33)+1+off/4, GSLX680_FW[(nBlk*33)+1+off/4].val, 8*(off&3));
			buf[off] = ((GSLX680_FW[(nBlk*33)+1+off/4].val) >> (8*(off&3))) & 0xFF;
            off++;
		}
	}
	if(off == 32){ // write last accumulated block
		//i2c_write(reg, buf, 32);
        	printf("Last i2c_write(%X, %X %X %X %X %X ..., 32)\r\n", reg,
                buf[0], buf[1], buf[2], buf[3], buf[4] );
	}
#endif
}

int touchTest() {
    i2c_init(touch_i2c, 400 * 1000);
    gpio_set_function(TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TOUCH_SCL, GPIO_FUNC_I2C);
    gpio_init(WAKE_PIN);
    gpio_set_dir(WAKE_PIN, GPIO_OUT);
    gpio_pull_up(WAKE_PIN);
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_IN);
    gpio_pull_up(IRQ_PIN);

    printf("\r\nToggle wake!\r\n");
    gpio_put(WAKE_PIN, 1);
    sleep_ms(50);
    gpio_put(WAKE_PIN, 0);
    sleep_ms(50);
    gpio_put(WAKE_PIN, 1);
    sleep_ms(30);

    clr_reg();
    sleep_ms(50);

    reset_chip();
    sleep_ms(10);

    load_fw();
    sleep_ms(50);

    reset_chip();
    sleep_ms(10);

    printf("\r\nI2C Bus Scan\r\n");
    printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");

    for (int addr = 0; addr < (1 << 7); ++addr) {
        if (addr % 16 == 0) {
            printf("%02x ", addr);
        }

        // Perform a 1-byte dummy read from the probe address. If a slave
        // acknowledges this address, the function returns the number of bytes
        // transferred. If the address byte is ignored, the function returns
        // -1.

        // Skip over any reserved addresses.
        int ret;
        uint8_t rxdata;
        if (reserved_addr(addr))
            ret = PICO_ERROR_GENERIC;
        else
            ret = i2c_read_blocking(touch_i2c, addr, &rxdata, 1, false);

        printf(ret < 0 ? "." : "@");
        printf(addr % 16 == 15 ? "\r\n" : "  ");
    }
    printf("Done.\r\n");
    return 0;
}
