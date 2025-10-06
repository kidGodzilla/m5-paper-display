#include <M5EPD.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"

// Debug logging macros (controlled by DEBUG_LOGGING in Config.h)
#if DEBUG_LOGGING
  #define LOG(...) Serial.printf(__VA_ARGS__)
  #define LOGLN(x) Serial.println(x)
#else
  #define LOG(...)
  #define LOGLN(x)
#endif

// Modern, minimal meeting room display for M5Paper in portrait orientation.
// Layout (left-aligned text hierarchy):
//  - Meeting Room Name (large)
//  - Current Meeting (title + time)
//  - Next Meeting (title + time)
//  - Actions (two full-width bars)

static const int PORTRAIT_WIDTH  = 540;  // X when in portrait
static const int PORTRAIT_HEIGHT = 960;  // Y when in portrait

// Power saving: how often to wake, refresh, and sleep again (seconds)
static uint32_t refreshIntervalSeconds = REFRESH_SECONDS; // runtime-configurable

// UI content placeholders (replace with live data later)
String displayKey           = DISPLAY_KEY; // default; overridden by SD config if present
String roomName             = ROOM_ID;
String currentMeetingTime   = "";
String currentMeetingTitle  = ""; // fallback if only one line is provided
String currentMeetingLine1  = "";
String currentMeetingLine2  = "";
String nextMeetingLabel     = "Next";
String nextMeetingTime      = "";
String nextMeetingTitle     = "";
int    canExtend            = 0;
int    canEndMeeting        = 0;
int    canInstantReserve    = 0;
int    occupied             = 0;
int    g_action             = 0; // 0=none, 1=reserve, 2=end, 3=extend
String voltstr              = "0.0";
static unsigned long touchCooldownUntilMs = 0;
static String wifiSsidStr = WIFI_SSID;
static String wifiPwStr   = WIFI_PW;
static bool timeSynced = false;
static int tzOffsetMinutes = 0; // from API 'timezone' (hours) * 60
static unsigned long lastSleepMs = 0;
static unsigned long lastNtpSyncMs = 0; // Track when we last synced NTP
static uint32_t consecutiveTimerWakeMisses = 0;

// Error state tracking
static bool hasCriticalError = false;
static String errorMessage = "";
static uint32_t consecutiveApiFails = 0;

static uint32_t consecutiveExt0Bounces = 0;
static unsigned long ext0CooldownUntilMs = 0;

// Business hours configuration (loaded from Config.h or SD config.json)
static bool enableBusinessHours = ENABLE_BUSINESS_HOURS;
static int businessHoursStart = BUSINESS_HOURS_START;
static int businessHoursEnd = BUSINESS_HOURS_END;
static bool deepSleepWeekends = DEEP_SLEEP_WEEKENDS;

// Interactive features configuration
static bool enableInstantReserve = ENABLE_INSTANT_RESERVE;
static bool enableExtendMeeting = ENABLE_EXTEND_MEETING;
static bool enableEndEarly = ENABLE_END_EARLY;
static uint32_t batterySaverSleepSeconds = BATTERY_SAVER_SLEEP_SECONDS;

// Battery saver mode: auto-enabled when all interactive features disabled
static bool enableBatterySaver = false; // computed at runtime

// Temporary wake mode (button press during deep sleep)
static bool inTemporaryWakeMode = false;
static unsigned long temporaryWakeStartMs = 0;
static const unsigned long TEMPORARY_WAKE_DURATION_MS = 30000; // 30 seconds to interact

M5EPD_Canvas canvas(&M5.EPD);

// Optional TrueType fonts from SD card (paths defined in Config.h)
static bool hasTTFFonts = false;

// Optional logo from SD card
static bool hasLogo = false;
static bool logoIsJpg = false;
static int logoWidth = 0;
static int logoHeight = 0;

static void useTTF(const char* path, int pixelSize) {
  if (!hasTTFFonts) return;
  canvas.loadFont(path, SD);
  canvas.createRender(pixelSize, 256);
  canvas.setTextSize(pixelSize);
  canvas.setTextColor(TEXT_COLOR_SHADE); // dark text
}

static void updateVoltageString() {
  uint32_t mv = M5.getBatteryVoltage();
  if (mv < 300) mv = 0; // guard weird reads
  float v = mv / 1000.0f;
  voltstr = String(v, 2);
}

// Check if current time is within business hours (or temporary wake mode)
static bool isWithinBusinessHours() {
  // Critical errors disable business hours (no touch interaction)
  if (hasCriticalError) return false;
  
  // Battery saver mode disables touch entirely (no buttons shown, always deep sleep)
  if (enableBatterySaver) return false;
  
  // Temporary wake mode overrides business hours check
  if (inTemporaryWakeMode) return true;
  
  if (!enableBusinessHours) return true; // feature disabled, always active
  if (!timeSynced) return true; // if time not synced, assume business hours (stay active)
  
  time_t nowSecs = time(nullptr);
  struct tm utcInfo;
  gmtime_r(&nowSecs, &utcInfo);
  
  // Apply timezone offset to get LOCAL time
  time_t localSecs = nowSecs + (tzOffsetMinutes * 60);
  struct tm localInfo;
  gmtime_r(&localSecs, &localInfo);
  
  int hour = localInfo.tm_hour;
  int wday = localInfo.tm_wday; // 0=Sunday, 6=Saturday
  
  LOG("Business hours check: UTC=%02d:%02d, Local=%02d:%02d (tz offset=%d min)\n",
      utcInfo.tm_hour, utcInfo.tm_min, localInfo.tm_hour, localInfo.tm_min, tzOffsetMinutes);
  
  // Check weekend
  if (deepSleepWeekends && (wday == 0 || wday == 6)) {
    LOG("Outside business hours: Weekend (local day=%d)\n", wday);
    return false;
  }
  
  // Check hour range (business hours are in LOCAL time)
  if (hour < businessHoursStart || hour >= businessHoursEnd) {
    LOG("Outside business hours: local hour=%d (range: %d-%d)\n", hour, businessHoursStart, businessHoursEnd);
    return false;
  }
  
  LOG("Within business hours: local hour=%d\n", hour);
  return true;
}

