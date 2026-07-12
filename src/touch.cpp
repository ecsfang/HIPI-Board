#include <stdio.h>
#include <cstring>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/i2c.h"
#include "gslX680fw.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <atomic>

#include "usb_serial.h"
#include "touch.h"

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

//#define DEBUG

#define GSL_DATA_REG		0x80
#define GSL_STATUS_REG		0xe0
#define GSL_PAGE_REG		0xf0

#define I2CBUF_SIZE         0x20
#define I2CBUF_MASK         ~(I2CBUF_SIZE-1)

struct Finger {
    uint8_t fingerID;
    uint32_t x;
    uint32_t y;
};

struct Touch_event {
    uint8_t NBfingers;
    struct Finger fingers[5];
};

struct Touch_event ts_event;

i2c_inst_t *touch_i2c = i2c0;


// volatile/atomic flagga som satts i ISR, lases i main-loopen
std::atomic<bool> g_dataReadyFlag{false};

// Callback-signaturen kraver en fri funktion (eller static member),
// inte en vanlig medlemsmetod — SDK:t kanner inte till C++-objekt.
static void gpio_irq_handler(uint gpio, uint32_t events) {
    if (gpio == IRQ_PIN && (events & GPIO_IRQ_EDGE_RISE)) {
        g_dataReadyFlag.store(true, std::memory_order_relaxed);
    }
}

void setupDataReadyInterrupt() {
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_IN);
    gpio_pull_down(IRQ_PIN);   // eller pull_up, beroende pa vilo-lage

    gpio_set_irq_enabled_with_callback(
        IRQ_PIN,
        GPIO_IRQ_EDGE_RISE,
        true,
        &gpio_irq_handler);
}

