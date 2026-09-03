#include <stdio.h>
#include <cstring>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <atomic>
#include <cstdlib>
#include <utility>

#include "usb_serial.h"
// DISPLAY_5INCH / DISPLAY_7INCH come from CMakeLists.txt's own
// target_compile_definitions() on the whole target (same as every other
// file in this project) -- no need to pull in display_config.h here just
// for these two macros; this file never touches the DisplayDriver type
// that header also defines.

// Same global trace flag used throughout the project (Config > Trace
// menu) -- gates the touch-event logging below.
extern bool bTrace;
#include "touch.h"

// -----------------------------------------------------------------------
// Shared between both touch controllers: pin assignments, the raw touch
// event format, and the IRQ plumbing that feeds touch_poll()'s debounce
// logic further down. The two panels use different controller chips
// (GSL1680 on the 5" board, FT5316 on the 7") but the SAME physical I2C/
// IRQ pins on this board design, and touch_poll()'s tap/release/swipe
// state machine only ever calls touch_get_point()/touch_is_down() --
// it doesn't know or care which chip answers those.
// -----------------------------------------------------------------------
#define TOUCH_SDA   16
#define TOUCH_SCL   17
#define IRQ_PIN     19

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

#if defined(DISPLAY_7INCH)
// =========================================================================
// FT5316 -- 7" panel (ER-TPC070-6)
//
// Register map confirmed against hellange/arduino's FT5xx6.h, which
// documents itself as tested against this exact panel ("Later tested
// with separate 7" touch panel ER-TPC070-6 with FT5316 controller"):
// https://github.com/hellange/arduino/blob/master/capacitive_7in_panel/FT5xx6.h
//
// Much simpler bring-up than GSL1680 -- no firmware upload needed at
// all, just a power-on wait and the interrupt pin setup shared above.
// =========================================================================

char gTouchDevice[] = "FT5316";

#define FT5316_I2C_ADDR   0x38

#define FT5316_TD_STATUS  0x02   // bits[3:0]: number of touch points (0-5)
#define FT5316_TOUCH1_XH  0x03   // bits[7:6]: event flag (unused here --
                                  // touch_poll()'s own debounce already
                                  // handles press/release transitions from
                                  // just the point count); bits[3:0]: X[11:8]
#define FT5316_TOUCH1_XL  0x04
#define FT5316_TOUCH1_YH  0x05   // bits[7:4]: touch/finger ID; bits[3:0]: Y[11:8]
#define FT5316_TOUCH1_YL  0x06

static int i2c_read(const uint8_t reg, uint8_t *buf, const uint8_t nbytes) {
    if (nbytes < 1) return 0;
    i2c_write_blocking(touch_i2c, FT5316_I2C_ADDR, &reg, 1, true);
    return i2c_read_blocking(touch_i2c, FT5316_I2C_ADDR, buf, nbytes, false);
}

int touchInit() {
    i2c_init(touch_i2c, 400 * 1000);
    gpio_set_function(TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TOUCH_SCL, GPIO_FUNC_I2C);

    // FT5316 needs no firmware upload (unlike GSL1680) -- it's fully
    // functional right after its own power-on reset settles.
    sleep_ms(50);

    setupDataReadyInterrupt();
    return 0;
}

// Reads TD_STATUS + the first touch point's X/Y/ID in one 5-byte burst
// (registers 0x02-0x06 are contiguous). verbose controls whether this
// logs -- mirrors the old touch_read()/touch_read_silent() split, since
// the silent version is called frequently by touch_poll() and shouldn't
// spam the console.
static uint8_t ft5316_read_touches(bool verbose) {
    uint8_t buf[5] = {0};
    if (i2c_read(FT5316_TD_STATUS, buf, sizeof(buf)) < 0) {
        ts_event.NBfingers = 0;
        return 0;
    }
    std::uint8_t n = buf[0] & 0x0F;
    if (n > 5) n = 0;   // guards against a garbled/mid-transaction read
    ts_event.NBfingers = n;
    if (n > 0) {
        ts_event.fingers[0].x = ((static_cast<uint32_t>(buf[1]) & 0x0F) << 8) | buf[2];
        ts_event.fingers[0].y = ((static_cast<uint32_t>(buf[3]) & 0x0F) << 8) | buf[4];
        ts_event.fingers[0].fingerID = static_cast<uint32_t>(buf[3]) >> 4;
    }
    if (verbose && n) {
        LOGF("Finger event: 0(%d): [%d,%d]\r\n",
             ts_event.fingers[0].fingerID, ts_event.fingers[0].x, ts_event.fingers[0].y);
    }
    return n;
}