// Calculate seconds until next business hours start
static uint64_t secondsUntilBusinessHours() {
  time_t nowSecs = time(nullptr);
  // Apply timezone offset to get LOCAL time
  time_t localSecs = nowSecs + (tzOffsetMinutes * 60);
  struct tm tinfo;
  gmtime_r(&localSecs, &tinfo);
  
  int currentHour = tinfo.tm_hour;
  int currentMin = tinfo.tm_min;
  int currentSec = tinfo.tm_sec;
  int wday = tinfo.tm_wday; // 0=Sunday, 6=Saturday
  
  // Calculate seconds since midnight today
  uint64_t secondsSinceMidnight = currentHour * 3600ULL + currentMin * 60ULL + currentSec;
  
  // If it's weekend, sleep until Monday start time
  if (deepSleepWeekends && (wday == 0 || wday == 6)) {
    int daysUntilMonday = (wday == 0) ? 1 : 2; // Sunday->1day, Saturday->2days
    uint64_t sleepSecs = (daysUntilMonday * 86400ULL) - secondsSinceMidnight + (businessHoursStart * 3600ULL);
    LOG("Weekend: sleeping %llu seconds until Monday %d:00\n", sleepSecs, businessHoursStart);
    return sleepSecs;
  }
  
  // If before business hours today, sleep until start time
  if (currentHour < businessHoursStart) {
    uint64_t sleepSecs = (businessHoursStart * 3600ULL) - secondsSinceMidnight;
    LOG("Before hours: sleeping %llu seconds until %d:00\n", sleepSecs, businessHoursStart);
    return sleepSecs;
  }
  
  // Otherwise, after hours today - sleep until tomorrow's start time
  uint64_t secondsUntilMidnight = 86400ULL - secondsSinceMidnight;
  uint64_t sleepSecs = secondsUntilMidnight + (businessHoursStart * 3600ULL);
  
  // But if tomorrow is weekend and we deep sleep weekends, extend to Monday
  int tomorrowWday = (wday + 1) % 7;
  if (deepSleepWeekends && (tomorrowWday == 0 || tomorrowWday == 6)) {
    int daysUntilMonday = (tomorrowWday == 6) ? 2 : 1; // Sat->2, Sun->1
    sleepSecs += (daysUntilMonday - 1) * 86400ULL;
    LOG("After hours (weekend ahead): sleeping %llu seconds until Monday %d:00\n", sleepSecs, businessHoursStart);
  } else {
    LOG("After hours: sleeping %llu seconds until tomorrow %d:00\n", sleepSecs, businessHoursStart);
  }
  
  return sleepSecs;
}

static void clampAndLogRefresh() {
  uint32_t before = refreshIntervalSeconds;
  if (refreshIntervalSeconds < 5) refreshIntervalSeconds = 5;
  if (refreshIntervalSeconds > 86400UL) refreshIntervalSeconds = 86400UL;
  if (refreshIntervalSeconds != before) {
    LOG("Clamped refreshSeconds from %lu to %lu\n", (unsigned long)before, (unsigned long)refreshIntervalSeconds);
  }
}

static void logWakeCause() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const char* name = "OTHER";
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: name = "TIMER"; break;
    case ESP_SLEEP_WAKEUP_EXT0:  name = "EXT0";  break;
    case ESP_SLEEP_WAKEUP_EXT1:  name = "EXT1";  break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: name = "TOUCHPAD"; break;
    default: break;
  }
  unsigned long delta = (lastSleepMs == 0) ? 0 : (millis() - lastSleepMs);
  LOG("Wake cause=%s (%d), slept ~%lums\n", name, (int)cause, delta);
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    consecutiveTimerWakeMisses = 0;
    consecutiveExt0Bounces = 0;
    ext0CooldownUntilMs = 0;
  } else if (cause != ESP_SLEEP_WAKEUP_EXT0) {
    consecutiveTimerWakeMisses++;
    if (consecutiveTimerWakeMisses >= 3) {
      Serial.println("ERROR: Too many non-timer wakes; restarting");
      delay(100);
      esp_restart();
    }
  } else {
    // EXT0 bounce (very short sleep) or stuck INT
    if (delta < 100) {
      consecutiveExt0Bounces++;
      // apply cooldown immediately after a few bounces
      if (consecutiveExt0Bounces >= 3) {
        ext0CooldownUntilMs = millis() + 5000; // 5s without EXT0 to allow timer wake
        LOG("EXT0 bouncing; applying 5s cooldown (skip arming EXT0)\n");
      }
    } else {
      consecutiveExt0Bounces = 0;
    }
  }
}

