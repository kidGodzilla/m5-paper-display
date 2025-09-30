# M5Paper Meeting Room Display (Portrait)

## Overview

This is a minimal, modern-looking meeting room display for the M5Paper e‑ink device. It renders a portrait UI with:

- Meeting Room Name
- Current Meeting Block
- Next Meeting Block
- Actions

By default, it uses light sleep so touch can wake the device at any time; periodic refresh occurs on a timer. You can switch strategies in `Config.h`.

Hardware

- Device: M5Stack M5Paper (ESP32 + 960×540 e‑ink)

https://shop.m5stack.com/products/m5papers3-esp32s3-development-kit

This is also available in the US through Mouser.

Dependencies

- Arduino IDE or PlatformIO
- ESP32 board support (Arduino Boards Manager: "esp32 by Espressif Systems")
- M5EPD library (Library Manager: search for "M5EPD")

Build & Flash (Arduino IDE)

1. Install ESP32 support and the M5EPD library.
2. Open `m5-paper-display.ino`.
3. Board: "M5Stack-Core2" or any ESP32; for M5Paper use `ESP32 Dev Module` with correct settings if a dedicated profile is unavailable.
4. Select the correct Serial Port for your device.
5. Upload.

Behavior

- On boot, the device loads optional SD config and TTF fonts, draws the UI in portrait orientation, and performs a high‑quality refresh.
- It then enters light sleep and wakes either on touch (TP INT) or a periodic timer (`refresh_seconds`).

Configuration & Customization

Project-level config lives in `Config.h`:

- `WIFI_SSID`, `WIFI_PW`: Wi‑Fi credentials
- `DISPLAY_KEY`: Meeting Room 365 display key
- `SCHEDULE_API_BASE`: API base (default `https://api.meetingroom365.com`)
- `REFRESH_SECONDS`: periodic refresh (seconds)
- Font paths: `FONT_REGULAR_PATH`, `FONT_BOLD_PATH` (TTF on SD root)
- UI: `SCREEN_ROTATION`, `AVAILABLE_BG_SHADE`, `OCCUPIED_BG_SHADE`, `BUTTON_FILL_SHADE`, `BUTTON_PRESSED_SHADE`, `TEXT_COLOR_SHADE`
- Touch wake pin: `TOUCH_INT_PIN`

Optional SD overrides via `/config.json` on the microSD card (see `sd_card_example/config.example.json`):

```json
{
  "wifi_ssid": "YourWiFiSSID",
  "wifi_password": "YourWiFiPassword",
  "display_key": "yourdisplaykeyhere",
  "refresh_seconds": 300
}
```

Place TTF fonts on the microSD root as `FreeSans.ttf` and `FreeSansBold.ttf` (or update the font paths in `Config.h`). If fonts are missing, the sketch falls back to built‑in fonts.

Actions & Touch

- Bottom action bars support Reserve, Extend, and End Early based on server flags. Taps wake the device, show pressed feedback, send `?action=` to the API, redraw, and return to sleep.

Examples

- See `examples/` for prior implementations and references.


