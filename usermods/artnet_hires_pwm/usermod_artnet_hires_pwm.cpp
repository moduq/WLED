#pragma once
#include "wled.h"

/*
 * ArtNet Hi-Res PWM Usermod für WLED
 * ====================================
 * Empfängt ArtNet/E1.31 und steuert bis zu 4 PWM-Kanäle
 * mit wählbarer Auflösung (8–14 Bit) und Frequenz.
 *
 * Pro Kanal: 2 aufeinanderfolgende DMX-Kanäle (Coarse + Fine = 16 Bit Eingang)
 * Die oberen (pwmBits) davon werden an ledcWrite() übergeben.
 *
 * Einstellbar in WLED UI unter "Usermods":
 *   - Pin pro Kanal          (-1 = deaktiviert)
 *   - ArtNet-Startkanal      (0-basiert, d.h. DMX ch1 = 0)
 *   - PWM-Auflösung (Bits)   (8 / 10 / 12 / 13 / 14)
 *   - PWM-Frequenz (Hz)      (z.B. 1000)
 *
 * Frequenz/Auflösungs-Richtwerte für ESP32-C3 @ 80 MHz:
 *   14 Bit →  ~4882 Hz max
 *   13 Bit →  ~9765 Hz max
 *   12 Bit →  ~19531 Hz max
 *   10 Bit →  ~78125 Hz max
 *
 * Empfehlung: 1000 Hz / 14 Bit für flimmerfreies Dimmen
 *
 * Installation:
 *   Ordner "artnet_hires_pwm" unter wled/usermods/ anlegen,
 *   diese Datei dort hinein, dann in platformio_override.ini:
 *     custom_usermods = artnet_hires_pwm
 */

#define ARTNET_HIRES_PWM_MAX_CHANNELS 4

class ArtnetHiresPWMUsermod : public Usermod {

private:
  // ── Konfiguration (alles über WLED-UI einstellbar) ─────────────────────
  int8_t   pin[ARTNET_HIRES_PWM_MAX_CHANNELS]     = {2, 3, 4, 5};
  uint16_t startCh[ARTNET_HIRES_PWM_MAX_CHANNELS] = {0, 2, 4, 6};
  uint16_t pwmFreq  = 1000;   // Hz
  uint8_t  pwmBits  = 14;     // Bit-Tiefe
  bool     enabled  = true;

  // ── Interner Zustand ────────────────────────────────────────────────────
  bool     initialised = false;
  uint32_t maxVal      = 0;   // 2^pwmBits - 1

  // LEDC-Kanal-Mapping: Usermod benutzt LEDC-Kanäle 8–11
  // (0–7 nutzt WLED intern für analoge LED-Ausgänge)
  static const uint8_t LEDC_CH_OFFSET = 8;

  // ── Hilfsfunktion: Pin freigeben ────────────────────────────────────────
  void detachPin(uint8_t ch) {
    if (pin[ch] >= 0) {
      ledcDetachPin(pin[ch]);
      pinManager.deallocatePin(pin[ch], PinOwner::UM_Unspecified);
    }
  }

  // ── Setup eines einzelnen LEDC-Kanals ──────────────────────────────────
  bool setupChannel(uint8_t ch) {
    if (pin[ch] < 0) return true; // deaktiviert, kein Fehler
    if (!pinManager.allocatePin(pin[ch], true, PinOwner::UM_Unspecified)) {
      DEBUG_PRINTF("[HiresPWM] Pin %d belegt!\n", pin[ch]);
      return false;
    }
    ledcSetup(LEDC_CH_OFFSET + ch, pwmFreq, pwmBits);
    ledcAttachPin(pin[ch], LEDC_CH_OFFSET + ch);
    ledcWrite(LEDC_CH_OFFSET + ch, 0);
    return true;
  }

  // ── Alle Kanäle neu initialisieren ─────────────────────────────────────
  void reinit() {
    // Alte Kanäle freigeben
    if (initialised) {
      for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) detachPin(i);
    }
    initialised = false;
    maxVal = (1UL << pwmBits) - 1;

    bool ok = true;
    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      if (!setupChannel(i)) ok = false;
    }
    initialised = ok;
    DEBUG_PRINTF("[HiresPWM] init %s | %u Hz | %u bit | maxVal=%u\n",
                 ok ? "OK" : "FEHLER", pwmFreq, pwmBits, (unsigned)maxVal);
  }

