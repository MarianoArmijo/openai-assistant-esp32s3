#include <driver/i2s.h>
#include <string.h>

#include "main.h"

// INMP441 input: 24kHz (OpenAI Realtime API minimum), 32-bit frame (18-bit audio MSB-aligned)
#define INPUT_SAMPLE_RATE  24000
// MAX98357A output: 24kHz (matches OpenAI Realtime API PCM16 output)
#define OUTPUT_SAMPLE_RATE 24000

// I2S DMA buffer sizes:
//  - Input: 10ms chunks (240 samples)
//  - Output: larger chunks to absorb OpenAI's bursty audio.delta frames
#define DMA_FRAME_SAMPLES_IN   240
#define DMA_FRAME_SAMPLES_OUT  1024
// 16 × 1024 × 2 bytes = 32 KB → ~683 ms playback buffer at 24 kHz
#define DMA_BUF_COUNT_OUT      16

// Software gain applied on output (shift 0 = 1×, 1 = 2× / +6dB, 2 = 4× / +12dB)
#define OUTPUT_GAIN_SHIFT      1  // ×2 (+6 dB)

// INMP441 input gain: lower shift = more gain.
// INMP441 outputs 24-bit MSB-aligned in 32-bit. Shift defines int16 mapping:
//   16 → unity (very quiet)
//   14 → +12 dB (good for close talk ~30-60 cm)
//   12 → +24 dB (good for ~1.5 m, but noise floor is louder)
//   10 → +36 dB (good for ~3 m, may clip on close shouting)
#define INPUT_SHIFT            14

// MAX98357A I2S amplifier — I2S_NUM_0 TX
#define DAC_BCLK_PIN  15
#define DAC_LRCLK_PIN 16
#define DAC_DATA_PIN   7

// INMP441 I2S microphone — I2S_NUM_1 RX
// L/R pin must be tied to GND (selects left channel)
#define ADC_BCLK_PIN   5
#define ADC_LRCLK_PIN  4
#define ADC_DATA_PIN   6

void oai_init_audio_capture() {
  // MAX98357A: 24kHz, 16-bit mono, Philips standard I2S, no MCLK
  i2s_config_t i2s_config_out = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = OUTPUT_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUF_COUNT_OUT,
      .dma_buf_len = DMA_FRAME_SAMPLES_OUT,
      .use_apll = 0,
      .tx_desc_auto_clear = true,
  };
  if (i2s_driver_install(I2S_NUM_0, &i2s_config_out, 0, NULL) != ESP_OK) {
    printf("Failed to configure I2S driver for audio output\n");
    return;
  }

  i2s_pin_config_t pin_config_out = {
      .mck_io_num   = I2S_PIN_NO_CHANGE,
      .bck_io_num   = DAC_BCLK_PIN,
      .ws_io_num    = DAC_LRCLK_PIN,
      .data_out_num = DAC_DATA_PIN,
      .data_in_num  = I2S_PIN_NO_CHANGE,
  };
  if (i2s_set_pin(I2S_NUM_0, &pin_config_out) != ESP_OK) {
    printf("Failed to set I2S pins for audio output\n");
    return;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  // INMP441: 24kHz, 32-bit frame, Philips standard I2S, mono left channel
  i2s_config_t i2s_config_in = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = INPUT_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = DMA_FRAME_SAMPLES_IN,
      .use_apll = 0,
  };
  if (i2s_driver_install(I2S_NUM_1, &i2s_config_in, 0, NULL) != ESP_OK) {
    printf("Failed to configure I2S driver for audio input\n");
    return;
  }

  i2s_pin_config_t pin_config_in = {
      .mck_io_num   = I2S_PIN_NO_CHANGE,
      .bck_io_num   = ADC_BCLK_PIN,
      .ws_io_num    = ADC_LRCLK_PIN,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num  = ADC_DATA_PIN,
  };
  if (i2s_set_pin(I2S_NUM_1, &pin_config_in) != ESP_OK) {
    printf("Failed to set I2S pins for audio input\n");
    return;
  }
}

// Read `samples` mono PCM16 samples from INMP441.
// INMP441 outputs 24-bit MSB-aligned in 32-bit frames. We shift down to int16
// range with INPUT_SHIFT (main "mic gain" knob) and saturate to avoid the
// wrap-around distortion a plain cast would produce at loud peaks.
void oai_audio_input(int16_t *buf, int samples) {
  static int32_t raw[DMA_FRAME_SAMPLES_IN];
  size_t bytes_read = 0;
  i2s_read(I2S_NUM_1, raw, samples * sizeof(int32_t), &bytes_read, portMAX_DELAY);
  int n = (int)(bytes_read / sizeof(int32_t));
  for (int i = 0; i < n; i++) {
    int32_t y = raw[i] >> INPUT_SHIFT;
    if (y > INT16_MAX) y = INT16_MAX;
    else if (y < INT16_MIN) y = INT16_MIN;
    buf[i] = (int16_t)y;
  }
}

// Write `samples` mono PCM16 samples to MAX98357A, with software gain.
void oai_audio_output(const int16_t *buf, int samples) {
  // Apply gain with hard clipping to avoid wrap-around distortion
  static int16_t boosted[2048];
  int chunk_max = (int)(sizeof(boosted) / sizeof(int16_t));

  int remaining = samples;
  const int16_t *src = buf;
  while (remaining > 0) {
    int chunk = remaining > chunk_max ? chunk_max : remaining;
    for (int i = 0; i < chunk; i++) {
      int32_t v = (int32_t)src[i] << OUTPUT_GAIN_SHIFT;
      if (v > INT16_MAX) v = INT16_MAX;
      else if (v < INT16_MIN) v = INT16_MIN;
      boosted[i] = (int16_t)v;
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, boosted, chunk * sizeof(int16_t), &written, portMAX_DELAY);
    src += chunk;
    remaining -= chunk;
  }
}
