// Battery monitor for M5Stack PaperS3 (ADC_VBAT on GPIO 3, ADC1 channel 2).
//
// The PaperS3 schematic routes the LiPo cell through a passive voltage
// divider into G3 (ADC1_CH2). The exact divider ratio is not published
// in the M5 docs but the standard M5 pattern is 1:2 (VBAT/2 reaches the
// ADC pin), giving headroom up to ~6.6V at the chosen attenuation.
// Calibrate VOLTAGE_DIVIDER_RATIO against a multimeter on first flash
// if the reported voltage is off.
//
// USB charging detection uses USB_DET on GPIO 5 (high when USB power
// is present). The PaperS3 has no battery-current measurement (per
// M5 docs), so charge state is "USB present" not "actually charging".

#pragma once
#include "global.hpp"

#if defined(BOARD_TYPE_PAPER_S3)

#include "non_copyable.hpp"

class Battery : NonCopyable
{
  public:
    bool setup();
    float read_level();      ///< Battery voltage (V). Returns 0.0 on failure.
    int read_percentage();   ///< 0..100 percentage estimate.
    bool is_usb_powered();   ///< USB cable detected (G5 high).

    static Battery & get_singleton() noexcept { return singleton; }

  private:
    static Battery singleton;
    Battery() = default;

    bool initialized = false;
};

#if __BATTERY_PAPER_S3__
  Battery & battery = Battery::get_singleton();
#else
  extern Battery & battery;
#endif

#endif // BOARD_TYPE_PAPER_S3
