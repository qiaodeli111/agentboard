// agentboard — desk display of local coding agents (Claude Code / Codex / pi / ChatGPT)
// Board: Waveshare ESP32-S3-RLCD-4.2 (ST7305 reflective LCD, 300x400 mono, U8g2)
//
// Transport (pick one):
//   USE_WIFI 0  -> USB serial: the Mac daemon pushes one JSON line every ~10s
//                  to /dev/cu.usbmodem* (default; works without any network)
//   USE_WIFI 1  -> the ESP32 polls GET http://BOARD_HOST:BOARD_PORT/api/status
//
// Compile FQBN: esp32:esp32:esp32s3:CDCOnBoot=cdc

#define USE_WIFI 0   // 0 = USB serial push (default), 1 = ESP polls over WiFi

#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "secrets.h"

#if USE_WIFI
#include <WiFi.h>
#include <HTTPClient.h>
#endif

// ---------------- display ----------------
// Pin remap per Waveshare wiki: SCK=11, MOSI=12, CS=40, DC=5, RST=41
U8G2_ST7305_300X400_F_4W_HW_SPI u8g2(U8G2_R0, /*cs=*/40, /*dc=*/5, /*reset=*/41);

#define W 300
#define H 400

#define F_HEAD  u8g2_font_9x15B_tr
#define F_ASCII u8g2_font_6x10_tr
#define F_CJK   u8g2_font_wqy12_t_gb2312

// ---------------- data ----------------
constexpr int MAX_SESSIONS = 10;
struct Session {
  char agent[8];
  char state[8];
  char project[24];
  char title[64];
  char model[16];
  int  age;
};
struct BoardData {
  char clock[8];
  char host[24];
  int  n;
  Session s[MAX_SESSIONS];
  bool hasCodex; float gpt5h, gptWeek; int32_t gptResetIn; char gptPlan[8];
  bool hasClaude; float claudeTok; float claudeCost; int claudeSessions;
  int procClaude, procCodex, procPi; bool chatgptApp;
};
BoardData data;
uint32_t lastDataMs = 0;        // millis() of last successful parse

#if USE_WIFI
const uint32_t POLL_MS = 10000;
const uint32_t REPAINT_MS = 60000;
uint32_t lastPoll = 0, lastPaint = 0;
String lastHash;
#endif

// ---------------- helpers ----------------
const char* agentShort(const char* a) {
  if (!strcmp(a, "claude")) return "CLAUDE";
  if (!strcmp(a, "codex"))  return "CODEX";
  if (!strcmp(a, "pi"))     return "PI";
  if (!strcmp(a, "chatgpt")) return "CHATGPT";
  return a;
}

void fmtAge(int sec, char* out, size_t n) {
  if (sec < 60)         snprintf(out, n, "%ds", sec);
  else if (sec < 3600)  snprintf(out, n, "%dm", sec / 60);
  else if (sec < 86400) snprintf(out, n, "%dh", sec / 3600);
  else                  snprintf(out, n, "%dd", sec / 86400);
}

void fmtTokens(float v, char* out, size_t n) {
  if (v >= 1e6)      snprintf(out, n, "%.1fM", v / 1e6);
  else if (v >= 1e3) snprintf(out, n, "%.0fk", v / 1e3);
  else               snprintf(out, n, "%d", (int)v);
}

// draw UTF-8 string, trimming whole codepoints until it fits maxW
void drawClipped(int x, int y, const char* s, int maxW) {
  static char buf[96];
  snprintf(buf, sizeof(buf), "%s", s);
  int len = strlen(buf);
  while (len > 0 && u8g2.getUTF8Width(buf) > maxW) {
    int i = len - 1;                       // start of the last codepoint
    while (i > 0 && (buf[i] & 0xC0) == 0x80) i--;
    buf[i] = 0;
    len = i;
  }
  if (len) u8g2.drawUTF8(x, y, buf);
}

void drawBar(int x, int y, int w, int h, float pct) {
  u8g2.drawFrame(x, y, w, h);
  int fill = (int)((w - 2) * pct / 100.0f);
  if (fill > w - 2) fill = w - 2;
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
}

void drawStateGlyph(int x, int y, const char* state) {
  if (!strcmp(state, "running"))      u8g2.drawDisc(x + 4, y - 3, 4);
  else if (!strcmp(state, "idle"))    u8g2.drawCircle(x + 4, y - 3, 4);
  else { u8g2.drawDisc(x + 4, y - 3, 2); u8g2.drawCircle(x + 4, y - 3, 4); }
}

