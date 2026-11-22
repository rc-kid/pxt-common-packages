#include "KiddoAudio.h"

#include <cstring>

#include "ErrorNo.h"
#include "dma.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

#include "i2s_mclk.pio.h"
#include "i2s_out16.pio.h"

namespace kiddo_arcade {

    namespace {

        // kidDo mk3 audio pins (sdk/backend/mk3/config.h)
        constexpr uint PIN_I2S_MCLK = 4;
        constexpr uint PIN_I2S_DAC = 5;
        constexpr uint PIN_I2S_BCLK = 7; // LRCK is BCLK + 1 (GPIO 8), both driven by the side set

        // NAU88C22 at I2C address 0x1a, CODAL uses 8-bit (shifted) addresses
        constexpr uint16_t CODEC_ADDRESS = 0x1a << 1;

        // NAU88C22 registers & bits used (names as in sdk/backend/mk3/audio/codec.h)
        constexpr uint8_t REG_RESET = 0;
        constexpr uint8_t REG_PWR_MGMT_1 = 1;
        constexpr uint16_t REF_IMP_80K = 0x01;
        constexpr uint16_t IOBUFEN = 4;
        constexpr uint16_t ABIASEN = 8;
        constexpr uint8_t REG_PWR_MGMT_2 = 2;
        constexpr uint16_t RHPEN = 256;
        constexpr uint16_t LHPEN = 128;
        constexpr uint8_t REG_PWR_MGMT_3 = 3;
        constexpr uint16_t RMIXEN = 8;
        constexpr uint16_t LMIXEN = 4;
        constexpr uint16_t RDACEN = 2;
        constexpr uint16_t LDACEN = 1;
        constexpr uint8_t REG_AUDIO_INTERFACE = 4;
        constexpr uint16_t WLEN_16 = 0;
        constexpr uint16_t AIFMT_I2S = 0b10000;
        constexpr uint8_t REG_CLK_CTRL_1 = 6;
        constexpr uint16_t CLKM_MCLK = 0; // MCLK directly, no PLL, no divider, slave mode
        constexpr uint8_t REG_JACK_DETECT_1 = 9;
        constexpr uint8_t REG_OUTPUT_CONTROL = 49;
        constexpr uint16_t TSEN = 2;
        constexpr uint8_t REG_LMIXER = 50;
        constexpr uint8_t REG_RMIXER = 51;
        constexpr uint16_t MIXER_DAC = 0b10101; // DAC to mixer (bypass gain bits at their 0dB default)
        constexpr uint8_t REG_LHP_VOLUME = 52;
        constexpr uint8_t REG_RHP_VOLUME = 53;
        constexpr uint16_t HEADPHONE_MUTE = 64;
        constexpr uint8_t REG_LSPKOUT_VOLUME = 54;
        constexpr uint8_t REG_RSPKOUT_VOLUME = 55;
        constexpr uint16_t SPEAKER_MUTE = 64;
        constexpr uint16_t UPDATE_BIT = 256;
        constexpr uint8_t REG_DEV_ID = 63;
        constexpr uint16_t DEV_ID = 0x1a;

        // I2S: every channel slot is 17 BCLKs (one-bit delay + 16 bits), 2 instructions per BCLK
        constexpr uint32_t I2S_CYCLES_PER_FRAME = 34 * 2;
        // MCLK must be 256 * sample rate, 2 instructions per MCLK period
        constexpr uint32_t MCLK_CYCLES_PER_FRAME = 256 * 2;

        void dmaCallback0(void * self) { static_cast<KiddoAudio *>(self)->refill(0); }
        void dmaCallback1(void * self) { static_cast<KiddoAudio *>(self)->refill(1); }

        float clockDivider(uint32_t hz) {
            return static_cast<float>(clock_get_hz(clk_sys)) / hz;
        }

    } // anonymous namespace

    KiddoAudio * KiddoAudio::instance_ = nullptr;

