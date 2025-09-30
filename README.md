# M5Paper Meeting Room Display

## Overview

A minimal, modern-looking meeting room display for the M5Paper e‑ink device. Features a clean, text-first portrait UI with:

- Optional Company/Room Logo
- Meeting Room Name
- Current Meeting Block (with word-wrapping for long titles)
- Next Meeting Block
- Touch-enabled Action Buttons (Reserve, Extend, End Early)
- Battery Indicator
- Debug Clock (optional)

By default, it uses light sleep during business hours so touch can wake the device at any time. Periodic refresh occurs on a timer. Outside of business hours, the device automatically enters deep sleep to conserve battery.

## Hardware

- **Device**: M5Stack M5Paper (ESP32 + 960×540 e‑ink display)
- **MicroSD Card**: Required for custom fonts and logo (optional for basic operation)


https://shop.m5stack.com/products/m5papers3-esp32s3-development-kit

This is also available in the US through Mouser.


## Dependencies

- Arduino IDE or PlatformIO
- ESP32 board support (Arduino Boards Manager: "esp32 by Espressif Systems")
- M5EPD library (Library Manager: search for "M5EPD")

## Build & Flash (Arduino IDE)

1. Install ESP32 support and the M5EPD library.
2. Open `m5-paper-display.ino`.
3. **Board Settings**:
   - Board: "M5Stack-Core2" or "ESP32 Dev Module"
   - CPU Frequency: **160MHz** (recommended for battery savings)
   - Upload Speed: 115200 or higher
4. Select the correct Serial Port for your device.
5. Upload.

## Behavior

### Normal Operation (During Business Hours)
- On boot, the device loads optional SD config, TTF fonts, and logo
- Fetches schedule from Meeting Room 365 API
- Draws the UI in portrait orientation with high‑quality grayscale refresh
- Enters **light sleep** and wakes on:
  - **Touch** (TP INT): User taps action button
  - **Timer**: Periodic refresh (default: 300 seconds / 5 minutes)
- Action buttons (Reserve, Extend, End Early) provide immediate visual feedback and update the schedule

### Power Management (Outside Business Hours)
- **Deep Sleep**: Automatically engages outside configured business hours
- **Default Schedule**: Active 8 AM - 7 PM weekdays, deep sleep nights and weekends
- **Wake Time**: Automatically calculates and wakes at the start of next business day
- **Battery Savings**: Deep sleep reduces power consumption from ~3-32mA to ~1-10mA
- **Customizable**: Set your own business hours or disable entirely for 24/7 operation

## Configuration

### Config.h (Compile-time Defaults)

Project-level config lives in `Config.h`:

**Network & API:**
- `WIFI_SSID`, `WIFI_PW`: Wi‑Fi credentials
- `DISPLAY_KEY`: Meeting Room 365 display key
- `SCHEDULE_API_BASE`: API base URL (default: `https://api.meetingroom365.com`)
- `REFRESH_SECONDS`: Periodic refresh interval in seconds (default: 300 = 5 minutes)

**Business Hours (Power Management):**
- `ENABLE_BUSINESS_HOURS`: `1` to enable, `0` for 24/7 operation (default: 1)
- `BUSINESS_HOURS_START`: Hour when display becomes active, 0-23 (default: 8 = 8 AM)
- `BUSINESS_HOURS_END`: Hour when display sleeps, 0-23 (default: 19 = 7 PM)
- `DEEP_SLEEP_WEEKENDS`: `1` to sleep Sat/Sun, `0` to stay active (default: 1)

**Fonts & Logo:**
- `FONT_REGULAR_PATH`: Path to TTF regular font on SD (default: `/FreeSans.ttf`)
- `FONT_BOLD_PATH`: Path to TTF bold font on SD (default: `/FreeSansBold.ttf`)
- Logo: Place `/logo.png` or `/logo.jpg` on SD card (max 60px height, optional)

**Display Options:**
- `SHOW_BATTERY`: `1` to show battery indicator, `0` to hide (default: 1)
- `SHOW_DEBUG_CLOCK`: `1` to show clock in top-left, `0` to hide (default: 0)
- `TWENTYFOUR_HOUR`: `1` for 24-hour time, `0` for 12-hour AM/PM (default: 1)
- `DEBUG_LOGGING`: `1` for verbose serial logs, `0` for errors only (default: 1)