// I2C reserves some addresses for special purposes. We exclude these from the scan.
// These are any addresses of the form 000 0xxx or 111 1xxx
bool reserved_addr(uint8_t addr) {
    return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

//void i2c_write_byte(uint8_t val) {
//    i2c_write_blocking(touch_i2c, GSLX680_I2C_ADDR, &val, 1, false);
//}

int i2c_write(uint8_t val, uint8_t *buf=NULL, uint8_t len=0)
{
    uint8_t wBuf[I2CBUF_SIZE+1];
    wBuf[0] = val;
    if( buf )
        memcpy(&wBuf[1], buf, len);
    return i2c_write_blocking(touch_i2c, GSLX680_I2C_ADDR, wBuf, len+1, false);
}

// Read byte(s) from specified register. If nbytes > 1, read from consecutive
// registers.
int i2c_read(const uint8_t reg, uint8_t *buf, const uint8_t nbytes)
{
    // Check to make sure caller is asking for 1 or more bytes
    if (nbytes < 1) {
        return 0;
    }

    // Read data from register(s) over I2C
    i2c_write_blocking(touch_i2c, GSLX680_I2C_ADDR, &reg, 1, true);
    return i2c_read_blocking(touch_i2c, GSLX680_I2C_ADDR, buf, nbytes, false);
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

void start_chip(void)
{
	uint8_t buf[4];

	buf[0] = 0x00;
	i2c_write(GSL_STATUS_REG, buf, 1);
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
	uint8_t buf[32+1];
	//size_t source_len = sizeof(GSLX680_FW);
	size_t source_len = (sizeof(GSLX680_FW)/sizeof(struct fw_data)); //sizeof(GSLX680_FW);
    int blockstart= 1;
	int reg= 0;
	int off= 0;
	size_t source_line;
    int nBlk = 0;
    int n = 0;
	printf("Firmware upload (%d bytes)\r\n", sizeof(GSLX680_FW));
	printf("Firmware upload (%d bytes)\r\n", sizeof(struct fw_data));
	printf("Firmware upload (%d bytes)\r\n", (sizeof(GSLX680_FW)/sizeof(struct fw_data)));
	printf("Firmware upload (%d bytes)\r\n", sizeof(GSLX680_FW));
    printf("Firmware upload (%d bytes)\r\n", source_len);
    tud_cdc_n_write_flush(0);
#if 1
    sleep_ms(100);


    for(int i=0; i<(sizeof(GSLX680_FW)/sizeof(struct fw_data)); i++) {
        //printf("%d[%X] --> ", i, GSLX680_FW[i].offset);
        //tud_cdc_n_write_flush(0);
        if( GSLX680_FW[i].offset == 0xf0 ) {
            //printf("=(%X)",GSLX680_FW[i].val);
            //tud_cdc_n_write_flush(0);
			buf[0] = GSLX680_FW[i].val & 0xFF; //pgm_read_byte_far(gslx680_fw + source_line); // gslx680_fw[source_line];
			buf[1] = 0;
			buf[2] = 0;
			buf[3] = 0;
#ifndef DEBUG
			n += i2c_write(GSL_PAGE_REG, buf, 4) - 1;
#else
        	printf("i2c_write(%X, %X %X %X %X, 4) -> %d\r\n", GSL_PAGE_REG,
                buf[0], buf[1], buf[2], buf[3], n );
            tud_cdc_n_write_flush(0);
            tud_task();
#endif
        } else {
            //printf(">>");
            //tud_cdc_n_write_flush(0);
            int reg = GSLX680_FW[i].offset;
            int of = reg & (I2CBUF_SIZE-1);
            memcpy(&buf[of], &GSLX680_FW[i].val, 4);
            if( (of+4) == I2CBUF_SIZE ) {
#ifndef DEBUG
			    n += i2c_write(reg & I2CBUF_MASK, buf, I2CBUF_SIZE) - 1; // write accumulated block
#else
        	    printf("i2c_write(%X, %X %X %X %X %X ... %X, 32) --> %d\r\n",
                    reg & I2CBUF_MASK,
                    buf[0], buf[1], buf[2], buf[3], buf[4], buf[I2CBUF_SIZE-1], n );
                tud_cdc_n_write_flush(0);
                tud_task();
#endif
            }
        }
        //sleep_ms(2);
        //printf("%d\r\n", n);
        tud_cdc_n_write_flush(0);
        tud_task();
    }
    printf("%d bytes written!\r\n", n);
    tud_cdc_n_write_flush(0);
    tud_task();
    sleep_ms(2000);

#else
    int nOff = 1;
    //for (source_line=0; source_line < source_len; source_line++) {
    //for (source_line=0; source_line < 13332; source_line++) {
    while(1) {
        if(off == 32) {
#ifndef DEBUG
            i2c_write(reg, buf, 32); // write accumulated block
#else
            printf("\r\ni2c_write(%X, %X %X %X %X %X ..., 32)", reg,
                buf[0], buf[1], buf[2], buf[3], buf[4] );
#endif
            reg += 32;
            off= 0;
            nOff++;
            if(reg >= 128) {
                blockstart= 1;
                nBlk++;
                nOff=1;
            }
        }
 
        if(blockstart) {
            if( (GSLX680_FW[nBlk*33].val) == 0xFF ) {
                printf("\r\nDone!\r\n");
                return;
            }
            blockstart= 0;
            buf[0] = GSLX680_FW[nBlk*33].val & 0xFF; //pgm_read_byte_far(gslx680_fw + source_line); // gslx680_fw[source_line];
            buf[1] = 0;
            buf[2] = 0;
            buf[3] = 0;
#ifndef DEBUG
            i2c_write(GSL_PAGE_REG, buf, 4);
#else
            printf("\r\ni2c_write([%d] %X, %X %X %X %X, 4)", nBlk, GSL_PAGE_REG,
                buf[0], buf[1], buf[2], buf[3] );
#endif
            reg= 0;
        } else {
            //buf[off++] = pgm_read_byte_far(gslx680_fw + source_line); // gslx680_fw[source_line];
            buf[off] = ((GSLX680_FW[(nBlk*33)+nOff+off/4].val) >> (8*(off&3))) & 0xFF;
#ifdef DEBUG
            printf(" %X:%X", (nBlk*33)+nOff+off/4, (GSLX680_FW[(nBlk*33)+1+off/4].val >> 8*(off&3)) & 0xFF);
#endif
            off++;
        }
        tud_cdc_n_write_flush(0);
    }
    if(off == 32){ // write last accumulated block
        i2c_write(reg, buf, 32);
        printf("\r\nLast i2c_write(%X, %X %X %X %X %X ..., 32)\r\n", reg,
            buf[0], buf[1], buf[2], buf[3], buf[4] );
    }
#endif

}

uint8_t touch_read()
{
    uint8_t TOUCHRECDATA[24] = {0};

    i2c_read(GSL_DATA_REG, TOUCHRECDATA, 24);

    ts_event.NBfingers = TOUCHRECDATA[0];
    for(int i = 0; i<ts_event.NBfingers; i++) {
        ts_event.fingers[i].x = ( (((uint32_t)TOUCHRECDATA[(i*4)+5])<<8) | (uint32_t)TOUCHRECDATA[(i*4)+4] ) & 0x00000FFF; // 12 bits of X coord
        ts_event.fingers[i].y = ( (((uint32_t)TOUCHRECDATA[(i*4)+7])<<8) | (uint32_t)TOUCHRECDATA[(i*4)+6] ) & 0x00000FFF;
        ts_event.fingers[i].fingerID = (uint32_t)TOUCHRECDATA[(i*4)+7] >> 4; // finger that did the touch
    }

    if( ts_event.NBfingers ) {
        printf("Finger event: ");
        for(int i = 0; i<ts_event.NBfingers; i++) {
        printf("%d(%d): [%d,%d] ",
            i,
            ts_event.fingers[i].fingerID,
            ts_event.fingers[i].x,
            ts_event.fingers[i].y);
        }
        printf("\r\n");
    }
    return ts_event.NBfingers;
}

int touchTest() {

    cdc0_printf("\r\n\t[touch] i2c_init..."); tud_cdc_n_write_flush(0); tud_task();
    sleep_ms(10);
    i2c_init(touch_i2c, 400 * 1000);
    gpio_set_function(TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TOUCH_SCL, GPIO_FUNC_I2C);
    cdc0_printf(" WAKE"); tud_cdc_n_write_flush(0); tud_task();
    sleep_ms(10);
    gpio_init(WAKE_PIN);
    gpio_set_dir(WAKE_PIN, GPIO_OUT);
    gpio_pull_up(WAKE_PIN);
    cdc0_printf(" IRQ"); tud_cdc_n_write_flush(0); tud_task();
    sleep_ms(10);
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_IN);
    gpio_pull_up(IRQ_PIN);
    cdc0_printf(" OK"); tud_cdc_n_write_flush(0); tud_task();
    sleep_ms(10);

    tud_cdc_n_write_flush(0);
    tud_task();

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

    start_chip();

    setupDataReadyInterrupt();

    printf("\r\nGSL1680 Boot up completed!\r\n");
    return 0;
}

#ifdef UI_DIALOG

// Tyst variant av avlasningen - ingen printf, anvands for intern
// tillstandssparning (kallas ofta, vill inte spamma konsolen).
static uint8_t touch_read_silent() {
    uint8_t TOUCHRECDATA[24] = {0};
    i2c_read(GSL_DATA_REG, TOUCHRECDATA, 24);

    ts_event.NBfingers = TOUCHRECDATA[0];
    for (int i = 0; i < ts_event.NBfingers; i++) {
        ts_event.fingers[i].x = ((static_cast<uint32_t>(TOUCHRECDATA[(i*4)+5]) << 8) |
                                  static_cast<uint32_t>(TOUCHRECDATA[(i*4)+4])) & 0x0FFF;
        ts_event.fingers[i].y = ((static_cast<uint32_t>(TOUCHRECDATA[(i*4)+7]) << 8) |
                                  static_cast<uint32_t>(TOUCHRECDATA[(i*4)+6])) & 0x0FFF;
        ts_event.fingers[i].fingerID = static_cast<uint32_t>(TOUCHRECDATA[(i*4)+7]) >> 4;
    }
    return ts_event.NBfingers;
}

// Rakoordinaternas fullskala fran GSL1680 (12-bit ADC).
// OBS: kalibrera dessa mot din faktiska panel - se kommentar nedan.
constexpr uint32_t TOUCH_RAW_MAX_X = 800;
constexpr uint32_t TOUCH_RAW_MAX_Y = 480;

// Hamtar forsta fingrets position, skalad till skarmens pixelupplosning
// (800x480). Returnerar false om inget finger ar nere.
bool touch_get_point(uint16_t& x, uint16_t& y) {
    if (touch_read_silent() == 0) return false;
    x = static_cast<uint16_t>((ts_event.fingers[0].x * 800) / TOUCH_RAW_MAX_X);
    y = static_cast<uint16_t>((ts_event.fingers[0].y * 480) / TOUCH_RAW_MAX_Y);
    return true;
}

// Latt koll (ingen skalning/parsing utover NBfingers) - for release-polling.
bool touch_is_down() {
    return touch_read_silent() > 0;
}

#endif