// ---------------- json -> BoardData ----------------
bool extractData(const String& payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  BoardData nd;
  memset(&nd, 0, sizeof(nd));
  strlcpy(nd.clock, doc["clock"] | "?", sizeof(nd.clock));
  strlcpy(nd.host, doc["host"] | "?", sizeof(nd.host));
  nd.chatgptApp = doc["procs"]["chatgpt_app"] | false;
  nd.procClaude = doc["procs"]["claude"] | 0;
  nd.procCodex  = doc["procs"]["codex"] | 0;
  nd.procPi     = doc["procs"]["pi"] | 0;

  JsonObject gpt = doc["quota"]["codex"];
  nd.hasCodex = !gpt.isNull();
  if (nd.hasCodex) {
    nd.gpt5h = gpt["primary"]["used_percent"] | 0.0;
    nd.gptWeek = gpt["secondary"]["used_percent"] | 0.0;
    int32_t resets = gpt["primary"]["resets_at"] | 0;
    int32_t gen = doc["generated_at"] | 0;
    nd.gptResetIn = resets > gen ? resets - gen : 0;
    strlcpy(nd.gptPlan, gpt["plan"] | "?", sizeof(nd.gptPlan));
  }
  JsonObject cl = doc["quota"]["claude"];
  nd.hasClaude = !cl.isNull();
  if (nd.hasClaude) {
    nd.claudeTok = (float)((double)(cl["today_tokens"] | 0));
    nd.claudeCost = (float)((double)(cl["today_cost"] | 0));
    nd.claudeSessions = cl["sessions_today"] | 0;
  }

  int i = 0;
  for (JsonObject item : doc["shown"].as<JsonArray>()) {
    if (i >= MAX_SESSIONS) break;
    const char* a = item["agent"] | "?";
    strlcpy(nd.s[i].agent, a, sizeof(nd.s[i].agent));
    strlcpy(nd.s[i].state, item["state"] | "?", sizeof(nd.s[i].state));
    strlcpy(nd.s[i].project, item["project"] | "?", sizeof(nd.s[i].project));
    strlcpy(nd.s[i].title, item["title"] | "", sizeof(nd.s[i].title));
    strlcpy(nd.s[i].model, item["model"] | "", sizeof(nd.s[i].model));
    nd.s[i].age = item["age"] | 0;
    i++;
  }
  nd.n = i;

  data = nd;
  lastDataMs = millis();
  return true;
}

// ---------------- ui ----------------
void render() {
  u8g2.clearBuffer();
  char buf[64];

  // header
  u8g2.setFont(F_HEAD);
  u8g2.drawUTF8(4, 15, "AgentBoard");
  u8g2.setFont(F_ASCII);
  if (lastDataMs) {
    snprintf(buf, sizeof(buf), "%s %s", data.host, data.clock);
    u8g2.drawStr(W - u8g2.getStrWidth(buf) - 4, 15, buf);
  }
  u8g2.drawHLine(0, 20, W);

  int y = 38;
  u8g2.setFont(F_ASCII);

  // quota: ChatGPT (codex account windows)
  if (data.hasCodex) {
    u8g2.drawStr(4, y, "ChatGPT");
    u8g2.setFont(F_CJK);
    snprintf(buf, sizeof(buf), " (%s)", data.gptPlan);
    u8g2.drawUTF8(52, y, buf);
    u8g2.setFont(F_ASCII);
    drawBar(104, y - 8, 84, 9, data.gpt5h);
    snprintf(buf, sizeof(buf), "%.0f%%", data.gpt5h);
    u8g2.drawStr(194, y, buf);
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(228, y, "5h重置");
    u8g2.setFont(F_ASCII);
    snprintf(buf, sizeof(buf), "%ldm", (long)(data.gptResetIn / 60));
    u8g2.drawStr(W - u8g2.getStrWidth(buf) - 4, y, buf);

    y += 15;
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(52, y, "每周");
    u8g2.setFont(F_ASCII);
    drawBar(104, y - 8, 84, 9, data.gptWeek);
    snprintf(buf, sizeof(buf), "%.0f%%", data.gptWeek);
    u8g2.drawStr(194, y, buf);
    y += 12;
  }

  // quota: Claude local stats
  if (data.hasClaude) {
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(4, y, "CLAUDE");
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(52, y, "今日");
    u8g2.setFont(F_ASCII);
    char tok[16];
    fmtTokens(data.claudeTok, tok, sizeof(tok));
    snprintf(buf, sizeof(buf), "%s tok", tok);
    u8g2.drawStr(88, y, buf);
    snprintf(buf, sizeof(buf), "$%.2f", data.claudeCost);
    u8g2.drawStr(160, y, buf);
    snprintf(buf, sizeof(buf), "%d sess", data.claudeSessions);
    u8g2.drawStr(220, y, buf);
    y += 12;
  }

  u8g2.drawHLine(0, y - 4, W);
  y += 8;

  // sessions
  int rowsLeft = (H - 30 - y) / 34;   // 16px head + 15px title + 3 gap
  for (int i = 0; i < data.n && rowsLeft > 0; i++, rowsLeft--) {
    Session& s = data.s[i];
    drawStateGlyph(4, y, s.state);
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(16, y, agentShort(s.agent));
    int x = 16 + u8g2.getStrWidth(agentShort(s.agent)) + 6;
    u8g2.setFont(F_CJK);
    drawClipped(x, y, s.project, 150);
    u8g2.setFont(F_ASCII);
    fmtAge(s.age, buf, sizeof(buf));
    u8g2.drawStr(W - u8g2.getStrWidth(buf) - 8, y, buf);

    y += 15;
    u8g2.setFont(F_CJK);
    drawClipped(16, y, s.title, W - 24);
    y += 19;
  }

  // footer
  u8g2.setFont(F_ASCII);
  u8g2.drawHLine(0, H - 22, W);
  int nRun = 0, nIdle = 0;
  for (int i = 0; i < data.n; i++) {
    if (!strcmp(data.s[i].state, "running")) nRun++;
    else if (!strcmp(data.s[i].state, "idle")) nIdle++;
  }
  if (!lastDataMs) {
    u8g2.setFont(F_CJK);
#if USE_WIFI
    u8g2.drawUTF8(4, H - 8, "等待网络数据...");
#else
    u8g2.drawUTF8(4, H - 8, "USB 等待数据...");
#endif
  } else {
    u8g2.setFont(F_CJK);
    snprintf(buf, sizeof(buf), "跑%d 闲%d", nRun, nIdle);
    u8g2.drawUTF8(4, H - 8, buf);
    int w = u8g2.getUTF8Width(buf);
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(4 + w + 8, H - 8, data.clock);
  }
  u8g2.setFont(F_ASCII);
  snprintf(buf, sizeof(buf), "c%d x%d p%d %s", data.procClaude, data.procCodex,
           data.procPi, data.chatgptApp ? "GPT.app" : "");
  u8g2.drawStr(W - u8g2.getStrWidth(buf) - 4, H - 8, buf);

  u8g2.sendBuffer();
}

