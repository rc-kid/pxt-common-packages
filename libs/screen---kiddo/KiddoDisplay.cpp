#include "KiddoDisplay.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/platform.h"

#include "ST7789_rgb16_double.pio.h"

namespace kiddo_arcade {

    namespace {

        // kidDo mk3 display pins (sdk/backend/mk3/config.h). The data lanes are reversed: DB15 is GPIO 20, DB0 is
        // GPIO 35.
        constexpr uint PIN_RDX = 11;
        constexpr uint PIN_DB15 = 20;
        constexpr uint PIN_DB7 = 28;
        constexpr uint PIN_WRX = 36;
        constexpr uint PIN_DCX = 37;
        constexpr uint PIN_CSX = 38;
        constexpr uint PIN_TE = 39;

        // the PIO's GPIO window has to start at 16 to reach GPIO 20..36, which pico-sdk only supports on the 48 GPIO
        // RP2350B (codal-pico2 uses PICO_BOARD=none, i.e. PICO_RP2350A undefined)
        constexpr uint PIO_GPIO_BASE = 16;
        static_assert(PICO_PIO_USE_GPIO_BASE, "the display needs PIO GPIO base 16 (RP2350B)");
        // display write speed, the state machine runs at 4x (KIDDO_ST7789_SPEED in the kidDo SDK)
        constexpr uint32_t DISPLAY_SPEED = 15000000;

        // ST7789 commands
        constexpr uint8_t SWRESET = 0x01;
        constexpr uint8_t SLPOUT = 0x11;
        constexpr uint8_t INVON = 0x21;
        constexpr uint8_t DISPON = 0x29;
        constexpr uint8_t CASET = 0x2a;
        constexpr uint8_t RASET = 0x2b;
        constexpr uint8_t RAMWR = 0x2c;
        constexpr uint8_t TEON = 0x35;
        constexpr uint8_t MADCTL = 0x36;
        constexpr uint8_t VSCSAD = 0x37;
        constexpr uint8_t COLMOD = 0x3a;
        constexpr uint8_t COLMOD_565 = 0x55;
        constexpr uint8_t TE_VSYNC = 0;

        uint32_t reverseBits(uint32_t value, unsigned bits) {
            uint32_t result = 0;
            for (unsigned i = 0; i < bits; ++i) {
                result = (result << 1) | (value & 1);
                value >>= 1;
            }
            return result;
        }

        uint32_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
            return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
        }

        // Bit-banged command interface, only the lower 8 data lanes (DB7..DB0) are used for commands and their
        // parameters. Timing as in the kidDo SDK (at least 66ns write cycle, 15ns per phase, generous margin).

        void writeStrobe() {
            busy_wait_at_least_cycles(24);
            gpio_put(PIN_WRX, true);
            busy_wait_at_least_cycles(24);
            gpio_put(PIN_WRX, false);
        }

        void sendByte(uint8_t b) {
            constexpr uint64_t mask = 0xffull << PIN_DB7;
            gpio_put_masked64(mask, static_cast<uint64_t>(reverseBits(b, 8)) << PIN_DB7);
            writeStrobe();
        }

        void beginCommand(uint8_t cmd) {
            gpio_put(PIN_CSX, false);
            gpio_put(PIN_DCX, false);
            sendByte(cmd);
        }

        void sendCommand(uint8_t cmd, uint8_t const * params = nullptr, unsigned size = 0) {
            beginCommand(cmd);
            gpio_put(PIN_DCX, true);
            while (size-- > 0)
                sendByte(*params++);
            gpio_put(PIN_CSX, true);
        }

        void sendCommand(uint8_t cmd, uint8_t param) {
            sendCommand(cmd, &param, 1);
        }

        void sendCommandRange(uint8_t cmd, uint16_t start, uint16_t end) {
            uint8_t params[] = {
                static_cast<uint8_t>(start >> 8), static_cast<uint8_t>(start),
                static_cast<uint8_t>(end >> 8), static_cast<uint8_t>(end)
            };
            sendCommand(cmd, params, sizeof(params));
        }

