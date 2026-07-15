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

typedef struct {
    uint8_t reg;
    uint8_t data[4];
    uint8_t len;
    uint8_t delay;
} TouchCmd_t;

TouchCmd_t cmdClr[4] = {
    {
        GSL_STATUS_REG,
        { 0x88, 0, 0, 0 },
        1,
        20
    },
    {
        GSL_DATA_REG,
        { 0x01, 0, 0, 0 },
        1,
        5
    },
    {
        0xe4,
        { 0x04, 0, 0, 0 },
        1,
        5
    },
    {
        GSL_STATUS_REG,
        { 0x00, 0, 0, 0 },
        1,
        20
    }
};

TouchCmd_t cmdStrt[1] = {
    {
        GSL_STATUS_REG,
        { 0x00, 0, 0, 0 },
        1,
        0
    }
};

TouchCmd_t cmdRst[3] = {
    {
        GSL_STATUS_REG,
        { 0x88, 0, 0, 0 },
        1,
        20
    },
    {
        0xe4,
        { 0x04, 0, 0, 0 },
        1,
        10
    },
    {
        0xbc,
        { 0, 0, 0, 0 },
        4,
        10
    }
};

void sendCmd(TouchCmd_t *pCmd, int nCmd)
{
    for( int i=0; i<nCmd; i++ ) {
    	i2c_write(pCmd->reg, pCmd->data, pCmd->len);
        if( pCmd->delay )
	        sleep_ms(pCmd->delay);
        pCmd++;
    }
}

void clr_reg(void)
{
    sendCmd(cmdClr, sizeof(cmdClr)/sizeof(TouchCmd_t));
}

void start_chip(void)
{
    sendCmd(cmdStrt, sizeof(cmdStrt)/sizeof(TouchCmd_t));
}

void reset_chip()
{
    sendCmd(cmdRst, sizeof(cmdRst)/sizeof(TouchCmd_t));
}

void load_fw(void)
{
	uint8_t buf[32];
	size_t source_len = (sizeof(GSLX680_FW)/sizeof(struct fw_data));

    for(int i=0; i<source_len; i++) {
        if( GSLX680_FW[i].offset == PAGE_REG ) {
            memcpy(&buf[0], &GSLX680_FW[i].val, 4);
			i2c_write(GSL_PAGE_REG, buf, 4);    // Select the block
        } else {
            int reg = GSLX680_FW[i].offset;     // Register to write to
            int off = reg & (I2CBUF_SIZE-1);    // 32-byte offset
            memcpy(&buf[off], &GSLX680_FW[i].val, 4);
            // Write when buffer is full
            if( (off+4) == I2CBUF_SIZE ) {
		        i2c_write(reg & I2CBUF_MASK, buf, I2CBUF_SIZE);
            }
        }
    }
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
        LOGF("Finger event: ");
        for(int i = 0; i<ts_event.NBfingers; i++) {
        LOGF("%d(%d): [%d,%d] ",
            i,
            ts_event.fingers[i].fingerID,
            ts_event.fingers[i].x,
            ts_event.fingers[i].y);
        }
        LOGF("\r\n");
    }
    return ts_event.NBfingers;
}

int touchInit() {

    i2c_init(touch_i2c, 400 * 1000);
    gpio_set_function(TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TOUCH_SCL, GPIO_FUNC_I2C);
    gpio_init(WAKE_PIN);
    gpio_set_dir(WAKE_PIN, GPIO_OUT);
    gpio_pull_up(WAKE_PIN);
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_IN);
    gpio_pull_up(IRQ_PIN);

    // Toggle wake to wake up the controller
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

    return 0;
}

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