static void rearmAndSleep() {
  // Battery saver mode: deep sleep always (no touch interaction)
  if (enableBatterySaver && !inTemporaryWakeMode) {
    uint32_t sleepSeconds = batterySaverSleepSeconds;
    
    // Align wake time to 5-minute intervals to match typical meeting booking times
    if (timeSynced) {
      time_t nowSecs = time(nullptr);
      time_t localSecs = nowSecs + (tzOffsetMinutes * 60);
      struct tm localInfo;
      gmtime_r(&localSecs, &localInfo);
      
      int currentMin = localInfo.tm_min;
      int currentSec = localInfo.tm_sec;
      
      // Find next 5-minute interval (0, 5, 10, 15, ..., 55)
      int nextIntervalMin = ((currentMin / 5) + 1) * 5;
      if (nextIntervalMin >= 60) {
        nextIntervalMin = 0; // wrap to next hour
      }
      
      // Calculate seconds until next 5-minute boundary
      int minutesUntil = (nextIntervalMin - currentMin);
      if (minutesUntil <= 0) {
        minutesUntil += 60; // wrapped to next hour
      }
      uint32_t secondsToInterval = (minutesUntil * 60) - currentSec;
      
      // Use the time to next interval, but ensure it's reasonable (at least 30s, max configured value)
      if (secondsToInterval < 30) {
        secondsToInterval += 300; // Add 5 minutes if we're too close
      }
      if (secondsToInterval > batterySaverSleepSeconds) {
        secondsToInterval = batterySaverSleepSeconds; // Cap at configured max
      }
      
      sleepSeconds = secondsToInterval;
      Serial.printf("Battery saver: aligning to 5-min interval (current: %02d:%02d, next interval: :%02d, sleep: %lus)\n",
                    currentMin, currentSec, nextIntervalMin, (unsigned long)sleepSeconds);
    } else {
      Serial.printf("Battery saver: time not synced, using configured interval: %lus\n", (unsigned long)sleepSeconds);
    }
    
    uint64_t deepSleepUs = (uint64_t)sleepSeconds * 1000000ULL;
    
    Serial.printf("Battery saver: deep sleeping for %lu seconds\n", (unsigned long)sleepSeconds);
    
    // Show what time we expect to wake up
    if (timeSynced) {
      time_t wakeTime = time(nullptr) + sleepSeconds;
      time_t localWake = wakeTime + (tzOffsetMinutes * 60);
      struct tm wakeInfo;
      gmtime_r(&localWake, &wakeInfo);
      Serial.printf("Expected wake time: %02d:%02d:%02d\n", wakeInfo.tm_hour, wakeInfo.tm_min, wakeInfo.tm_sec);
    }
    
    Serial.println("Disabling power rails...");
    M5.disableEPDPower();
    // Keep EXTPower enabled so buttons can wake the device
    // M5.disableEXTPower(); // Commented out - buttons need power
    WiFi.mode(WIFI_OFF);
    
    // Arm timer wake
    esp_sleep_enable_timer_wakeup(deepSleepUs);
    Serial.printf("Timer wake armed for %lu seconds\n", (unsigned long)sleepSeconds);
    
    // Arm physical button wake (G37, G38, G39) so user can check schedule
    uint64_t buttonMask = (1ULL << GPIO_NUM_37) | (1ULL << GPIO_NUM_38) | (1ULL << GPIO_NUM_39);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ALL_LOW);
    Serial.printf("EXT1 wake armed: %s (G37/G38/G39)\n", err == ESP_OK ? "OK" : "FAILED");
    Serial.println("Entering deep sleep NOW (battery saver)");
    Serial.flush();
    
    delay(200);
    esp_deep_sleep_start();
    // Device will reboot on wake
  }
  
  // Check if we're outside business hours - if so, deep sleep until next business day
  if (!isWithinBusinessHours()) {
    uint64_t deepSleepSeconds = secondsUntilBusinessHours();
    uint64_t deepSleepUs = deepSleepSeconds * 1000000ULL;
    
    // Cap deep sleep to prevent overflow (max ~48 days)
    if (deepSleepUs > 0xFFFFFFFFFFFFULL) {
      deepSleepUs = 86400ULL * 1000000ULL; // fallback: 1 day
    }
    
    Serial.printf("Deep sleeping outside business hours for %llu seconds\n", deepSleepSeconds);
    Serial.println("Disabling power rails...");
    M5.disableEPDPower();
    // Keep EXTPower enabled so buttons can wake the device
    // M5.disableEXTPower(); // Commented out - buttons need power
    WiFi.mode(WIFI_OFF);
    
    // Arm timer wake for next business hours
    esp_sleep_enable_timer_wakeup(deepSleepUs);
    Serial.printf("Timer wake armed for %llu seconds\n", deepSleepSeconds);
    
    // Arm physical button wake (G37, G38, G39) so user can check schedule
    uint64_t buttonMask = (1ULL << GPIO_NUM_37) | (1ULL << GPIO_NUM_38) | (1ULL << GPIO_NUM_39);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ALL_LOW);
    Serial.printf("EXT1 wake armed: %s (G37/G38/G39)\n", err == ESP_OK ? "OK" : "FAILED");
    Serial.println("Entering deep sleep NOW");
    Serial.flush();
    
    delay(200);
    esp_deep_sleep_start();
    // Device will reboot on wake
  }
  
  // Normal light sleep during business hours
  #if EPD_POWER_OFF_IN_SLEEP
  M5.disableEPDPower();
  #endif
  #if EXTPWR_OFF_IN_SLEEP
  M5.disableEXTPower();
  #endif
  clampAndLogRefresh();
  uint64_t armUs2 = (uint64_t)refreshIntervalSeconds * 1000000ULL;
  LOG("Re-arming timer wake in %lus (%llu us)\n", (unsigned long)refreshIntervalSeconds, (unsigned long long)armUs2);
  esp_sleep_enable_timer_wakeup(armUs2);
  bool extArmed2 = armExt0IfIdle();
  LOG("EXT0 armed=%s\n", extArmed2 ? "yes" : "no");
  WiFi.mode(WIFI_OFF);
  lastSleepMs = millis();
  esp_light_sleep_start();
}

static bool armExt0IfIdle() {
  // Only arm EXT0 if touch reports finger up (INT not asserted)
  if (ext0CooldownUntilMs && millis() < ext0CooldownUntilMs) {
    return false;
  }
  M5.TP.update();
  if (!M5.TP.isFingerUp()) return false;
  esp_sleep_enable_ext0_wakeup(TOUCH_INT_PIN, 0);
  return true;
}

// Simple two-line word wrap for titles: splits near middle at space
void drawTitleTwoLine(const char* text, int x, int &y, int lineSpacing) {
  String s = String(text);
  if (s.length() <= 20) {
    canvas.drawString(s.c_str(), x, y);
    return;
  }
  int mid = s.length() / 2;
  int split = -1;
  for (int i = mid; i >= 0 && i >= mid - 12; --i) {
    if (s[i] == ' ') { split = i; break; }
  }
  if (split == -1) {
    for (int i = mid; i < s.length() && i <= mid + 12; ++i) {
      if (s[i] == ' ') { split = i; break; }
    }
  }
  if (split == -1) split = mid; // fallback hard split
  String line1 = s.substring(0, split);
  String line2 = s.substring(split + 1);
  canvas.drawString(line1.c_str(), x, y);
  y += lineSpacing;
  canvas.drawString(line2.c_str(), x, y);
}

void drawCenteredString(const char* text, int centerX, int centerY) {
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString(text, centerX, centerY);
  canvas.setTextDatum(TL_DATUM);
}

void drawActionBar(int x, int y, int w, int h, const char* label) {
  const int r = 2; // subtle corners
  // very light grey fill (close to white)
  canvas.fillRoundRect(x, y, w, h, r, BUTTON_FILL_SHADE);
  canvas.setTextDatum(MC_DATUM); // Middle-Center alignment
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 44);
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(5);
  }
  // Center text in button (with 2px down adjustment for visual alignment)
  canvas.drawString(label, x + w / 2, y + h / 2 + 2);
  canvas.setTextDatum(TL_DATUM);
}

// Pressed-state variant for immediate feedback
void drawActionBarPressed(int x, int y, int w, int h, const char* label) {
  const int r = 2;
  // Slightly darker bar to indicate press
  canvas.fillRoundRect(x, y, w, h, r, BUTTON_PRESSED_SHADE);
  canvas.setTextDatum(MC_DATUM); // Middle-Center alignment
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 44);
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(5);
  }
  // Draw label in white on dark bar
  uint8_t prevColor = 14;
  canvas.setTextColor(0);
  // Center text in button (with 2px down adjustment for visual alignment)
  canvas.drawString(label, x + w / 2, y + h / 2 + 2);
  canvas.setTextColor(prevColor);
  canvas.setTextDatum(TL_DATUM);
}

