#include "KiddoAudio.h"

// Sound output for the kidDo handheld: the mixer (WSynthesizer) streams to the NAU88C22 codec over I2S. The codec
// shares the I2C bus given by the PIN_SDA / PIN_SCL config with the rest of the board.

class SoundOutput {
  public:
    kiddo_arcade::KiddoAudio dac;

    SoundOutput(DataSource &data)
        : dac(*pxt::getI2C(LOOKUP_PIN(SDA), LOOKUP_PIN(SCL)), data) {
        // streaming starts with the mixer's first pullRequest()
        dac.init();
    }

    // there is a single output (headphones on kidDo mk3)
    void setOutput(int output) {}
};