    KiddoAudio::KiddoAudio(codal::I2C & i2c, codal::DataSource & source, int sampleRate):
        i2c_{i2c}, source_{source}, sampleRate_{sampleRate} {
        memset(buffer_, 0, sizeof(buffer_));
        instance_ = this;
        source_.connect(*this);
    }

    bool KiddoAudio::init() {
        uint16_t id = 0;
        bool found = getRegister(REG_DEV_ID, id) && (id & 0x1ff) == DEV_ID;

        // reset & power up, as the kidDo SDK's Codec::reset() and Codec::powerUp() (datasheet page 20, power up
        // guidance, < 3.6V operation), but without the speaker
        setRegister(REG_RESET, 0xff);
        busy_wait_ms(10);
        // thermal shutdown on, no boosts
        setRegister(REG_OUTPUT_CONTROL, TSEN);
        // tie-off buffer, then let the reference voltage charge slowly through the internal 80k resistors
        setRegister(REG_PWR_MGMT_1, IOBUFEN);
        setRegister(REG_PWR_MGMT_1, REF_IMP_80K | IOBUFEN | ABIASEN);
        busy_wait_ms(250);
        setRegister(REG_CLK_CTRL_1, CLKM_MCLK);
        setRegister(REG_AUDIO_INTERFACE, WLEN_16 | AIFMT_I2S);

        // mk3 hardware bug: headphone detection does not work, so no jack detection and no automatic switching. The
        // output is hardwired to the headphones, the speaker drivers stay off and muted.
        setRegister(REG_JACK_DETECT_1, 0);
        setRegister(REG_LSPKOUT_VOLUME, SPEAKER_MUTE);
        setRegister(REG_RSPKOUT_VOLUME, SPEAKER_MUTE | UPDATE_BIT);

        // I2S playback (Codec::playbackI2S() without RSPKEN | LSPKEN): headphone drivers, DAC & mixers, DAC to mixers
        setRegister(REG_PWR_MGMT_2, RHPEN | LHPEN);
        setRegister(REG_PWR_MGMT_3, RMIXEN | LMIXEN | RDACEN | LDACEN);
        setRegister(REG_LMIXER, MIXER_DAC);
        setRegister(REG_RMIXER, MIXER_DAC);
        setVolume(volume_);

        startMasterClock();
        dmaChannel_[0] = dma_claim_unused_channel(true);
        dmaChannel_[1] = dma_claim_unused_channel(true);
        startI2S();
        return found;
    }

    void KiddoAudio::start() {
        if (streaming_)
            return;
        streaming_ = true;
        // two DMA channels in ping-pong, both start with silence, then refill from the source as they complete
        fill(0, false);
        fill(1, false);
        pio_sm_set_enabled(pio_, i2sSm_, true);
        dma_channel_start(dmaChannel_[0]);
    }

    int KiddoAudio::pullRequest() {
        start();
        return DEVICE_OK;
    }

    void KiddoAudio::setVolume(uint8_t volume) {
        if (volume > MAX_VOLUME)
            volume = MAX_VOLUME;
        volume_ = volume;
        uint16_t value = (volume == 0) ? HEADPHONE_MUTE : volume;
        setRegister(REG_LHP_VOLUME, value);
        setRegister(REG_RHP_VOLUME, value | UPDATE_BIT);
    }