void drawUI() {
  // Background: white when available, subtle grey when occupied
  if (occupied) {
    canvas.fillCanvas(OCCUPIED_BG_SHADE);
  } else {
    canvas.fillCanvas(AVAILABLE_BG_SHADE);
  }

  const int margin = 24;
  const int contentWidth = PORTRAIT_WIDTH - margin * 2;

  // Debug clock (top-left) - drawn first at fixed position
  if (SHOW_DEBUG_CLOCK) {
    time_t nowSecs = time(nullptr);
    LOG("[drawUI] Debug clock: rawTime=%lu tzOffsetMin=%d\n", (unsigned long)nowSecs, tzOffsetMinutes);
    if (tzOffsetMinutes != 0) nowSecs += tzOffsetMinutes * 60;
    struct tm tinfo;
    gmtime_r(&nowSecs, &tinfo);
    char buf[16];
    if (TWENTYFOUR_HOUR) {
      snprintf(buf, sizeof(buf), "%02d:%02d", tinfo.tm_hour, tinfo.tm_min);
    } else {
      int hour12 = tinfo.tm_hour % 12; if (hour12 == 0) hour12 = 12;
      const char* ampm = (tinfo.tm_hour < 12) ? "AM" : "PM";
      snprintf(buf, sizeof(buf), "%d:%02d %s", hour12, tinfo.tm_min, ampm);
    }
    LOG("[drawUI] Debug clock text: %s\n", buf);
    canvas.setTextDatum(TL_DATUM);
    if (hasTTFFonts) { useTTF(FONT_REGULAR_PATH, 28); } else { canvas.setTextFont(1); canvas.setTextSize(3); }
    canvas.drawString(buf, margin, 20);
  }

  // Battery indicator (top-right) - drawn at fixed position
  if (SHOW_BATTERY) {
    // Read battery voltage and map to percent (3300-4350mV)
    uint32_t mv = M5.getBatteryVoltage();
    if (mv < 3300) mv = 3300; if (mv > 4350) mv = 4350;
    int pct = (int)((mv - 3300) * 100 / (4350 - 3300));
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;

    int bx = PORTRAIT_WIDTH - 24 - 80; // right margin 24, width ~80
    int by = 14;                        // top margin (moved up for vertical centering with clock)
    int bw = 80;
    int bh = 28;
    int capW = 6;
    // body
    canvas.drawRoundRect(bx, by, bw, bh, 6, TEXT_COLOR_SHADE);
    // cap
    canvas.drawRoundRect(bx + bw, by + (bh/4), capW, bh/2, 2, TEXT_COLOR_SHADE);
    // fill
    int innerW = bw - 6; int innerH = bh - 6;
    int fillW = innerW * pct / 100;
    canvas.fillRect(bx + 3, by + 3, fillW, innerH, TEXT_COLOR_SHADE);
  }

  // Start main content layout - position everything below debug clock/battery
  int y = 70; // Start below debug area (moved down 10px for logo)

  // Optional logo at top (moves everything down if present)
  if (hasLogo && logoHeight > 0) {
    // Draw the logo (return value is unreliable, may be non-zero even on success)
    if (logoIsJpg) {
      canvas.drawJpgFile(SD, "/logo.jpg", margin, y);
      LOG("[drawUI] Drew JPG logo\n");
    } else {
      canvas.drawPngFile(SD, "/logo.png", margin, y);
      LOG("[drawUI] Drew PNG logo\n");
    }
    // Always add logo spacing if hasLogo is true (logo was detected during setup)
    y += logoHeight + 80; // logo height + generous spacing before room name
  } else {
    // No logo: add standard spacing
    y += 20;
  }

  // Room name (large, left-aligned, bigger scale)
  canvas.setTextDatum(ML_DATUM); // Middle-Left alignment (revert to original)
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 68);
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(6);
  }
  canvas.drawString(roomName.c_str(), margin, y); // left-aligned with other text
  canvas.setTextDatum(TL_DATUM);

  // Current meeting block (prominent)
  y += 86; // spacing after room name (decreased by 2px)
  canvas.setTextDatum(ML_DATUM); // Middle-Left alignment (revert to original)
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 36);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString("Now", margin, y);
  y += 68; // spacing after "Now"
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 64); // current meeting title size
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(6);
  }
  // Build a single subject string (concatenate L1 + L2 if both provided), then word-wrap
  {
    String subject = currentMeetingTitle;
    if (currentMeetingLine1.length() > 0 || currentMeetingLine2.length() > 0) {
      subject = currentMeetingLine1;
      if (currentMeetingLine2.length() > 0) {
        if (subject.length() > 0) subject += " ";
        subject += currentMeetingLine2;
      }
    }

    // Multi-line word wrap for meeting title (supports 3+ lines)
    const float approxCharWidth = 0.56f; // FreeSans ~0.56 * px
    const float fontPx = 64.0f;
    int maxChars = (int)((float)contentWidth / (fontPx * approxCharWidth));
    if (maxChars < 8) maxChars = 8;
    const int lineSpacing = 69; // spacing between wrapped lines

    String remaining = subject;
    bool firstLine = true;
    while (remaining.length() > 0) {
      if (!firstLine) {
        y += lineSpacing;
      }
      
      if ((int)remaining.length() <= maxChars) {
        // Last line fits completely
        canvas.drawString(remaining.c_str(), margin - 1, y);
        break;
    } else {
        // Need to wrap: find word boundary
        int split = remaining.lastIndexOf(' ', maxChars);
        if (split < 0) split = maxChars; // Hard split if no space found
        String line = remaining.substring(0, split);
        canvas.drawString(line.c_str(), margin - 1, y);
        remaining = remaining.substring(split + 1); // +1 to skip the space
        firstLine = false;
      }
    }
  }
  y += 67; // increased from 64 to add 3px more space before time
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 40);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString(currentMeetingTime.c_str(), margin, y);
  canvas.setTextDatum(TL_DATUM);

  // Next meeting block
  y += 96; // spacing before Next
  canvas.setTextDatum(ML_DATUM); // Middle-Left alignment (revert to original)
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 36);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString(nextMeetingLabel.c_str(), margin, y);
  y += 59; // spacing after "Next"
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 44);
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(4);
  }
  // Wrap next meeting title if too long for one line
  {
    const float approxCharWidth = 0.56f; // FreeSans ~0.56 * px
    int maxChars = (int)((float)contentWidth / (44.0f * approxCharWidth));
    if (maxChars < 8) maxChars = 8;
    if ((int)nextMeetingTitle.length() > maxChars) {
      int split = nextMeetingTitle.lastIndexOf(' ', maxChars);
      if (split < 0) split = maxChars;
      String line1 = nextMeetingTitle.substring(0, split);
      String line2 = nextMeetingTitle.substring(split + 1);
      canvas.drawString(line1.c_str(), margin, y);
      y += 52; // line spacing for next title
      canvas.drawString(line2.c_str(), margin, y);
    } else {
      canvas.drawString(nextMeetingTitle.c_str(), margin, y);
    }
  }
  y += 56; // spacing after next meeting title
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 40);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString(nextMeetingTime.c_str(), margin, y);

  // Error message area (replaces buttons if critical error)
  if (hasCriticalError) {
    int messageY = PORTRAIT_HEIGHT - 80;
    canvas.setTextDatum(TC_DATUM);
    if (hasTTFFonts) {
      useTTF(FONT_REGULAR_PATH, 28);
    } else {
      canvas.setTextFont(1);
      canvas.setTextSize(3);
    }
    canvas.setTextColor(12); // Darker for visibility
    canvas.drawString("ERROR", PORTRAIT_WIDTH / 2, messageY);
    messageY += 36;
    canvas.setTextColor(TEXT_COLOR_SHADE);
    canvas.drawString(errorMessage.c_str(), PORTRAIT_WIDTH / 2, messageY);
  canvas.setTextDatum(TL_DATUM);
  }
  // Actions area at bottom (only show buttons if within business hours - touch is active)
  else if (isWithinBusinessHours()) {
  int actionsH = 108;
    int buttonGap = 12; // gap between two buttons
    int bottomMargin = margin; // match left/right margin (24px)

  // Decide actions based on flags AND config settings
  String firstAction = "";
  String secondAction = "";
  if (occupied) {
    if (canExtend && enableExtendMeeting) firstAction = "Extend";
    if (canEndMeeting && enableEndEarly) {
      if (firstAction.length() == 0) firstAction = "End Early"; else secondAction = "End Early";
    }
  } else {
      if (canInstantReserve && enableInstantReserve) firstAction = "Reserve";
  }

  if (firstAction.length() > 0 && secondAction.length() == 0) {
      // Single button: full-width at bottom
      int singleY = PORTRAIT_HEIGHT - actionsH - bottomMargin;
    drawActionBar(margin, singleY, contentWidth, actionsH, firstAction.c_str());
    } else if (firstAction.length() > 0 && secondAction.length() > 0) {
      // Two buttons: side-by-side at bottom
      int buttonY = PORTRAIT_HEIGHT - actionsH - bottomMargin;
      int buttonWidth = (contentWidth - buttonGap) / 2;
      drawActionBar(margin, buttonY, buttonWidth, actionsH, firstAction.c_str());
      drawActionBar(margin + buttonWidth + buttonGap, buttonY, buttonWidth, actionsH, secondAction.c_str());
    }
  } else {
    // Outside business hours OR battery saver mode when vacant: show sleep message
    int messageY = PORTRAIT_HEIGHT - 100; // Position in button area
    canvas.setTextDatum(TC_DATUM); // Top-Center alignment
    if (hasTTFFonts) {
      useTTF(FONT_REGULAR_PATH, 28);
    } else {
      canvas.setTextFont(1);
      canvas.setTextSize(3);
    }
    
    // Different message for battery saver vs business hours
    if (enableBatterySaver) {
      String msg = occupied ? "Battery saver mode enabled." : "Saving battery while vacant.";
      canvas.drawString(msg.c_str(), PORTRAIT_WIDTH / 2, messageY);
      messageY += 36;
      
      // Calculate next wake time aligned to 5-minute interval
      time_t nowSecs = time(nullptr);
      time_t localSecs = nowSecs + (tzOffsetMinutes * 60);
      struct tm localInfo;
      gmtime_r(&localSecs, &localInfo);
      
      // Calculate next 5-minute interval
      int currentMin = localInfo.tm_min;
      int currentSec = localInfo.tm_sec;
      int nextIntervalMin = ((currentMin / 5) + 1) * 5;
      if (nextIntervalMin >= 60) {
        nextIntervalMin = 0;
      }
      int minutesUntil = (nextIntervalMin - currentMin);
      if (minutesUntil <= 0) {
        minutesUntil += 60;
      }
      uint32_t secondsToInterval = (minutesUntil * 60) - currentSec;
      if (secondsToInterval < 30) {
        secondsToInterval += 300; // Add 5 minutes if too close
      }
      if (secondsToInterval > batterySaverSleepSeconds) {
        secondsToInterval = batterySaverSleepSeconds;
      }
      
      // Calculate next wake time
      time_t nextWakeSecs = localSecs + secondsToInterval;
      struct tm nextWake;
      gmtime_r(&nextWakeSecs, &nextWake);
      
      char buf[32];
      if (TWENTYFOUR_HOUR) {
        snprintf(buf, sizeof(buf), "Next check: %02d:%02d", nextWake.tm_hour, nextWake.tm_min);
      } else {
        int hour12 = nextWake.tm_hour % 12; if (hour12 == 0) hour12 = 12;
        const char* ampm = (nextWake.tm_hour < 12) ? "AM" : "PM";
        snprintf(buf, sizeof(buf), "Next check: %d:%02d %s", hour12, nextWake.tm_min, ampm);
      }
      canvas.drawString(buf, PORTRAIT_WIDTH / 2, messageY);
    } else {
      canvas.drawString("Device is sleeping.", PORTRAIT_WIDTH / 2, messageY);
      messageY += 36;
      
      // Calculate and show next wake time
      time_t nowSecs = time(nullptr);
      time_t localSecs = nowSecs + (tzOffsetMinutes * 60);
      struct tm localInfo;
      gmtime_r(&localSecs, &localInfo);
      
      String nextUpdate = "Next update: ";
      if (deepSleepWeekends && (localInfo.tm_wday == 0 || localInfo.tm_wday == 6)) {
        nextUpdate += "Monday " + String(businessHoursStart) + ":00";
      } else if (localInfo.tm_hour >= businessHoursEnd) {
        // After hours - wake tomorrow
        if (deepSleepWeekends && ((localInfo.tm_wday + 1) % 7 == 0 || (localInfo.tm_wday + 1) % 7 == 6)) {
          nextUpdate += "Monday " + String(businessHoursStart) + ":00";
        } else {
          nextUpdate += "Tomorrow " + String(businessHoursStart) + ":00";
        }
      } else {
        // Before hours - wake today
        nextUpdate += "Today " + String(businessHoursStart) + ":00";
      }
      
      canvas.drawString(nextUpdate.c_str(), PORTRAIT_WIDTH / 2, messageY);
    }
    canvas.setTextDatum(TL_DATUM);
  }
}

