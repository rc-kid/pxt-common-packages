/** MakeCode Arcade sound output for kidDo mk3: I2S to the NAU88C22 audio codec, under CODAL.

    Ported from the kidDo SDK (sdk/backend/mk3/audio/codec.h, the audio part of sdk/backend/mk3/mk3.cpp) to plain
    CODAL / pico-sdk 2.x. MIT licensed, see the kidDo repository.

    The class is a CODAL DataSink built like codal-rp2040's RP2040PWM (the RP2040 Arcade sound output): two DMA channels
    in ping-pong feed the I2S PIO program and every time one of them finishes, the next block is pulled from the
    upstream DataSource. The expected sample format is MakeCode's mixer output (pxt-common-packages libs/mixer,
    WSynthesizer::pull()): blocks of 256 mono samples, 16 bits each, 10-bit unsigned (offset 512). Samples are
    converted to signed 16-bit and duplicated to both channels. An empty buffer means silence.

    Resources: pio1 (2 state machines: MCLK and I2S, GPIO base 0 since the I2S pins are GPIO 4..8), 2 DMA channels
    with CODAL's low priority DMA IRQ (DMA_SetChannelCallback_lowpri), the codec on the shared I2C bus (address 0x1a).

    mk3 headphone detection does not work (hardware bug), so the output is hardwired to the headphones and the speaker
    is kept off: the codec's jack detection is disabled and the speaker drivers are never powered.
 */
#pragma once

#include <cstdint>

#include "DataStream.h"
#include "I2C.h"
#include "hardware/pio.h"

namespace kiddo_arcade {

    class KiddoAudio : public codal::DataSink {
    public:

        /** MakeCode's RP2040 sound output runs at 44kHz, the mixer adapts to whatever getSampleRate() says. */
        static constexpr int DEFAULT_SAMPLE_RATE = 44100;

        /** Samples per block (MakeCode mixer block size). */
        static constexpr unsigned BLOCK_SAMPLES = 256;

        /** Headphone volume range of the codec, 0 = mute, 63 = +6dB, 57 = 0dB, 1 dB per step. */
        static constexpr uint8_t MAX_VOLUME = 63;

        KiddoAudio(codal::I2C & i2c, codal::DataSource & source, int sampleRate = DEFAULT_SAMPLE_RATE);

        /** Resets and powers up the codec (busy waits ~0.3s) and sets up the PIO and DMA channels. Does not pull from
            the source yet (MakeCode's mixer constructs its sound output from its own constructor). Returns false if the
            codec did not respond with its device id (everything is set up anyway).
         */
        bool init();

        /** Starts streaming, pulling from the source from now on (silence while it returns empty buffers). Called on
            the first pullRequest(), or explicitly. Does nothing if already streaming.
         */
        void start();

        int getSampleRate() const { return sampleRate_; }

        /** Headphone volume, 0 (mute) .. MAX_VOLUME. */
        void setVolume(uint8_t volume);
        uint8_t volume() const { return volume_; }

        /** DataSink: the source has data. Starts streaming if not yet started; after that we pull at our own pace. */
        int pullRequest() override;

        /** Called from the DMA IRQ when channel `index` has finished its block. */
        void refill(int index) { fill(index, true); }

        /** The instance (there is a single audio output), or nullptr. */
        static KiddoAudio * instance() { return instance_; }

    private:

        void fill(int index, bool pull);
        void setRegister(uint8_t reg, uint16_t value);
        bool getRegister(uint8_t reg, uint16_t & value);
        void startMasterClock();
        void startI2S();

        codal::I2C & i2c_;
        codal::DataSource & source_;
        int sampleRate_;
        uint8_t volume_ = 40;
        bool streaming_ = false;
        static KiddoAudio * instance_;

        PIO pio_ = pio1;
        uint mclkSm_ = 0;
        uint i2sSm_ = 0;
        uint dmaChannel_[2] = { 0, 0 };
        // stereo frames, upper half first channel, lower half second channel
        uint32_t buffer_[2][BLOCK_SAMPLES];

    }; // kiddo_arcade::KiddoAudio

} // namespace kiddo_arcade