    void KiddoAudio::fill(int index, bool pull) {
        // channel `index` has finished (or is being set up): fill its buffer with the next block from the source
        uint32_t * buffer = buffer_[index];
        codal::ManagedBuffer data;
        if (pull)
            data = source_.pull();
        unsigned n = data.length() / 2;
        if (n > BLOCK_SAMPLES)
            n = BLOCK_SAMPLES;
        uint16_t const * samples = reinterpret_cast<uint16_t const *>(data.getBytes());
        for (unsigned i = 0; i < n; ++i) {
            // 10-bit unsigned (offset 512) -> signed 16-bit, same value for both channels
            uint16_t s = static_cast<uint16_t>((static_cast<int>(samples[i]) - 512) << 6);
            buffer[i] = (static_cast<uint32_t>(s) << 16) | s;
        }
        // empty or short block: silence
        for (unsigned i = n; i < BLOCK_SAMPLES; ++i)
            buffer[i] = 0;

        // re-arm the channel, it is started by the other one when that finishes
        uint ch = dmaChannel_[index];
        dma_channel_config c = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(pio_, i2sSm_, true));
        channel_config_set_chain_to(&c, dmaChannel_[1 - index]);
        dma_channel_configure(ch, &c, &pio_->txf[i2sSm_], buffer, BLOCK_SAMPLES, false);
        // at 44.1kHz a block is ~5.8ms, low priority is enough. CODAL disables the channel's IRQ before calling us, so
        // the callback is registered (and the IRQ re-enabled) every time.
        DMA_SetChannelCallback_lowpri(ch, index == 0 ? dmaCallback0 : dmaCallback1, this);
    }

    void KiddoAudio::startMasterClock() {
        pio_set_gpio_base(pio_, 0);
        mclkSm_ = pio_claim_unused_sm(pio_, true);
        uint offset = pio_add_program(pio_, &i2s_mclk_program);
        pio_gpio_init(pio_, PIN_I2S_MCLK);
        pio_sm_set_consecutive_pindirs(pio_, mclkSm_, PIN_I2S_MCLK, 1, true);
        pio_sm_config c = i2s_mclk_program_get_default_config(offset);
        sm_config_set_sideset_pins(&c, PIN_I2S_MCLK);
        sm_config_set_clkdiv(&c, clockDivider(sampleRate_ * MCLK_CYCLES_PER_FRAME));
        pio_sm_init(pio_, mclkSm_, offset, &c);
        pio_sm_set_enabled(pio_, mclkSm_, true);
    }

    void KiddoAudio::startI2S() {
        i2sSm_ = pio_claim_unused_sm(pio_, true);
        uint offset = pio_add_program(pio_, &i2s_out16_program);
        pio_gpio_init(pio_, PIN_I2S_DAC);
        pio_gpio_init(pio_, PIN_I2S_BCLK);
        pio_gpio_init(pio_, PIN_I2S_BCLK + 1);
        pio_sm_set_consecutive_pindirs(pio_, i2sSm_, PIN_I2S_DAC, 1, true);
        pio_sm_set_consecutive_pindirs(pio_, i2sSm_, PIN_I2S_BCLK, 2, true);
        pio_sm_config c = i2s_out16_program_get_default_config(offset);
        sm_config_set_out_pins(&c, PIN_I2S_DAC, 1);
        sm_config_set_sideset_pins(&c, PIN_I2S_BCLK);
        sm_config_set_clkdiv(&c, clockDivider(sampleRate_ * I2S_CYCLES_PER_FRAME));
        pio_sm_init(pio_, i2sSm_, offset, &c);
        // enabled by start() once the first blocks are queued
    }

    void KiddoAudio::setRegister(uint8_t reg, uint16_t value) {
        // 7-bit register address, 9-bit value: [reg << 1 | value bit 8, value bits 7..0]
        uint8_t cmd[] = {
            static_cast<uint8_t>((reg << 1) | ((value >> 8) & 0x01)),
            static_cast<uint8_t>(value & 0xff)
        };
        i2c_.write(CODEC_ADDRESS, cmd, sizeof(cmd));
    }

    bool KiddoAudio::getRegister(uint8_t reg, uint16_t & value) {
        uint8_t result[2];
        if (i2c_.readRegister(CODEC_ADDRESS, static_cast<uint8_t>(reg << 1), result, 2) != DEVICE_OK)
            return false;
        value = (result[0] << 8) | result[1];
        return true;
    }

} // namespace kiddo_arcade
