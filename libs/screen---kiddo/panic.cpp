#include "pxt.h"

#include "KiddoDisplay.h"

// Panic screen for the kidDo display. The stock panic code (screen---st7735) bit-bangs SPI, which kidDo does not
// have; this draws the same sad face and panic code through the KiddoDisplay driver instead. The driver only polls
// its DMA channels, so it works with interrupts disabled.

namespace pxt {
kiddo_arcade::KiddoDisplay &getKiddoDisplay();
}

namespace _pxt_panic {

using kiddo_arcade::KiddoDisplay;

// target_panic has been called
static bool panicMode = false;

static const uint8_t numbers[] = {
    0x06, 0x09, 0x09, 0x09, 0x06, // 0
    0x04, 0x06, 0x04, 0x04, 0x0e, // 1
    0x07, 0x08, 0x06, 0x01, 0x0f, // 2
    0x0f, 0x08, 0x04, 0x09, 0x06, // 3
    0x0c, 0x0a, 0x09, 0x1f, 0x08, // 4
    0x1f, 0x01, 0x0f, 0x10, 0x0f, // 5
    0x08, 0x04, 0x0e, 0x11, 0x0e, // 6
    0x1f, 0x08, 0x04, 0x02, 0x01, // 7
    0x0e, 0x11, 0x0e, 0x11, 0x0e, // 8
    0x0e, 0x11, 0x0e, 0x04, 0x02, // 9
    0x11, 0x00, 0x0e, 0x1b, 0x11, // :(
};

// black, red, yellow; the rest unused
static const uint8_t panicPalette[48] = {
    0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00,
};

// MakeCode image layout (column-major, 4 bits per pixel), static so that it does not depend on the heap
static uint8_t panicScreen[KiddoDisplay::FRAME_BYTES];

#define SIZE 4

static void setPixel(int x, int y, uint8_t c) {
    uint8_t &b = panicScreen[x * KiddoDisplay::COLUMN_BYTES + y / 2];
    if (y & 1)
        b = (b & 0x0f) | (c << 4);
    else
        b = (b & 0xf0) | c;
}

static void drawNumber(int idx, int x, int y, uint8_t color) {
    const uint8_t *src = &numbers[idx * 5];
    for (int i = 0; i < 5; i++) {
        uint8_t ch = *src++;
        for (int j = 0; j < 5; j++) {
            if (ch & (1 << j)) {
                for (int xx = 0; xx < SIZE - 1; ++xx)
                    for (int yy = 0; yy < SIZE - 1; ++yy)
                        setPixel(x + j * SIZE + xx, y + i * SIZE + yy, color);
            }
        }
    }
}

static void drawPanic(int code) {
    memset(panicScreen, 0, sizeof(panicScreen));
    drawNumber(10, 70, 20, 1);
    int x = 50;
    int y = 60;
    drawNumber((code / 100) % 10, x, y, 2);
    x += 24;
    drawNumber((code / 10) % 10, x, y, 2);
    x += 24;
    drawNumber((code / 1) % 10, x, y, 2);
    auto &display = pxt::getKiddoDisplay();
    display.setPalette(panicPalette);
    display.sendFrame(panicScreen);
    display.waitForSendDone();
}

extern "C" __attribute__((weak)) void platform_panic(int statusCode) {}

extern "C" void target_panic(int statusCode) {
    __disable_irq();      // low-level disable
    target_disable_irq(); // make sure they stay disabled in DMESG()
    DMESG("*** CODAL PANIC : [%d]", statusCode);
    __disable_irq(); // disable IRQ after DMESG() again just to make sure

    if (panicMode) {
        // avoid recursive panic invocation
        while (1) {
        }
    }

    gcFreeze();

    // remember first panic code
    panicMode = true;

    drawPanic(statusCode);
    // the board may, e.g., keep watching its buttons to allow a reboot
    platform_panic(statusCode);

    while (1) {
    }
}

} // namespace _pxt_panic
