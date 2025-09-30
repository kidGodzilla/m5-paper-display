#pragma once

// Wi-Fi credentials
#ifndef WIFI_SSID
#define WIFI_SSID "NETWORK_SSID"
#endif
#ifndef WIFI_PW
#define WIFI_PW   "NETWORK_PASSWORD"
#endif

// Schedule API (example)
#ifndef SCHEDULE_API_BASE
#define SCHEDULE_API_BASE "https://api.meetingroom365.com"
#endif

// Room/display identifier
#ifndef ROOM_ID
#define ROOM_ID "displaykey"
#endif

// Display key alias (prefer this name in docs)
#ifndef DISPLAY_KEY
#define DISPLAY_KEY ROOM_ID
#endif

// Refresh interval seconds
#ifndef REFRESH_SECONDS
#define REFRESH_SECONDS 300
#endif

// Path to SD config
#ifndef SD_CONFIG_PATH
#define SD_CONFIG_PATH "/config.json"
#endif

// Optional TTF font paths on SD (root). If absent, built-in fonts are used
#ifndef FONT_REGULAR_PATH
#define FONT_REGULAR_PATH "/Inter.ttf"
#endif
#ifndef FONT_BOLD_PATH
#define FONT_BOLD_PATH "/InterBold.ttf"
#endif

// UI tunables
#ifndef SCREEN_ROTATION
#define SCREEN_ROTATION 90
#endif
#ifndef AVAILABLE_BG_SHADE
#define AVAILABLE_BG_SHADE 0   // pure white
#endif
#ifndef OCCUPIED_BG_SHADE
#define OCCUPIED_BG_SHADE 3   // subtle grey
#endif
#ifndef BUTTON_FILL_SHADE
#define BUTTON_FILL_SHADE 1   // very light grey
#endif
#ifndef BUTTON_PRESSED_SHADE
#define BUTTON_PRESSED_SHADE 5 // darker grey on press
#endif
#ifndef TEXT_COLOR_SHADE
#define TEXT_COLOR_SHADE 14   // dark text
#endif

// Touch interrupt pin for wake
#ifndef TOUCH_INT_PIN
#define TOUCH_INT_PIN GPIO_NUM_36
#endif

// Battery indicator
#ifndef SHOW_BATTERY
#define SHOW_BATTERY 1 // set 0 to hide battery indicator
#endif

// Debug clock (small HH:MM at top-left)
#ifndef SHOW_DEBUG_CLOCK
#define SHOW_DEBUG_CLOCK 1 // set 1 to show a small clock for debugging
#endif

// Time format: 24-hour (1) or 12-hour with AM/PM (0)
#ifndef TWENTYFOUR_HOUR
#define TWENTYFOUR_HOUR 1
#endif

// Debug logging: 0=minimal (errors only), 1=verbose (all operations)
#ifndef DEBUG_LOGGING
#define DEBUG_LOGGING 1
#endif

// Business hours (deep sleep outside these times to conserve battery)
#ifndef ENABLE_BUSINESS_HOURS
#define ENABLE_BUSINESS_HOURS 1  // 1=enable business hours sleep, 0=always active (24/7)
#endif
#ifndef BUSINESS_HOURS_START
#define BUSINESS_HOURS_START 8  // 8 AM (24-hour format, 0-23)
#endif
#ifndef BUSINESS_HOURS_END
#define BUSINESS_HOURS_END 19   // 7 PM (24-hour format, 0-23)
#endif
#ifndef DEEP_SLEEP_WEEKENDS
#define DEEP_SLEEP_WEEKENDS 1   // 1=deep sleep Sat/Sun, 0=stay active on weekends
#endif

// Power optimization toggles
#ifndef EPD_POWER_OFF_IN_SLEEP
#define EPD_POWER_OFF_IN_SLEEP 1 // disable EPD power during light sleep, re-enable on wake
#endif
#ifndef EXTPWR_OFF_IN_SLEEP
#define EXTPWR_OFF_IN_SLEEP 0 // usually keep EXTPower on so touch stays powered
#endif