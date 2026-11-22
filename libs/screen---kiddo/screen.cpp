#include "pxt.h"

#include "KiddoDisplay.h"

// MakeCode Arcade screen for the kidDo handheld: adapts the KiddoDisplay driver (ST7789 on a 16-bit parallel bus,
// MakeCode's 160x120 image doubled to 320x240) to the screen API used by the screen library.

// The backlight is controlled by the board (on kidDo mk3 through the IO MCU), the board package provides the
// implementation. Brightness in percent, 0 .. 100.
extern "C" __attribute__((weak)) void kiddo_set_backlight(int percent) {}

namespace pxt {

// statically allocated, the driver holds the converted frame (~41 KB)
static kiddo_arcade::KiddoDisplay kiddoDisplay;
static bool kiddoDisplayInitialized = false;

kiddo_arcade::KiddoDisplay &getKiddoDisplay() {
    if (!kiddoDisplayInitialized) {
        kiddoDisplayInitialized = true;
        kiddoDisplay.init();
    }
    return kiddoDisplay;
}

//% expose
int setScreenBrightnessSupported() {
    return 1;
}

//% expose
void setScreenBrightness(int level) {
    kiddo_set_backlight(level);
}

//% expose
void setPalette(Buffer buf) {
    if (48 != buf->length)
        target_panic(PANIC_SCREEN_ERROR);
    getKiddoDisplay().setPalette(buf->data);
}

//% expose
void updateScreen(Image_ img) {
    if (!img)
        return;
    if (img->bpp() != 4 || img->width() != kiddo_arcade::KiddoDisplay::WIDTH ||
        img->height() != kiddo_arcade::KiddoDisplay::HEIGHT)
        target_panic(PANIC_SCREEN_ERROR);
    // converts the image into the driver's own buffer, so the game can keep drawing right away
    getKiddoDisplay().sendFrame(img->pix());
}

//% expose
void updateStats(String msg) {
    DMESG("stats: %s", msg->getUTF8Data());
}

} // namespace pxt
