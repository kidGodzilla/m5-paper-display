#include <M5EPD.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Config.h"

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

M5EPD_Canvas canvas(&M5.EPD);

// Optional TrueType fonts from SD card (paths defined in Config.h)
static bool hasTTFFonts = false;

static void useTTF(const char* path, int pixelSize) {
  if (!hasTTFFonts) return;
  canvas.loadFont(path, SD);
  canvas.createRender(pixelSize, 256);
  canvas.setTextSize(pixelSize);
  canvas.setTextColor(TEXT_COLOR_SHADE); // dark text
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
  canvas.setTextDatum(ML_DATUM);
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 44);
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(5);
  }
  canvas.drawString(label, x + 24, y + h / 2 + 1);
  canvas.setTextDatum(TL_DATUM);
}

// Pressed-state variant for immediate feedback
void drawActionBarPressed(int x, int y, int w, int h, const char* label) {
  const int r = 2;
  // Slightly darker bar to indicate press
  canvas.fillRoundRect(x, y, w, h, r, BUTTON_PRESSED_SHADE);
  canvas.setTextDatum(ML_DATUM);
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 44);
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(5);
  }
  // Draw label in white on dark bar
  uint8_t prevColor = 14;
  canvas.setTextColor(0);
  canvas.drawString(label, x + 24, y + h / 2 + 1);
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

  // Top spacing
  int y = 72;

  // Spacer
  y += 8;

  // Battery indicator (top-right)
  if (SHOW_BATTERY) {
    // Read battery voltage and map to percent (3300-4350mV)
    uint32_t mv = M5.getBatteryVoltage();
    if (mv < 3300) mv = 3300; if (mv > 4350) mv = 4350;
    int pct = (int)((mv - 3300) * 100 / (4350 - 3300));
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;

    int bx = PORTRAIT_WIDTH - 24 - 80; // right margin 24, width ~80
    int by = 24;                        // top margin
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

  // Room name (large, left-aligned, bigger scale)
  y += 8;
  canvas.setTextDatum(ML_DATUM);
  if (hasTTFFonts) {
    useTTF(FONT_BOLD_PATH, 68); // slightly reduced
  } else {
    canvas.setTextFont(2);
    canvas.setTextSize(6);
  }
  canvas.drawString(roomName.c_str(), margin, y);
  canvas.setTextDatum(TL_DATUM);

  // Current meeting block (prominent)
  // Current meeting (text-first, generous spacing)
  y += 88; // more spacing before Now
  canvas.setTextDatum(ML_DATUM);
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 36);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString("Now", margin, y);
  y += 68;
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

    // Approximate width-based wrap at current title font size (64px)
    const float approxCharWidth = 0.56f; // FreeSans ~0.56 * px
    const float fontPx = 64.0f;
    int maxChars = (int)((float)contentWidth / (fontPx * approxCharWidth));
    if (maxChars < 8) maxChars = 8;

    if ((int)subject.length() > maxChars) {
      int split = subject.lastIndexOf(' ', maxChars);
      if (split < 0) split = maxChars;
      String line1 = subject.substring(0, split);
      String line2 = subject.substring(split + 1);
      canvas.drawString(line1.c_str(), margin, y);
      y += 66;
      canvas.drawString(line2.c_str(), margin, y);
    } else {
      canvas.drawString(subject.c_str(), margin, y);
    }
  }
  y += 64;
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 40);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString(currentMeetingTime.c_str(), margin, y);
  canvas.setTextDatum(TL_DATUM);

  // Next meeting block (outlined)
  y += 96; // more spacing before Next
  canvas.setTextDatum(ML_DATUM);
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 36);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString(nextMeetingLabel.c_str(), margin, y);
  y += 59;
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
  y += 56;
  if (hasTTFFonts) {
    useTTF(FONT_REGULAR_PATH, 40);
  } else {
    canvas.setTextFont(1);
    canvas.setTextSize(4);
  }
  canvas.drawString(nextMeetingTime.c_str(), margin, y);
  canvas.setTextDatum(TL_DATUM);

  // Actions area at bottom
  // Actions: two full-width bars at bottom
  int actionsH = 108;
  int actionsY = PORTRAIT_HEIGHT - actionsH * 2 - 64;

  // Decide actions based on flags
  String firstAction = "";
  String secondAction = "";
  if (occupied) {
    if (canExtend) firstAction = "Extend";
    if (canEndMeeting) {
      if (firstAction.length() == 0) firstAction = "End Early"; else secondAction = "End Early";
    }
  } else {
    if (canInstantReserve) firstAction = "Reserve 15 min";
  }

  if (firstAction.length() > 0 && secondAction.length() == 0) {
    int singleY = PORTRAIT_HEIGHT - actionsH - 24; // pin to bottom
    drawActionBar(margin, singleY, contentWidth, actionsH, firstAction.c_str());
  } else {
    if (firstAction.length() > 0) {
      drawActionBar(margin, actionsY, contentWidth, actionsH, firstAction.c_str());
    }
    if (secondAction.length() > 0) {
      drawActionBar(margin, actionsY + actionsH + 20, contentWidth, actionsH, secondAction.c_str());
    }
  }
}

