// Sidetone: a sine tone that follows key_down, through one MAX98357A for the speaker and a second one for
// headphones. Both get the same tone at the same moment; each has its own volume.
// Blocks are 2 ms and the driver holds four of them, so the tone trails the key by a few milliseconds.
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "fn.h"

#define RATE  16000
#define BLOCK 32
static i2s_chan_handle_t tx, tx_hp;
static volatile int beep_hz, beep_blocks;

static void audio_task(void *arg) {
  float phase = 0, gain = 0;
  const float ramp = 1.0f / (0.005f * RATE);   // 5 ms fade in and out, so the tone does not click
  int16_t frames[BLOCK * 2], hp[BLOCK * 2];
  for (;;) {
    bool beeping = beep_blocks > 0;
    if (beeping) beep_blocks--;
    bool on = key_down || beeping;
    float hz = beeping ? beep_hz : settings.tone_hz;
    float level = (settings.volume / 100.0f) * (settings.volume / 100.0f) * 0.6f;   // squared: even steps to the ear
    float hp_level = (settings.hp_volume / 100.0f) * (settings.hp_volume / 100.0f) * 0.6f;
    for (int i = 0; i < BLOCK; i++) {
      gain += on ? ramp : -ramp; if (gain > 1) gain = 1; if (gain < 0) gain = 0;
      phase += 2 * (float)M_PI * hz / RATE; if (phase > 2 * (float)M_PI) phase -= 2 * (float)M_PI;
      float v = sinf(phase) * gain * 32767;
      frames[2 * i] = frames[2 * i + 1] = (int16_t)(v * level);   // same in both channels: the amplifier plays their average
      hp[2 * i] = hp[2 * i + 1] = (int16_t)(v * hp_level);
    }
    size_t written;
    i2s_channel_write(tx, frames, sizeof frames, &written, portMAX_DELAY);
    // The headphone output runs on the same clock, so it is ready when the speaker output is. It never waits
    // longer than two blocks: a fault there must not hold up the speaker.
    if (tx_hp) i2s_channel_write(tx_hp, hp, sizeof hp, &written, pdMS_TO_TICKS(4));
  }
}

void audio_beep(int hz, int ms) { beep_hz = hz; beep_blocks = ms * RATE / 1000 / BLOCK; }

void audio_start(void) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 4; chan.dma_frame_num = BLOCK; chan.auto_clear = true;
  i2s_new_channel(&chan, &tx, NULL);
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCLK, .ws = PIN_LRC, .dout = PIN_DIN, .din = I2S_GPIO_UNUSED },
  };
  i2s_channel_init_std_mode(tx, &cfg);
  i2s_channel_enable(tx);
  i2s_chan_config_t chan_hp = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_hp.dma_desc_num = 4; chan_hp.dma_frame_num = BLOCK; chan_hp.auto_clear = true;
  cfg.gpio_cfg.bclk = PIN_HP_BCLK; cfg.gpio_cfg.ws = PIN_HP_LRC; cfg.gpio_cfg.dout = PIN_HP_DIN;
  if (i2s_new_channel(&chan_hp, &tx_hp, NULL) != ESP_OK || i2s_channel_init_std_mode(tx_hp, &cfg) != ESP_OK || i2s_channel_enable(tx_hp) != ESP_OK) tx_hp = NULL;
  xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 20, NULL, 1);
}
