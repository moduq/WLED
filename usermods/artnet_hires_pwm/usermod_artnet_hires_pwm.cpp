#pragma once
#include "wled.h"

/*
 * ArtNet Hi-Res PWM Usermod für WLED
 * ====================================
 * Empfängt ArtNet/E1.31 und steuert bis zu 4 PWM-Kanäle
 * mit wählbarer Auflösung (8–14 Bit) und Frequenz.
 *
 * Pro Kanal: 2 aufeinanderfolgende "virtuelle Pixel" aus dem LED-Buffer
 * Pixel N  → Coarse (R-Kanal = MSB)
 * Pixel N+1 → Fine   (R-Kanal = LSB)
 *
 * In WLED ArtNet "Multi RGB" Modus: je 3 DMX-Kanäle = 1 Pixel (R,G,B)
 * Wir lesen nur den R-Kanal (Byte 0) jedes Pixels als 8-Bit-Wert.
 *
 * Einstellbar in WLED UI unter Usermods:
 *   - Pin pro Kanal          (-1 = deaktiviert)
 *   - Pixel-Index (Coarse)   welcher Pixel = MSB
 *   - PWM-Auflösung (Bits)   8 / 10 / 12 / 13 / 14
 *   - PWM-Frequenz (Hz)      z.B. 1000
 *
 * Frequenz/Auflösungs-Richtwerte ESP32/C3 @ 80 MHz:
 *   14 Bit →  ~4882 Hz max
 *   13 Bit →  ~9765 Hz max
 *   12 Bit →  ~19531 Hz max
 *
 * Empfehlung: 1000 Hz / 14 Bit
 */

#define ARTNET_HIRES_PWM_MAX_CHANNELS 4
// LEDC-Kanäle 8–11 (0–7 nutzt WLED intern)
#define LEDC_CH_OFFSET 8

class ArtnetHiresPWMUsermod : public Usermod {
private:
  int8_t   pin[ARTNET_HIRES_PWM_MAX_CHANNELS]       = {2, 3, 4, 5};
  uint16_t pixelIdx[ARTNET_HIRES_PWM_MAX_CHANNELS]  = {0, 2, 4, 6};
  uint16_t pwmFreq  = 1000;
  uint8_t  pwmBits  = 14;
  bool     enabled  = true;
  bool     initialised = false;
  uint32_t maxVal   = 0;

  void detachChannel(uint8_t ch) {
    if (pin[ch] >= 0) {
      ledcDetachPin(pin[ch]);
      pinManager.deallocatePin(pin[ch], PinOwner::UM_Unspecified);
    }
  }

  bool setupChannel(uint8_t ch) {
    if (pin[ch] < 0) return true;
    if (!pinManager.allocatePin(pin[ch], true, PinOwner::UM_Unspecified)) {
      DEBUG_PRINTF("[HiresPWM] Pin %d belegt!\n", pin[ch]);
      return false;
    }
    ledcSetup(LEDC_CH_OFFSET + ch, pwmFreq, pwmBits);
    ledcAttachPin(pin[ch], LEDC_CH_OFFSET + ch);
    ledcWrite(LEDC_CH_OFFSET + ch, 0);
    return true;
  }

  void reinit() {
    if (initialised) {
      for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) detachChannel(i);
    }
    initialised = false;
    maxVal = (1UL << pwmBits) - 1;
    bool ok = true;
    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      if (!setupChannel(i)) ok = false;
    }
    initialised = ok;
  }

public:
  void setup() override { reinit(); }
  void loop()  override { }

  // Wird nach jedem Pixel-Update aufgerufen
  void handleOverlayDraw() override {
    if (!enabled || !initialised) return;
    if (realtimeMode == REALTIME_MODE_INACTIVE) return;

    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      if (pin[i] < 0) continue;

      // Coarse = R-Kanal von Pixel[pixelIdx]
      // Fine   = R-Kanal von Pixel[pixelIdx+1]
      uint32_t colCoarse = strip.getPixelColor(pixelIdx[i]);
      uint32_t colFine   = strip.getPixelColor(pixelIdx[i] + 1);
      uint8_t coarse = (colCoarse >> 16) & 0xFF;  // R
      uint8_t fine   = (colFine   >> 16) & 0xFF;  // R

      uint32_t val16 = ((uint32_t)coarse << 8) | fine;
      uint32_t pwmVal;

      if (pwmBits >= 16)     pwmVal = val16;
      else if (pwmBits <= 8) pwmVal = coarse;
      else                   pwmVal = val16 >> (16 - pwmBits);

      if (pwmVal > maxVal) pwmVal = maxVal;
      ledcWrite(LEDC_CH_OFFSET + i, pwmVal);
    }
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject("ArtnetHiresPWM");
    top["enabled"] = enabled;
    top["freq"]    = pwmFreq;
    top["bits"]    = pwmBits;
    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      String key = "ch" + String(i);
      JsonObject ch = top.createNestedObject(key);
      ch["pin"] = pin[i];
      ch["pix"] = pixelIdx[i];
    }
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root["ArtnetHiresPWM"];
    if (top.isNull()) return false;
    bool changed = false;
    bool ne = top["enabled"] | enabled;
    uint16_t nf = top["freq"] | pwmFreq;
    uint8_t  nb = top["bits"] | pwmBits;
    if (ne != enabled || nf != pwmFreq || nb != pwmBits) {
      enabled = ne; pwmFreq = nf; pwmBits = nb; changed = true;
    }
    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      String key = "ch" + String(i);
      JsonObject ch = top[key];
      if (ch.isNull()) continue;
      int8_t   np = ch["pin"] | pin[i];
      uint16_t nx = ch["pix"] | pixelIdx[i];
      if (np != pin[i] || nx != pixelIdx[i]) {
        pin[i] = np; pixelIdx[i] = nx; changed = true;
      }
    }
    if (changed && initialised) reinit();
    return true;
  }

  void addToJsonInfo(JsonObject& root) override {
    if (!enabled) return;
    JsonObject um = root["u"];
    if (um.isNull()) um = root.createNestedObject("u");
    JsonArray ar = um.createNestedArray("HiResPWM");
    ar.add(String(pwmFreq) + "Hz/" + String(pwmBits) + "bit");
  }

  uint16_t getId() override { return 0xAB42; }
};

static ArtnetHiresPWMUsermod artnet_hires_pwm_instance;
REGISTER_USERMOD(artnet_hires_pwm_instance);