// cheap change detection: the panel is reflective, redraws are cheap, but
// skipping identical frames avoids flicker and saves power
bool renderIfChanged(const char* newHash) {
  static char lastHash[512] = "";
  if (strcmp(lastHash, newHash) == 0) return false;
  strlcpy(lastHash, newHash, sizeof(lastHash));
  render();
  return true;
}

void buildHash(char* out, size_t n) {
  snprintf(out, n, "%s|%.0f|%.0f|%d|%d|%d|%d|", data.clock, data.gpt5h, data.gptWeek,
           (int)data.claudeTok, data.claudeSessions, data.procClaude + data.procCodex + data.procPi,
           data.chatgptApp);
  size_t len = strlen(out);
  for (int i = 0; i < data.n && len < n - 40; i++) {
    snprintf(out + len, n - len, "%s%d|%.14s|", data.s[i].state, data.s[i].age / 60,
             data.s[i].title);
    len = strlen(out);
  }
}

// ---------------- setup / loop ----------------
#if USE_WIFI
bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

bool fetchStatus() {
  if (!wifiUp()) return false;
  HTTPClient http;
  String url = String("http://") + BOARD_HOST + ":" + BOARD_PORT + "/api/status";
  if (!http.begin(url)) return false;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  httpBody = http.getString();
  http.end();
  return extractData(httpBody);
}
#endif

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(16 * 1024);
#if USE_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif

  SPI.begin(11, -1, 12, 40);   // remap: SCK=11, MOSI=12 (Waveshare RLCD-4.2)
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(F_HEAD);
  u8g2.drawUTF8(4, 30, "AgentBoard");
  u8g2.setFont(F_CJK);
#if USE_WIFI
  u8g2.drawUTF8(4, 60, "连接 WiFi 中...");
#else
  u8g2.drawUTF8(4, 60, "USB 等待 Mac 推送数据...");
#endif
  u8g2.sendBuffer();

  memset(&data, 0, sizeof(data));
}

#if USE_WIFI
String httpBody;
#endif

void loop() {
#if USE_WIFI
  uint32_t now = millis();
  if (wifiUp() && now - lastPoll >= POLL_MS) {
    lastPoll = now;
    if (fetchStatus()) {
      char hash[512];
      buildHash(hash, sizeof(hash));
      if (strcmp(hash, lastHash.c_str()) != 0 || now - lastPaint > REPAINT_MS) {
        lastHash = hash;
        render();
        lastPaint = now;
      }
    }
  }
  delay(20);
#else
  // ---- USB serial push: drain continuously, act on complete lines ----
  static String line;
  bool gotLine = false;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { gotLine = true; break; }
    line += c;
    if (line.length() > 16384) line = "";   // corrupt / oversized: reset
  }
  if (gotLine) {
    bool ok = extractData(line);
    line = "";
    if (ok) {
      char hash[512];
      buildHash(hash, sizeof(hash));
      renderIfChanged(hash);
    }
  }
  // repaint occasionally so relative ages stay honest even if data is stable
  static uint32_t lastTick = 0;
  if (lastDataMs && millis() - lastTick > 60000) {
    lastTick = millis();
    render();
  }
  delay(Serial.available() ? 0 : 5);
#endif
}