// Networking
static bool wifiConnected = false;

static bool connectWiFi() {
  LOG("[connectWiFi] Attempting to connect to '%s'...\n", wifiSsidStr.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsidStr.c_str(), wifiPwStr.c_str());
  unsigned long start = millis();
  for (int i = 0; i < 50 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(200);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    LOG("[connectWiFi] SUCCESS in %lums, IP=%s\n", millis() - start, WiFi.localIP().toString().c_str());
    // Success - reset consecutive fail counter if we had one
    if (consecutiveApiFails > 0 && consecutiveApiFails < 3) {
      consecutiveApiFails = 0;
    }
  } else {
    Serial.printf("ERROR: Wi-Fi connect failed status=%d\n", (int)WiFi.status());
    consecutiveApiFails++;
    if (consecutiveApiFails >= 3) {
      hasCriticalError = true;
      errorMessage = "Wi-Fi connection failed. Check credentials.";
    }
    return false;
  }
  // Only sync NTP if we haven't synced yet, or it's been more than 24 hours
  if (wifiConnected && (!timeSynced || (millis() - lastNtpSyncMs > 86400000UL))) {
    LOG("[connectWiFi] Syncing NTP...\n");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long ntpStart = millis();
    time_t now = 0;
    while ((now = time(nullptr)) < 1609459200 && millis() - ntpStart < 5000) { // wait up to ~5s
      delay(200);
    }
    timeSynced = (now >= 1609459200);
    if (timeSynced) lastNtpSyncMs = millis();
    LOG("[connectWiFi] NTP sync %s (took %lums, now=%lu)\n", timeSynced ? "OK" : "TIMEOUT", millis() - ntpStart, (unsigned long)now);
  } else if (timeSynced) {
    LOG("[connectWiFi] Skipping NTP (last sync %lu ms ago)\n", millis() - lastNtpSyncMs);
  }
  return wifiConnected;
}