        void initializePinsBitBang() {
            for (uint pin = PIN_DB15; pin < PIN_DB15 + 16; ++pin) {
                gpio_init(pin);
                gpio_set_dir(pin, GPIO_OUT);
            }
            gpio_init(PIN_WRX);
            gpio_set_dir(PIN_WRX, GPIO_OUT);
            gpio_init(PIN_RDX);
            gpio_set_dir(PIN_RDX, GPIO_OUT);
            gpio_put(PIN_RDX, true);
            gpio_init(PIN_DCX);
            gpio_set_dir(PIN_DCX, GPIO_OUT);
            gpio_init(PIN_CSX);
            gpio_set_dir(PIN_CSX, GPIO_OUT);
            gpio_put(PIN_CSX, true);
            gpio_init(PIN_TE);
            gpio_set_dir(PIN_TE, GPIO_IN);
        }

    } // anonymous namespace

    void KiddoDisplay::init() {
        // display reset & configuration, same sequence as the kidDo SDK's ST7789::reset()
        initializePinsBitBang();
        sendCommand(SWRESET);
        busy_wait_ms(150);
        sendCommand(VSCSAD, static_cast<uint8_t>(0));
        // RGB565, column-first refresh
        sendCommand(COLMOD, COLMOD_565);
        sendCommand(MADCTL, static_cast<uint8_t>(0));
        sendCommand(TEON, TE_VSYNC);
        sendCommand(SLPOUT);
        busy_wait_ms(150);
        sendCommand(DISPON);
        busy_wait_ms(150);
        sendCommand(INVON);
        // full screen window, in native (portrait) coordinates: columns are the landscape rows and vice versa
        sendCommandRange(CASET, 0, 239);
        sendCommandRange(RASET, 0, 319);

        // the default MakeCode Arcade palette until the runtime sets its own
        static constexpr uint8_t defaultPalette[48] = {
            0x00, 0x00, 0x00,  0xff, 0xff, 0xff,  0xff, 0x21, 0x21,  0xff, 0x93, 0xc4,
            0xff, 0x81, 0x35,  0xff, 0xf6, 0x09,  0x24, 0x9c, 0xa3,  0x78, 0xdc, 0x52,
            0x00, 0x3f, 0xad,  0x87, 0xf2, 0xff,  0x8e, 0x2e, 0xc4,  0xa4, 0x83, 0x9f,
            0x5c, 0x40, 0x6c,  0xe5, 0xcd, 0xc4,  0x91, 0x46, 0x3d,  0x00, 0x00, 0x00,
        };
        setPalette(defaultPalette);

        // control blocks: columns from the right edge to the left, each twice
        for (unsigned i = 0; i < NUM_BLOCKS; ++i) {
            unsigned x = WIDTH - 1 - i / 2;
            blocks_[i] = { COLUMN_BYTES, frame_ + x * COLUMN_BYTES };
        }
        blocks_[NUM_BLOCKS] = { 0, nullptr };

        // start the RAMWR command and keep it open, the PIO then takes over the data lanes and WRX
        beginCommand(RAMWR);
        gpio_put(PIN_DCX, true);

        pio_set_gpio_base(pio_, PIO_GPIO_BASE);
        sm_ = pio_claim_unused_sm(pio_, true);
        uint offset = pio_add_program(pio_, &ST7789_rgb16_double_program);
        for (uint pin = PIN_DB15; pin < PIN_DB15 + 16; ++pin)
            pio_gpio_init(pio_, pin);
        pio_gpio_init(pio_, PIN_WRX);
        pio_sm_set_consecutive_pindirs(pio_, sm_, PIN_DB15, 16, true);
        pio_sm_set_consecutive_pindirs(pio_, sm_, PIN_WRX, 1, true);
        pio_sm_config c = ST7789_rgb16_double_program_get_default_config(offset);
        sm_config_set_out_pins(&c, PIN_DB15, 16);
        sm_config_set_sideset_pins(&c, PIN_WRX);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        // shift right (low half first), no autopull
        sm_config_set_out_shift(&c, true, false, 32);
        sm_config_set_clkdiv(&c, static_cast<float>(clock_get_hz(clk_sys)) / (DISPLAY_SPEED * 4));
        pio_sm_init(pio_, sm_, offset, &c);
        pio_sm_set_enabled(pio_, sm_, true);

        // data channel: one column (60 words) into the PIO, then chains to the control channel
        dataChannel_ = dma_claim_unused_channel(true);
        controlChannel_ = dma_claim_unused_channel(true);
        dma_channel_config dc = dma_channel_get_default_config(dataChannel_);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_read_increment(&dc, true);
        channel_config_set_write_increment(&dc, false);
        channel_config_set_dreq(&dc, pio_get_dreq(pio_, sm_, true));
        channel_config_set_chain_to(&dc, controlChannel_);
        // no interrupt per column; the terminating null block does not raise one either since we poll
        channel_config_set_irq_quiet(&dc, true);
        dma_channel_configure(dataChannel_, &dc, &pio_->txf[sm_], nullptr, 0, false);

        // control channel: writes a {count, read address} pair to the data channel's alias 3 registers, the read
        // address write triggers the data channel. The write address wraps every 8 bytes.
        dma_channel_config cc = dma_channel_get_default_config(controlChannel_);
        channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
        channel_config_set_read_increment(&cc, true);
        channel_config_set_write_increment(&cc, true);
        channel_config_set_ring(&cc, true, 3);
        dma_channel_configure(controlChannel_, &cc, &dma_hw->ch[dataChannel_].al3_transfer_count, blocks_, 2, false);

        initialized_ = true;
    }

    void KiddoDisplay::setPalette(uint8_t const * rgb888) {
        uint32_t colors[16];
        for (int i = 0; i < 16; ++i)
            colors[i] = reverseBits(rgb565(rgb888[i * 3], rgb888[i * 3 + 1], rgb888[i * 3 + 2]), 16);
        // low nibble is the upper pixel, which is sent first (low half of the word)
        for (int b = 0; b < 256; ++b)
            lut_[b] = colors[b & 0xf] | (colors[b >> 4] << 16);
    }

    bool KiddoDisplay::busy() const {
        if (!frameStarted_)
            return false;
        // the control channel has read the terminating block once the frame is through
        uintptr_t end = reinterpret_cast<uintptr_t>(blocks_ + NUM_BLOCKS + 1);
        return dma_hw->ch[controlChannel_].read_addr != end || dma_channel_is_busy(dataChannel_) ||
               dma_channel_is_busy(controlChannel_);
    }

    void KiddoDisplay::waitForSendDone() const {
        while (busy())
            tight_loop_contents();
    }

    void KiddoDisplay::sendFrame(uint8_t const * pixels) {
        if (!initialized_)
            return;
        waitForSendDone();
        for (int i = 0; i < FRAME_BYTES; ++i)
            frame_[i] = lut_[pixels[i]];
        frameStarted_ = true;
        dma_channel_set_read_addr(controlChannel_, blocks_, true);
    }

} // namespace kiddo_arcade