public:

  // ── setup() ─────────────────────────────────────────────────────────────
  void setup() override {
    reinit();
  }

  // ── loop() ──────────────────────────────────────────────────────────────
  // Der eigentliche Update läuft in handleOverlayDraw() damit er nach dem
  // ArtNet-Verarbeitungsschritt von WLED kommt.
  void loop() override { }

  // ── handleOverlayDraw() — wird nach jedem Pixel-Update aufgerufen ───────
  void handleOverlayDraw() override {
    if (!enabled || !initialised) return;

    // Nur aktiv wenn WLED im Realtime-Modus ist (ArtNet/E1.31 aktiv)
    // Bei realtimeMode == REALTIME_MODE_INACTIVE normale WLED-Kontrolle
    if (realtimeMode == REALTIME_MODE_INACTIVE) return;

    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      if (pin[i] < 0) continue;

      // Zwei ArtNet-Kanäle lesen: Coarse (MSB) + Fine (LSB)
      // realtimeBuffer[] enthält die rohen DMX-Werte (uint8_t, 0–255)
      uint16_t ch = startCh[i];
      uint8_t coarse = (ch   < MAX_LEDS * 3) ? realtimeBuffer[ch]     : 0;
      uint8_t fine   = (ch+1 < MAX_LEDS * 3) ? realtimeBuffer[ch + 1] : 0;

      // 16-Bit-Wert zusammensetzen, dann auf pwmBits skalieren
      uint32_t val16 = ((uint32_t)coarse << 8) | fine;  // 0–65535
      uint32_t pwmVal;

      if (pwmBits >= 16) {
        pwmVal = val16;
      } else if (pwmBits <= 8) {
        // Bei 8 Bit: nur Coarse verwenden
        pwmVal = coarse;
      } else {
        // 14 Bit: val16 >> 2  (16 - 14 = 2)
        pwmVal = val16 >> (16 - pwmBits);
      }

      // Clamp sicherheitshalber
      if (pwmVal > maxVal) pwmVal = maxVal;

      ledcWrite(LEDC_CH_OFFSET + i, pwmVal);
    }
  }

  // ── Konfiguration speichern ─────────────────────────────────────────────
  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject("ArtnetHiresPWM");
    top["enabled"] = enabled;
    top["freq"]    = pwmFreq;
    top["bits"]    = pwmBits;

    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      String key = "ch" + String(i);
      JsonObject ch = top.createNestedObject(key);
      ch["pin"]   = pin[i];
      ch["start"] = startCh[i];
    }
  }

  // ── Konfiguration laden ─────────────────────────────────────────────────
  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root["ArtnetHiresPWM"];
    if (top.isNull()) return false;

    bool needReinit = false;

    bool newEnabled = top["enabled"] | enabled;
    uint16_t newFreq = top["freq"] | pwmFreq;
    uint8_t  newBits = top["bits"] | pwmBits;

    if (newFreq != pwmFreq || newBits != pwmBits || newEnabled != enabled) {
      pwmFreq = newFreq;
      pwmBits = newBits;
      enabled = newEnabled;
      needReinit = true;
    }

    for (uint8_t i = 0; i < ARTNET_HIRES_PWM_MAX_CHANNELS; i++) {
      String key = "ch" + String(i);
      JsonObject ch = top[key];
      if (ch.isNull()) continue;

      int8_t  newPin   = ch["pin"]   | pin[i];
      uint16_t newStart = ch["start"] | startCh[i];

      if (newPin != pin[i] || newStart != startCh[i]) {
        pin[i]     = newPin;
        startCh[i] = newStart;
        needReinit = true;
      }
    }

    if (needReinit && initialised) reinit();
    return true;
  }

  // ── Info-Panel in WLED ──────────────────────────────────────────────────
  void addToJsonInfo(JsonObject& root) override {
    if (!enabled) return;
    JsonObject um = root["u"];
    if (um.isNull()) um = root.createNestedObject("u");

    JsonArray ar = um.createNestedArray("HiResPWM");
    if (!initialised) {
      ar.add("Fehler beim Init");
      return;
    }
    String info = String(pwmFreq) + " Hz / " + String(pwmBits) + " Bit";
    ar.add(info);
  }

  uint16_t getId() override { return 0xAB42; }
};

REGISTER_USERMOD(ArtnetHiresPWMUsermod);
