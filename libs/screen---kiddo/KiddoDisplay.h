/** MakeCode Arcade screen driver for the kidDo mk3 display (ST7789, 320x240, 16-bit MCU interface on PIO).

    Ported from the kidDo SDK (sdk/backend/mk3/screen/ST7789.*) to plain pico-sdk 2.x so that it runs under CODAL.
    MIT licensed, see the kidDo repository.

    The driver takes frames in MakeCode Arcade's native screen format, i.e. a 160x120 image with 4 bits per pixel,
    stored column by column (60 bytes per column, low nibble = upper pixel), and a 16 entry RGB888 palette. Each frame
    is converted through a 256 entry lookup table (one image byte = two pixels -> one 32-bit word with two RGB565
    pixels) and streamed to the display with DMA:

    - the display is in column-first mode (MADCTL 0, the kidDo SDK's default), so consecutive pixels fill a landscape
      column of 240 pixels top to bottom, columns going from the right edge (x = 319) to the left (x = 0)
    - the PIO program writes every pixel twice (vertical doubling)
    - every converted column is sent twice (horizontal doubling)

    The column order is described by a fixed list of DMA control blocks, which a second DMA channel feeds to the data
    channel, so a whole frame is sent without any interrupts.

    The display stays in the RAMWR (memory write) command forever, every frame is exactly 320 * 240 pixels so the
    display's write pointer wraps back to the start of the window after each frame.
 */
#pragma once

#include <cstdint>

#include "hardware/pio.h"

namespace kiddo_arcade {

    class KiddoDisplay {
    public:

        /** MakeCode Arcade screen size. */
        static constexpr int WIDTH = 160;
        static constexpr int HEIGHT = 120;
        /** Bytes per image column (4 bits per pixel). */
        static constexpr int COLUMN_BYTES = HEIGHT / 2;
        /** Size of the image data passed to sendFrame(). */
        static constexpr int FRAME_BYTES = WIDTH * COLUMN_BYTES;

        /** Resets and initializes the display, then claims a PIO state machine and two DMA channels and enters the
            frame streaming mode. Busy waits for about half a second (display reset). The backlight is controlled by
            the IO MCU and is not touched here.
         */
        void init();

        /** Sets the palette from 16 RGB888 colors (48 bytes, r, g, b), as passed to MakeCode's setPalette(). Takes
            effect with the next frame.
         */
        void setPalette(uint8_t const * rgb888);

        /** Sends a frame in MakeCode Arcade's image format (FRAME_BYTES bytes). Waits for the previous frame to finish,
            converts the image (so the caller may modify it as soon as this returns) and starts the DMA transfer.
         */
        void sendFrame(uint8_t const * pixels);

        /** True while a frame is being transferred. */
        bool busy() const;

        /** Waits until the frame transfer, if any, is finished. */
        void waitForSendDone() const;

    private:

        static constexpr unsigned NUM_BLOCKS = WIDTH * 2;

        struct ControlBlock {
            uint32_t count;
            uint32_t const * readAddr;
        };

        PIO pio_ = pio0;
        uint sm_ = 0;
        uint dataChannel_ = 0;
        uint controlChannel_ = 0;
        bool initialized_ = false;
        bool frameStarted_ = false;

        // pairs of pixels, already bit-reversed for the data lanes
        uint32_t lut_[256];
        // converted frame, one word per image byte, same column-major order as the image
        uint32_t frame_[FRAME_BYTES];
        // the order in which the columns are sent, plus a terminating null block
        ControlBlock blocks_[NUM_BLOCKS + 1];

    }; // kiddo_arcade::KiddoDisplay

} // namespace kiddo_arcade