uint8_t touch_read() {
    return ft5316_read_touches(true);
}

static uint8_t touch_read_silent() {
    return ft5316_read_touches(false);
}

// Hamtar forsta fingrets position. Returnerar false om inget finger ar nere.
//
// NOTE: FT5316-family controllers are typically factory-calibrated to
// report coordinates already in the panel's own pixel space (0..1023,
// 0..599 for this 1024x600 ER-TPC070-6 panel) -- unlike GSL1680's raw
// 12-bit ADC values, which need scaling (see the DISPLAY_5INCH branch
// below). No scaling applied here for that reason. If X/Y come out
// swapped or inverted on real hardware -- a common panel-mounting-
// orientation gotcha, not something datasheets typically specify -- swap
// or negate right here rather than touching touch_poll()'s generic
// debounce logic further down, which doesn't know or care about any of
// this.
bool touch_get_point(uint16_t& x, uint16_t& y) {
    if (touch_read_silent() == 0) return false;
    // Confirmed via real-hardware calibration: this panel reports raw
    // coordinates mirrored on BOTH axes relative to the display's own
    // pixel space -- physical (0,0) read back as roughly (max_x,max_y)
    // and vice versa (e.g. physical top-left measured ~(970,598) against
    // a 1024x600 panel, physical bottom-right ~(20,4)). Not a swap, both
    // axes independently inverted -- consistent with this panel being
    // mounted rotated 180 degrees relative to the controller's own
    // default orientation, which (like the earlier "does X/Y need
    // swapping" question this function's own comment already flagged as
    // unconfirmed) isn't something the datasheet specifies up front.
    //
    // Local constants rather than LT7683.hpp's SCREEN_MAX_X/Y -- this
    // file deliberately doesn't pull in display_config.h (see the note
    // where usb_serial.h is included above), and this panel's resolution
    // is already effectively hardcoded throughout this branch anyway
    // (same as GSL1680's own TOUCH_RAW_MAX_X/Y below).
    constexpr uint16_t kPanelMaxX = 1024;
    constexpr uint16_t kPanelMaxY = 600;
    x = static_cast<uint16_t>((kPanelMaxX - 1) - ts_event.fingers[0].x);
    y = static_cast<uint16_t>((kPanelMaxY - 1) - ts_event.fingers[0].y);
    return true;
}

bool touch_is_down() {
    return touch_read_silent() > 0;
}

#else  // DISPLAY_5INCH
// =========================================================================
// GSL1680 -- 5" panel
// Description: https://www.buydisplay.com/download/ic/GSL1680.pdf
// https://weatherhelge.wordpress.com/2015/02/09/5-capacitive-touch-panel-with-gsl1680-upn-running-with-arduino/
// https://linux-sunxi.org/GSL1680
// https://github.com/hellange/GSL1680
// =========================================================================
#include "gslX680fw.h"

char gTouchDevice[] = "GSL1680";

#define WAKE_PIN    18

#define GSLX680_I2C_ADDR    0x40

//#define DEBUG

#define GSL_DATA_REG		0x80
#define GSL_STATUS_REG		0xe0
#define GSL_PAGE_REG		0xf0

#define I2CBUF_SIZE         0x20
#define I2CBUF_MASK         ~(I2CBUF_SIZE-1)