static void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
}

static bool fetchSchedule() {
  LOG("[fetchSchedule] Starting...\n");
  if (!connectWiFi()) {
    Serial.println("ERROR: fetchSchedule: Wi-Fi connect failed");
    return false;
  }
  LOG("[fetchSchedule] Wi-Fi connected, building HTTP request\n");
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8); // 8 second timeout (default is 30s)
  HTTPClient http;
  http.setTimeout(8000); // 8 second timeout
  // Snapshot and clear action immediately to avoid accidental repeats
  int actionParam = g_action;
  g_action = 0;
  updateVoltageString();
  String url = String(SCHEDULE_API_BASE) + "/eink/" + displayKey + "?action=" + String(actionParam) + "&voltage=" + voltstr + "&model=m3paper";
  LOG("[fetchSchedule] URL: %s\n", url.c_str());
  if (!http.begin(client, url)) {
    Serial.println("ERROR: fetchSchedule: http.begin() failed");
    disconnectWiFi();
    consecutiveApiFails++;
    if (consecutiveApiFails >= 3) {
      hasCriticalError = true;
      errorMessage = "Network error. Check Wi-Fi configuration.";
    }
    return false;
  }
  LOG("[fetchSchedule] Sending GET request...\n");
  unsigned long httpStart = millis();
  int code = http.GET();
  LOG("[fetchSchedule] HTTP response code=%d (took %lums)\n", code, millis() - httpStart);
  if (code != HTTP_CODE_OK) {
    Serial.printf("ERROR: fetchSchedule: HTTP GET failed code=%d (%s)\n", code, http.errorToString(code).c_str());
    http.end();
    disconnectWiFi();
    consecutiveApiFails++;
    if (consecutiveApiFails >= 3) {
      hasCriticalError = true;
      errorMessage = "API error. Please contact support.";
    }
    return false;
  }

  LOG("[fetchSchedule] Parsing JSON response...\n");
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  disconnectWiFi();
  if (err) {
    Serial.printf("ERROR: fetchSchedule: JSON parse error: %s\n", err.c_str());
    return false;
  }
  LOG("[fetchSchedule] JSON parsed successfully\n");
  
  // Success - reset error counters
  consecutiveApiFails = 0;

  // Map fields per Meeting Room 365 e-ink API
  roomName            = doc["name"].as<String>();
  currentMeetingTime  = doc["currentMeetingTimeString"].as<String>();
  currentMeetingLine1 = doc["currentMeetingSubjectLine1"].as<String>();
  currentMeetingLine2 = doc["currentMeetingSubjectLine2"].as<String>();
  if (currentMeetingLine1.length() == 0 && currentMeetingLine2.length() == 0) {
    currentMeetingTitle = doc["status"].as<String>();
  } else {
    currentMeetingTitle = currentMeetingLine1;
  }
  nextMeetingLabel    = doc["nextMeetingString"].as<String>();
  nextMeetingTitle    = doc["nextMeetingSubject"].as<String>();
  nextMeetingTime     = doc["nextMeetingTimeString"].as<String>();
  canExtend           = doc["canExtend"].as<int>();
  canEndMeeting       = doc["canEndMeeting"].as<int>();
  canInstantReserve   = doc["canInstantReserve"].as<int>();
  occupied            = doc["occupied"].as<int>();
  if (doc.containsKey("timezone")) {
    tzOffsetMinutes = doc["timezone"].as<int>() * 60; // hours to minutes
  }
  
  // Use API timestamp as fallback if NTP failed or isn't synced
  if (doc.containsKey("timestamp")) {
    unsigned long apiTimestamp = doc["timestamp"].as<unsigned long>();
    if (apiTimestamp > 1609459200) { // sanity check (after Jan 1, 2021)
      // Set system time from API if NTP hasn't synced
      if (!timeSynced) {
        struct timeval tv;
        tv.tv_sec = apiTimestamp;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        timeSynced = true;
        lastNtpSyncMs = millis();
        LOG("[fetchSchedule] Time synced from API timestamp: %lu\n", apiTimestamp);
      } else {
        LOG("[fetchSchedule] API timestamp received: %lu (already synced via NTP)\n", apiTimestamp);
      }
    }
  }
  
  // action already cleared above
  LOG("[fetchSchedule] SUCCESS: room=%s occupied=%d canExtend=%d canEndMeeting=%d canInstantReserve=%d\n",
      roomName.c_str(), occupied, canExtend, canEndMeeting, canInstantReserve);

  return true;
}

static bool postExtend() {
  g_action = 3; // extend
  return fetchSchedule();
}

static bool postEndEarly() {
  g_action = 2; // end early
  return fetchSchedule();
}