// Networking
static bool wifiConnected = false;

static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsidStr.c_str(), wifiPwStr.c_str());
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(200);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  return wifiConnected;
}

static void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
}

static bool fetchSchedule() {
  if (!connectWiFi()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  // Snapshot and clear action immediately to avoid accidental repeats
  int actionParam = g_action;
  g_action = 0;
  String url = String(SCHEDULE_API_BASE) + "/eink/" + displayKey + "?action=" + String(actionParam) + "&voltage=" + voltstr;
  Serial.println(url);
  if (!http.begin(client, url)) { disconnectWiFi(); return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); disconnectWiFi(); return false; }

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  disconnectWiFi();
  if (err) return false;

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
  // action already cleared above

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
  Serial.println("Booting M5Paper Meeting Room");
  M5.begin(true, true, true, true); // enable SD
  M5.EPD.SetRotation(SCREEN_ROTATION); // rotate to portrait; canvas is 540x960
  M5.EPD.Clear(true);

  canvas.createCanvas(PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
  canvas.setTextColor(TEXT_COLOR_SHADE); // dark text (foreground only)
  canvas.setTextSize(3);
  canvas.setTextDatum(TL_DATUM);

  // Load config.json from SD if present
  if (SD.begin()) {
    if (SD.exists(SD_CONFIG_PATH)) {
      File f = SD.open(SD_CONFIG_PATH, FILE_READ);
      if (f) {
        DynamicJsonDocument cfg(2048);
        DeserializationError e = deserializeJson(cfg, f);
        f.close();
        if (!e) {
          const char* ssid = cfg["wifi_ssid"] | WIFI_SSID;
          const char* pw   = cfg["wifi_password"] | WIFI_PW;
          const char* key  = cfg["display_key"] | ROOM_ID;
          int refresh      = cfg["refresh_seconds"] | (int)REFRESH_SECONDS;
          displayKey = String(key);
          roomName   = displayKey; // until API loads real name
          // Override Wi-Fi creds used by connectWiFi
          wifiSsidStr = String(ssid);
          wifiPwStr   = String(pw);
          // Update refresh interval
          refreshIntervalSeconds = (uint32_t)refresh;
          Serial.println("Loaded SD config.json");
        } else {
          Serial.println("Failed to parse config.json");
        }
      }
    }
  }

  // Detect fonts on SD (paths at card root)
  if (SD.exists(FONT_REGULAR_PATH) && SD.exists(FONT_BOLD_PATH)) {
    hasTTFFonts = true;
    Serial.println("TTF fonts found on SD");
  } else {
    Serial.println("TTF fonts not found; using built-ins");
  }

  // Default placeholders when offline
  currentMeetingTitle = "Daily Standup";
  currentMeetingTime  = "10:00 - 10:30";
  nextMeetingTitle    = "Design Sync";
  nextMeetingTime     = "10:45 - 11:30";

  // Try fetch schedule
  if (!fetchSchedule()) {
    Serial.println("fetchSchedule FAILED; using placeholders");
  }

  drawUI();

  // Push to display using a high-quality grayscale mode
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
  // Allow time to read serial logs before sleep
  delay(2000);

  // Switch to light sleep with TP INT wake to allow touch any time
  esp_sleep_enable_timer_wakeup((uint64_t)refreshIntervalSeconds * 1000000ULL);
  esp_sleep_enable_ext0_wakeup(TOUCH_INT_PIN, 0); // TP INT, active low
  // Touch is initialized by M5.begin when touch is enabled; just set rotation
  M5.TP.SetRotation(SCREEN_ROTATION);
  Serial.println("Entering light sleep");
  esp_light_sleep_start();
}

void loop() {
  // After light sleep wake: poll touch briefly, handle action tap, refresh, and go back to sleep
  tp_finger_t finger;
  bool touched = M5.TP.available();
  if (millis() < touchCooldownUntilMs) {
    // Skip processing touches during cooldown to avoid double actions
    goto sleep_again;
  }
  if (touched) {
    M5.TP.update();
    if (!M5.TP.isFingerUp()) {
      finger = M5.TP.readFinger(0);
      int x = finger.x;
      int y = finger.y;
      // Determine action areas
      const int margin = 24;
      const int contentWidth = PORTRAIT_WIDTH - margin * 2;
      const int actionsH = 108;
      int actionsYTop = PORTRAIT_HEIGHT - actionsH * 2 - 64;
      int singleY = PORTRAIT_HEIGHT - actionsH - 24;

      // Compute what is drawn based on flags
      String firstAction = "";
      String secondAction = "";
      if (occupied) {
        if (canExtend) firstAction = "Extend";
        if (canEndMeeting) {
          if (firstAction.length() == 0) firstAction = "End Early"; else secondAction = "End Early";
        }
      } else {
        if (canInstantReserve) firstAction = "Reserve 15 min";
      }

      bool didAction = false;
      auto inRect = [&](int rx, int ry, int rw, int rh) { return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh; };
      if (firstAction.length() > 0 && secondAction.length() == 0) {
        if (inRect(margin, singleY, contentWidth, actionsH)) {
          // Press feedback: darker bar + working text, fast update
          drawActionBarPressed(margin, singleY, contentWidth, actionsH, firstAction.c_str());
          canvas.pushCanvas(0, 0, UPDATE_MODE_A2);
          if (firstAction == "Reserve 15 min") didAction = postReserve15();
          else if (firstAction == "Extend") didAction = postExtend();
          else if (firstAction == "End Early") didAction = postEndEarly();
        }
      } else {
        if (firstAction.length() > 0 && inRect(margin, actionsYTop, contentWidth, actionsH)) {
          drawActionBarPressed(margin, actionsYTop, contentWidth, actionsH, firstAction.c_str());
          canvas.pushCanvas(0, 0, UPDATE_MODE_A2);
          if (firstAction == "Reserve 15 min") didAction = postReserve15();
          else if (firstAction == "Extend") didAction = postExtend();
          else if (firstAction == "End Early") didAction = postEndEarly();
        }
        if (secondAction.length() > 0 && inRect(margin, actionsYTop + actionsH + 20, contentWidth, actionsH)) {
          drawActionBarPressed(margin, actionsYTop + actionsH + 20, contentWidth, actionsH, secondAction.c_str());
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
        // Wait for finger release to avoid triggering new action on updated UI
        unsigned long startWait = millis();
        while (!M5.TP.isFingerUp() && millis() - startWait < 1500) {
          M5.TP.update();
          delay(30);
        }
        // Short cooldown after action
        touchCooldownUntilMs = millis() + 1200;
      }
    }
  }

  // Re-arm sleep quickly to save power
sleep_again:
  esp_sleep_enable_timer_wakeup((uint64_t)refreshIntervalSeconds * 1000000ULL);
  esp_sleep_enable_ext0_wakeup(TOUCH_INT_PIN, 0);
  esp_light_sleep_start();
}


