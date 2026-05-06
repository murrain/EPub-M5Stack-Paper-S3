#define __BATTERY_PAPER_S3__ 1
#include "battery_paper_s3.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "logging.hpp"

Battery Battery::singleton;

namespace
{
  constexpr adc_unit_t      BAT_ADC_UNIT      = ADC_UNIT_1;
  constexpr adc_channel_t   BAT_ADC_CHANNEL   = ADC_CHANNEL_2;   // GPIO 3 -> ADC1_CH2
  constexpr adc_atten_t     BAT_ADC_ATTEN     = ADC_ATTEN_DB_12; // ~0..3.3V at the pin
  constexpr adc_bitwidth_t  BAT_ADC_BITWIDTH  = ADC_BITWIDTH_DEFAULT;
  constexpr gpio_num_t      USB_DET_GPIO      = GPIO_NUM_5;

  // Best-effort assumption for the on-board divider. Tune empirically:
  // measure the actual battery with a multimeter and divide by the
  // reported read_level() to get the correct ratio.
  constexpr float VOLTAGE_DIVIDER_RATIO = 2.0f;

  // Number of ADC samples averaged per read_level() call.
  constexpr int   READ_SAMPLES = 8;

  adc_oneshot_unit_handle_t adc_handle = nullptr;
  adc_cali_handle_t         cali_handle = nullptr;

  static constexpr char const * TAG = "BatteryPaperS3";
}

bool Battery::setup()
{
  if (initialized) return true;

  adc_oneshot_unit_init_cfg_t init_cfg = {};
  init_cfg.unit_id  = BAT_ADC_UNIT;
  init_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;

  esp_err_t err = adc_oneshot_new_unit(&init_cfg, &adc_handle);
  if (err != ESP_OK) {
    LOG_E("Battery: adc_oneshot_new_unit failed (%s)", esp_err_to_name(err));
    return false;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten    = BAT_ADC_ATTEN;
  chan_cfg.bitwidth = BAT_ADC_BITWIDTH;

  err = adc_oneshot_config_channel(adc_handle, BAT_ADC_CHANNEL, &chan_cfg);
  if (err != ESP_OK) {
    LOG_E("Battery: adc_oneshot_config_channel failed (%s)", esp_err_to_name(err));
    // Release the ADC unit handle we already own so a future retry of
    // setup() doesn't fail with INVALID_STATE on the unit re-claim.
    adc_oneshot_del_unit(adc_handle);
    adc_handle = nullptr;
    return false;
  }

  // Try eFuse curve-fitting calibration first; fall back to raw scale on failure.
  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id  = BAT_ADC_UNIT;
  cali_cfg.chan     = BAT_ADC_CHANNEL;
  cali_cfg.atten    = BAT_ADC_ATTEN;
  cali_cfg.bitwidth = BAT_ADC_BITWIDTH;
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) != ESP_OK) {
    LOG_I("Battery: ADC calibration unavailable; using raw->mV approximation");
    cali_handle = nullptr;
  }

  // USB_DET is a GPIO input; no internal pull (the line is driven by the
  // 5V rail through a divider on the schematic).
  gpio_config_t io_cfg = {};
  io_cfg.pin_bit_mask = 1ULL << USB_DET_GPIO;
  io_cfg.mode         = GPIO_MODE_INPUT;
  io_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
  io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_cfg.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&io_cfg);

  initialized = true;
  LOG_I("Battery: ADC1_CH2 (GPIO 3) ready, divider=%.2f, calib=%s",
        VOLTAGE_DIVIDER_RATIO, cali_handle ? "yes" : "no");
  return true;
}

float Battery::read_level()
{
  if (!initialized) return 0.0f;

  int sum_mv = 0;
  int count  = 0;
  for (int i = 0; i < READ_SAMPLES; ++i) {
    int raw;
    if (adc_oneshot_read(adc_handle, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
    int mv;
    if (cali_handle != nullptr) {
      if (adc_cali_raw_to_voltage(cali_handle, raw, &mv) != ESP_OK) continue;
    } else {
      // 12-bit ADC, 12dB attenuation -> ~3.3V full scale (uncalibrated).
      mv = (raw * 3300) / 4095;
    }
    sum_mv += mv;
    count++;
  }

  if (count == 0) return 0.0f;
  float avg_pin_v = (float)sum_mv / count / 1000.0f;
  return avg_pin_v * VOLTAGE_DIVIDER_RATIO;
}

int Battery::read_percentage()
{
  float v = read_level();
  // Cheap linear map across a Li-ion's usable band. The discharge curve
  // is non-linear and a real fuel-gauge would track coulombs, but we
  // don't have a fuel gauge IC on PaperS3 — this is just for the icon.
  constexpr float V_EMPTY = 3.30f;
  constexpr float V_FULL  = 4.15f;
  if (v <= 0.0f) return 0;
  int pct = (int)((v - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

bool Battery::is_usb_powered()
{
  if (!initialized) return false;
  return gpio_get_level(USB_DET_GPIO) != 0;
}

#endif // BOARD_TYPE_PAPER_S3