static bool postReserve15() {
  g_action = 1; // instant reserve
  return fetchSchedule();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("M5Paper Meeting Room Display");
  M5.begin(true, true, true, true); // enable SD
  M5.EPD.SetRotation(SCREEN_ROTATION); // rotate to portrait; canvas is 540x960
  M5.EPD.Clear(true);

  canvas.createCanvas(PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
  canvas.setTextColor(TEXT_COLOR_SHADE); // dark text (foreground only)
  canvas.setTextSize(3);
  canvas.setTextDatum(TL_DATUM);

  // Initialize battery saver from default config values
  enableBatterySaver = (!enableInstantReserve && !enableExtendMeeting && !enableEndEarly);

  // Load config.json from SD if present
  Serial.println("[setup] Attempting to load config.json from SD...");
  if (SD.begin()) {
    Serial.println("[setup] SD card initialized");
    if (SD.exists(SD_CONFIG_PATH)) {
      Serial.printf("[setup] Found config file: %s\n", SD_CONFIG_PATH);
      File f = SD.open(SD_CONFIG_PATH, FILE_READ);
      if (f) {
        Serial.printf("[setup] Opened config file, size: %d bytes\n", f.size());
        DynamicJsonDocument cfg(2048);
        DeserializationError e = deserializeJson(cfg, f);
        f.close();
        if (!e) {
          Serial.println("[setup] JSON parsed successfully");
          const char* ssid = cfg["wifi_ssid"] | WIFI_SSID;
          const char* pw   = cfg["wifi_password"] | WIFI_PW;
          const char* key  = cfg["display_key"] | ROOM_ID;
          int refresh      = cfg["refresh_seconds"] | (int)REFRESH_SECONDS;
          int enableBiz    = cfg["enable_business_hours"] | (int)ENABLE_BUSINESS_HOURS;
          int bizStart     = cfg["business_hours_start"] | BUSINESS_HOURS_START;
          int bizEnd       = cfg["business_hours_end"] | BUSINESS_HOURS_END;
          int sleepWeekend = cfg["deep_sleep_weekends"] | (int)DEEP_SLEEP_WEEKENDS;
          int instantReserve = cfg["enable_instant_reserve"] | (int)ENABLE_INSTANT_RESERVE;
          int extendMeeting = cfg["enable_extend_meeting"] | (int)ENABLE_EXTEND_MEETING;
          int endEarly = cfg["enable_end_early"] | (int)ENABLE_END_EARLY;
          int batterySaverSleep = cfg["battery_saver_sleep_seconds"] | (int)BATTERY_SAVER_SLEEP_SECONDS;
          
          Serial.printf("[setup] Raw key from JSON: '%s'\n", key);
          Serial.printf("[setup] cfg.containsKey(\"display_key\"): %d\n", cfg.containsKey("display_key"));
          
          displayKey = String(key);
          roomName   = "Demo Room"; // until API loads real name
          // Override Wi-Fi creds used by connectWiFi
          wifiSsidStr = String(ssid);
          wifiPwStr   = String(pw);
          
          Serial.printf("[setup] Final displayKey: '%s'\n", displayKey.c_str());
          
          // Check for demo/placeholder display keys (but allow any actual configured value)
          // Only flag obviously invalid/placeholder values
          if (displayKey == "" || displayKey == "your_display_key_here") {
            hasCriticalError = true;
            errorMessage = "Configure display_key in config.json";
            Serial.printf("ERROR: Invalid display key detected: '%s'\n", displayKey.c_str());
          }
          // Update refresh interval
          refreshIntervalSeconds = (uint32_t)refresh;
          // Update business hours
          enableBusinessHours = (enableBiz != 0);
          businessHoursStart = bizStart;
          businessHoursEnd = bizEnd;
          deepSleepWeekends = (sleepWeekend != 0);
          // Update interactive features
          enableInstantReserve = (instantReserve != 0);
          enableExtendMeeting = (extendMeeting != 0);
          enableEndEarly = (endEarly != 0);
          batterySaverSleepSeconds = (uint32_t)batterySaverSleep;
          // Battery saver auto-enabled if all interactive features are disabled
          enableBatterySaver = (!enableInstantReserve && !enableExtendMeeting && !enableEndEarly);
          Serial.println("Loaded SD config.json");
          LOG("Business hours: %s, %d:00 - %d:00, weekends=%s\n", enableBusinessHours ? "enabled" : "disabled", businessHoursStart, businessHoursEnd, deepSleepWeekends ? "sleep" : "active");
          LOG("Interactive features: reserve=%s extend=%s endEarly=%s\n", enableInstantReserve ? "yes" : "no", enableExtendMeeting ? "yes" : "no", enableEndEarly ? "yes" : "no");
          LOG("Battery saver: %s (auto), sleep=%lus\n", enableBatterySaver ? "enabled" : "disabled", (unsigned long)batterySaverSleepSeconds);
        } else {
          Serial.println("ERROR: Failed to parse config.json");
        }
      }
    }
  }

  // Detect fonts on SD (paths at card root)
  if (SD.exists(FONT_REGULAR_PATH) && SD.exists(FONT_BOLD_PATH)) {
    hasTTFFonts = true;
    LOG("TTF fonts found on SD\n");
  } else {
    LOG("TTF fonts not found; using built-ins\n");
  }

  // Detect logo on SD (optional) - try PNG first, then JPG
  if (SD.exists("/logo.png")) {
    File logoFile = SD.open("/logo.png", FILE_READ);
    if (logoFile && logoFile.size() > 0) {
      hasLogo = true;
      logoIsJpg = false;
      logoHeight = 60; // Fixed max height for logo (adjustable in Config.h if needed)
      LOG("Logo PNG found on SD (file size: %d bytes, max height: %dpx)\n", logoFile.size(), logoHeight);
      logoFile.close();
    } else {
      Serial.println("ERROR: logo.png found but could not open or is empty");
      if (logoFile) logoFile.close();
    }
  } else if (SD.exists("/logo.jpg")) {
    File logoFile = SD.open("/logo.jpg", FILE_READ);
    if (logoFile && logoFile.size() > 0) {
      hasLogo = true;
      logoIsJpg = true;
      logoHeight = 60; // Fixed max height for logo
      LOG("Logo JPG found on SD (file size: %d bytes, max height: %dpx)\n", logoFile.size(), logoHeight);
      logoFile.close();
    } else {
      Serial.println("ERROR: logo.jpg found but could not open or is empty");
      if (logoFile) logoFile.close();
    }
  } else {
    LOG("No logo.png or logo.jpg found on SD\n");
  }

  // Check for critically low battery (below 3.0V = ~5%)
  updateVoltageString();
  uint32_t mv = M5.getBatteryVoltage();
  if (mv > 100 && mv < 3000) { // sanity check: not 0, but dangerously low
    hasCriticalError = true;
    errorMessage = "Low battery. Please charge device.";
    Serial.printf("ERROR: Critically low battery: %dmV\n", mv);
  }

  // Default placeholders when offline
  currentMeetingTitle = "Daily Standup";
  currentMeetingTime  = "10:00 - 10:30";
  nextMeetingTitle    = "Design Sync";
  nextMeetingTime     = "10:45 - 11:30";

  // Try fetch schedule (skip if already in critical error state)
  if (!hasCriticalError && !fetchSchedule()) {
    LOG("Initial fetchSchedule failed; using placeholders\n");
  }

  drawUI();

  // Push to display using a high-quality grayscale mode
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
  // Allow time for e-ink refresh to complete (GC16 mode takes longer)
  delay(4000);

  // Touch is initialized by M5.begin when touch is enabled; just set rotation
  M5.TP.SetRotation(SCREEN_ROTATION);
  
  // Enter appropriate sleep mode (deep sleep for battery saver, light sleep otherwise)
  LOG("Setup complete, entering sleep mode\n");
  rearmAndSleep();
}

void loop() {
  logWakeCause();
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  
  // On wake: re-enable any power rails we disabled before sleep
  #if EPD_POWER_OFF_IN_SLEEP
  M5.enableEPDPower();
  #endif
  #if EXTPWR_OFF_IN_SLEEP
  M5.enableEXTPower();
  #endif
  delay(10); // allow rails to stabilize
  
  // If button wake during deep sleep (battery saver or outside business hours), enter temporary wake mode
  if (wakeupCause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("[loop] BUTTON wake (EXT1) during deep sleep: entering temporary wake mode (30s)");
    inTemporaryWakeMode = true;
    temporaryWakeStartMs = millis();
    
    // Initialize touch panel for temporary wake mode
    M5.TP.SetRotation(SCREEN_ROTATION);
    Serial.println("[loop] Touch panel initialized for temporary wake");
    
    updateVoltageString();
    if (!fetchSchedule()) {
      Serial.println("[loop] fetchSchedule FAILED; keeping previous UI");
    }
    drawUI(); // Will show buttons because inTemporaryWakeMode = true
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    delay(4000); // Allow e-ink refresh to complete (GC16 mode takes longer)
    Serial.println("[loop] Temporary wake mode active - touch enabled for 30s");
    // Fall through to normal touch handling loop below
  } else if (wakeupCause != ESP_SLEEP_WAKEUP_TIMER) {
    // Unexpected wake cause in battery saver mode - log and continue to timer refresh
    Serial.printf("[loop] WARNING: Unexpected wake cause during battery saver: %d\n", (int)wakeupCause);
  }
  
  // If this was a timer wake, refresh data and UI immediately
  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
    LOG("[loop] TIMER wake: refreshing schedule and UI\n");
    // Note: NTP will auto-sync if it's been >24h since last sync
    updateVoltageString();
    LOG("[loop] Voltage updated: %s\n", voltstr.c_str());
    if (!fetchSchedule()) {
      LOG("[loop] fetchSchedule FAILED; keeping previous UI\n");
    } else {
      LOG("[loop] fetchSchedule SUCCESS\n");
    }
    LOG("[loop] Drawing UI...\n");
    drawUI();
    LOG("[loop] Pushing canvas to display...\n");
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    LOG("[loop] Canvas pushed, waiting for e-ink refresh to complete...\n");
    delay(4000); // Allow e-ink display time to complete physical refresh (GC16 mode takes longer)
    LOG("[loop] Re-arming sleep after timer wake\n");
    rearmAndSleep();
    return;
  }
  // Check if temporary wake mode has expired
  if (inTemporaryWakeMode && (millis() - temporaryWakeStartMs > TEMPORARY_WAKE_DURATION_MS)) {
    Serial.println("[loop] Temporary wake mode expired, returning to deep sleep");
    inTemporaryWakeMode = false;
    rearmAndSleep(); // Will deep sleep again (outside business hours)
    return;
  }
  
  // In temporary wake mode: stay awake and poll for touches continuously
  if (inTemporaryWakeMode) {
    tp_finger_t finger;
    bool touched = M5.TP.available();
    if (millis() < touchCooldownUntilMs) {
      delay(100); // Wait during cooldown
      return; // Loop again
    }
    if (!touched) {
      delay(100); // Poll every 100ms
      return; // Loop again to check for timeout or touch
    }
    // Touch detected - process it
    M5.TP.update();
    if (M5.TP.isFingerUp()) {
      delay(100);
      return; // Loop again
    }
    // Finger is down, process touch (fall through to touch handling below)
  }
  
  // After light sleep wake: poll touch briefly, handle action tap
  tp_finger_t finger;
  bool touched = M5.TP.available();
  if (millis() < touchCooldownUntilMs) {
    // Skip processing touches during cooldown to avoid double actions
    rearmAndSleep();
    return;
  }
  if (touched) {
    M5.TP.update();
    if (!M5.TP.isFingerUp()) {
      finger = M5.TP.readFinger(0);
      int x = finger.x;
      int y = finger.y;
      // Determine action areas (must match drawUI calculations)
      const int margin = 24;
      const int contentWidth = PORTRAIT_WIDTH - margin * 2;
      const int actionsH = 108;
      const int buttonGap = 12;
      const int bottomMargin = margin; // 24px

      // Compute what is drawn based on flags AND config settings
      String firstAction = "";
      String secondAction = "";
      if (occupied) {
        if (canExtend && enableExtendMeeting) firstAction = "Extend";
        if (canEndMeeting && enableEndEarly) {
          if (firstAction.length() == 0) firstAction = "End Early"; else secondAction = "End Early";
        }
      } else {
        if (canInstantReserve && enableInstantReserve) firstAction = "Reserve";
      }

      bool didAction = false;
      auto inRect = [&](int rx, int ry, int rw, int rh) { return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh; };
      if (firstAction.length() > 0 && secondAction.length() == 0) {
        // Single button layout (full-width)
        int singleY = PORTRAIT_HEIGHT - actionsH - bottomMargin;
        if (inRect(margin, singleY, contentWidth, actionsH)) {
          drawActionBarPressed(margin, singleY, contentWidth, actionsH, firstAction.c_str());
          canvas.pushCanvas(0, 0, UPDATE_MODE_A2);
          if (firstAction == "Reserve") didAction = postReserve15();
          else if (firstAction == "Extend") didAction = postExtend();
          else if (firstAction == "End Early") didAction = postEndEarly();
        }
      } else if (firstAction.length() > 0 && secondAction.length() > 0) {
        // Two buttons: side-by-side at bottom
        int buttonY = PORTRAIT_HEIGHT - actionsH - bottomMargin;
        int buttonWidth = (contentWidth - buttonGap) / 2;
        if (inRect(margin, buttonY, buttonWidth, actionsH)) {
          // First button (left)
          drawActionBarPressed(margin, buttonY, buttonWidth, actionsH, firstAction.c_str());
          canvas.pushCanvas(0, 0, UPDATE_MODE_A2);
          if (firstAction == "Reserve") didAction = postReserve15();
          else if (firstAction == "Extend") didAction = postExtend();
          else if (firstAction == "End Early") didAction = postEndEarly();
        }
        if (inRect(margin + buttonWidth + buttonGap, buttonY, buttonWidth, actionsH)) {
          // Second button (right)
          drawActionBarPressed(margin + buttonWidth + buttonGap, buttonY, buttonWidth, actionsH, secondAction.c_str());
          canvas.pushCanvas(0, 0, UPDATE_MODE_A2);
          if (secondAction == "End Early") didAction = postEndEarly();
          else if (secondAction == "Extend") didAction = postExtend();
        }
      }

      if (didAction) {
        // Re-draw with new state
        canvas.fillCanvas(0);
        drawUI();
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        // Allow e-ink refresh to complete before waiting for finger release
        delay(4000);
        // Wait for finger release to avoid triggering new action on updated UI
        unsigned long startWait = millis();
        while (!M5.TP.isFingerUp() && millis() - startWait < 1500) {
          M5.TP.update();
          delay(30);
        }
        // Short cooldown after action
        touchCooldownUntilMs = millis() + 1200;
        
        // If in temporary wake mode and user took an action, extend the wake time
        if (inTemporaryWakeMode) {
          temporaryWakeStartMs = millis(); // Reset timer
          Serial.println("[loop] Action taken in temporary wake mode - extended 30s");
        }
      }
    }
  }

  // Re-arm sleep quickly to save power
  rearmAndSleep();
}