**UI Appearance:**
- `SCREEN_ROTATION`: Display rotation (default: 90 for portrait)
- `AVAILABLE_BG_SHADE`, `OCCUPIED_BG_SHADE`: Background colors (0-15, 0=white)
- `BUTTON_FILL_SHADE`, `BUTTON_PRESSED_SHADE`: Button colors
- `TEXT_COLOR_SHADE`: Text color (default: 14 = dark)

### config.json (Runtime Overrides - Recommended)

Optional SD overrides via `/config.json` on the microSD card (see `sd_card_example/config.example.json`):

```json
{
  "wifi_ssid": "YourWiFiSSID",
  "wifi_password": "YourWiFiPassword",
  "display_key": "yourdisplaykeyhere",
  "refresh_seconds": 300,
  "enable_business_hours": 1,
  "business_hours_start": 8,
  "business_hours_end": 19,
  "deep_sleep_weekends": 1
}
```

**Why use config.json?**
- Change settings without recompiling/reflashing
- Deploy same firmware to multiple displays with different configs
- Easy to update Wi-Fi credentials or business hours

### SD Card Files (All Optional)

| File | Purpose | Required? |
|------|---------|----------|
| `/config.json` | Runtime configuration overrides | Optional |
| `/FreeSans.ttf` | Regular font (nicer than built-in) | Optional |
| `/FreeSansBold.ttf` | Bold font (nicer than built-in) | Optional |
| `/logo.png` or `/logo.jpg` | Company/room logo (max 60px height) | Optional |

## Setting Up Your SD Card

### 1. Fonts (Recommended for Better Appearance)

Download FreeSans fonts:
- [FreeSans.ttf](https://github.com/opensourcedesign/fonts/raw/master/gnu-freefont_freesans/FreeSans.ttf)
- [FreeSansBold.ttf](https://github.com/opensourcedesign/fonts/raw/master/gnu-freefont_freesans/FreeSansBold.ttf)

Place both files in the **root** of your SD card.

### 2. Logo (Optional)

The M5EPD library is picky about image formats. Your logo must be:
- **Format**: PNG (8-bit grayscale) or JPG (grayscale)
- **Size**: Max 60px height, recommended ~400px width
- **Color**: Grayscale (not RGB/RGBA)

**Convert your logo using ImageMagick:**
```bash
# PNG (8-bit grayscale)
convert your-logo.png -colorspace Gray -depth 8 -resize 400x60 logo.png

# Or JPG (often more reliable with M5EPD)
convert your-logo.png -colorspace Gray -quality 90 -resize 400x60 logo.jpg
```

**Or use an online converter:**
1. Go to [online-convert.com](https://www.online-convert.com/)
2. Choose "Convert to PNG" or "Convert to JPG"
3. Settings:
   - Color: **Grayscale**
   - Resize: **400 x 60 pixels** (or smaller)
4. Download and save as `/logo.png` or `/logo.jpg` on your SD card

**Troubleshooting logo loading:**
- If you see `ERROR: Failed to render logo (error code: X)`, try converting to JPG instead
- Ensure the file is in the **root** of the SD card, not in a folder
- Check the serial monitor for error codes and file size confirmation

### 3. config.json (Recommended)

Copy `sd_card_example/config.example.json` to your SD card as `/config.json` and edit with your settings.

**Example for 24/7 operation:**
```json
{
  "wifi_ssid": "OfficeWiFi",
  "wifi_password": "YourPassword",
  "display_key": "boardroom1",
  "refresh_seconds": 300,
  "enable_business_hours": 0
}
```

**Example for custom business hours (7 AM - 8 PM, including weekends):**
```json
{
  "wifi_ssid": "OfficeWiFi",
  "wifi_password": "YourPassword",
  "display_key": "boardroom1",
  "refresh_seconds": 300,
  "enable_business_hours": 1,
  "business_hours_start": 7,
  "business_hours_end": 20,
  "deep_sleep_weekends": 0
}
```