// I2C reserves some addresses for special purposes. We exclude these from the scan.
// These are any addresses of the form 000 0xxx or 111 1xxx
bool reserved_addr(uint8_t addr) {
    return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

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

#endif  // DISPLAY_5INCH / DISPLAY_7INCH

// -----------------------------------------------------------------------
// Debounced tap detection -- shared between both controllers. Only ever
// calls touch_get_point()/touch_is_down() (via touch_read_silent() inside
// those, chip-specific above), so nothing here needs to know which chip
// is actually answering.
// -----------------------------------------------------------------------

namespace {
bool touchActive = false;
bool touchConfirmed = false;
// Separate from touchConfirmed -- that means "confirmed as a tap"
// specifically, whereas this just means "the one-shot confirm check at
// kTouchConfirmMs has already run" (pass or fail). Without it, a touch
// that FAILS confirmation (moved too far -- i.e. every real swipe) kept
// re-entering that check on every single touch_poll() call for as long
// as the finger stayed down, since time_reached(confirmDeadline) stays
// true forever once reached -- spamming the trace log hundreds of times
// per gesture and wasting an I2C read each time.
bool confirmChecked = false;
uint16_t pendingTx = 0, pendingTy = 0;
absolute_time_t confirmDeadline;
absolute_time_t lastReleasePoll = get_absolute_time();

// See the rationale in touch.h -- a single stray reading (no real finger)
// looks identical to a real tap at the first sample, so we wait a short
// moment and check the same spot is still reporting "down" before acting.
constexpr uint32_t kTouchConfirmMs = 60;
constexpr int kTouchConfirmToleranceSq = 30 * 30;  // 30px radius, squared

// Swipe detection -- evaluated at release using the very first sample
// (pendingTx/pendingTy) vs. the last known position before the finger
// lifted (lastTx/lastTy, updated on every release-poll tick below, not
// just at the 60ms confirm mark, so a slower real-world swipe still gets
// tracked all the way to its actual endpoint).
constexpr int kSwipeMinDx = 150;      // minimum horizontal travel, in pixels
constexpr int kSwipeMinDy = 150;      // minimum vertical travel, in pixels -- same
                                       // feel as the horizontal one, just for the
                                       // "Devices" list's scroll gesture instead
uint16_t lastTx = 0, lastTy = 0;

TouchTapCallback     tapCallback;
TouchReleaseCallback releaseCallback;
TouchSwipeCallback   swipeCallback;
TouchVerticalSwipeCallback verticalSwipeCallback;
}  // namespace

void touch_set_tap_callback(TouchTapCallback cb) {
    tapCallback = std::move(cb);
}

void touch_set_release_callback(TouchReleaseCallback cb) {
    releaseCallback = std::move(cb);
}

void touch_set_swipe_callback(TouchSwipeCallback cb) {
    swipeCallback = std::move(cb);
}

void touch_set_vertical_swipe_callback(TouchVerticalSwipeCallback cb) {
    verticalSwipeCallback = std::move(cb);
}

void touch_poll() {
    // Fast reaction to a NEW press (the interrupt flag is set by IRQ_PIN).
    // Only records the press and arms the confirmation timer here -- the
    // tap callback only fires once the touch has been debounced below.
    if (g_dataReadyFlag.exchange(false, std::memory_order_relaxed)) {
        if (!touchActive) {
            uint16_t tx, ty;
            if (touch_get_point(tx, ty)) {
                touchActive     = true;
                touchConfirmed  = false;
                confirmChecked  = false;
                pendingTx = tx;
                pendingTy = ty;
                lastTx = tx;
                lastTy = ty;
                confirmDeadline = make_timeout_time_ms(kTouchConfirmMs);
                if (bTrace) LOGF("\r\n[TOUCH] press (%u,%u)", tx, ty);
            }
        }
        // If touchActive is already true: ignore -- the finger is still
        // down, just waiting for release before the next press counts.
    }

    // Confirmation step: re-sample a moment after the first detection,
    // before acting on it. Filters out single-sample glitches (no second
    // reading at all, or one that's jumped far away) at the cost of only
    // kTouchConfirmMs of added latency on a genuine tap.
    if (touchActive && !confirmChecked && time_reached(confirmDeadline)) {
        confirmChecked = true;
        uint16_t tx, ty;
        if (touch_get_point(tx, ty)) {
            const int dx = static_cast<int>(tx) - static_cast<int>(pendingTx);
            const int dy = static_cast<int>(ty) - static_cast<int>(pendingTy);
            if (dx * dx + dy * dy <= kTouchConfirmToleranceSq) {
                touchConfirmed = true;
                if (bTrace) LOGF("\r\n[TOUCH] confirmed tap (%u,%u)", pendingTx, pendingTy);
                if (tapCallback) tapCallback(pendingTx, pendingTy);
            } else if (bTrace) {
                LOGF("\r\n[TOUCH] confirm failed: moved (%d,%d) from (%u,%u) -- treating as drag/swipe candidate",
                     dx, dy, pendingTx, pendingTy);
            }
            // else: moved too far between samples -- treat as noise or a
            // drag, ignore. Still waits for release below as normal.
        } else if (bTrace) {
            LOGF("\r\n[TOUCH] confirm failed: finger gone by %ums -- single-sample blip", kTouchConfirmMs);
        }
        // else: finger already gone by the confirm deadline -- was a
        // single-sample blip, not a real press. Ignore it.
    }

    // Lightweight poll (every ~30 ms) to detect WHEN the finger lifts --
    // no IRQ arrives for that, so we have to ask actively. Also doubles as
    // our ongoing position tracker: touch_get_point() gives us both "is it
    // still down" and "where" in one I2C read, so we use it here instead
    // of the plain touch_is_down() check, updating lastTx/lastTy each time
    // so a swipe's actual endpoint is known even if the gesture takes
    // longer than the initial confirm window.
    if (touchActive &&
        absolute_time_diff_us(lastReleasePoll, get_absolute_time()) > 30'000) {
        lastReleasePoll = get_absolute_time();
        uint16_t tx, ty;
        if (touch_get_point(tx, ty)) {
            lastTx = tx;
            lastTy = ty;
            if (bTrace) LOGF("\r\n[TOUCH]   still down @ (%u,%u)", tx, ty);
        } else {
            touchActive = false;
            touchConfirmed = false;

            // Swipe check: total travel from the very first sample to the
            // last known position before lift-off. Requires enough
            // horizontal distance, and that the gesture stayed more
            // horizontal than vertical (so a mostly-vertical drag, e.g. a
            // finger sliding while trying to tap, doesn't misfire one).
            // The vertical check below is the mirror image, for the
            // "Devices" list's scroll gesture -- mutually exclusive with
            // the horizontal one by construction (each requires the OTHER
            // axis to be the smaller of the two).
            const int dx = static_cast<int>(lastTx) - static_cast<int>(pendingTx);
            const int dy = static_cast<int>(lastTy) - static_cast<int>(pendingTy);
            const bool isSwipe = std::abs(dx) >= kSwipeMinDx && std::abs(dy) < std::abs(dx);
            const bool isVerticalSwipe = std::abs(dy) >= kSwipeMinDy && std::abs(dx) < std::abs(dy);
            if (bTrace) {
                LOGF("\r\n[TOUCH] release: start(%u,%u) end(%u,%u) dx=%d dy=%d minDx=%d -> %s",
                     pendingTx, pendingTy, lastTx, lastTy, dx, dy, kSwipeMinDx,
                     isSwipe ? "SWIPE" : (isVerticalSwipe ? "VERTICAL SWIPE" : "no swipe"));
            }
            if (isSwipe && swipeCallback) {
                swipeCallback(dx > 0);   // true = left-to-right ("forward")
            } else if (isVerticalSwipe && verticalSwipeCallback) {
                verticalSwipeCallback(dy > 0);   // true = top-to-bottom ("down")
            }

            if (releaseCallback) releaseCallback();
        }
    }
}
