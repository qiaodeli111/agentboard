// agentboard — desk display of local coding agents (Claude Code / Codex / pi / ChatGPT)
// Board: Waveshare ESP32-S3-RLCD-4.2 (ST7305 reflective LCD, 300x400 mono, U8g2)
//
// Transport (pick one):
//   USE_WIFI 0  -> USB serial: the Mac daemon pushes one JSON line every ~10s
//                  to /dev/cu.usbmodem* (default; works without any network)
//   USE_WIFI 1  -> the ESP32 polls GET http://BOARD_HOST:BOARD_PORT/api/status
//
// Pages (KEY button GPIO18, short press = next page, long press = clear notifs):
//   0 notifications   1 all sessions   2 per-model token usage
//
// Compile FQBN: esp32:esp32:esp32s3:CDCOnBoot=cdc

#define USE_WIFI 0   // 0 = USB serial push (default), 1 = ESP polls over WiFi

#include <ArduinoJson.h>
#include <SPI.h>
#include <U8g2lib.h>
#include "secrets.h"

#if USE_WIFI
#include <WiFi.h>
#include <HTTPClient.h>
#endif

// ---------------- display ----------------
// Pin remap per Waveshare wiki: SCK=11, MOSI=12, CS=40, DC=5, RST=41
// U8G2_R1 = landscape (400x300). If it ends up upside-down, use U8G2_R3.
U8G2_ST7305_300X400_F_4W_HW_SPI u8g2(U8G2_R1, /*cs=*/40, /*dc=*/5, /*reset=*/41);

#define W 400
#define H 300
#define KEY_PIN 18                  // board KEY button (pullup, pressed = LOW)
#define BOOT_PIN 0                  // BOOT button works too (pullup, pressed = LOW)

#define F_HEAD  u8g2_font_9x15B_tr
#define F_ASCII u8g2_font_6x10_tr
#define F_CJK   u8g2_font_wqy12_t_gb2312
#define F_BIG   u8g2_font_logisoso62_tn   // home-page clock
#define F_MID   u8g2_font_wqy16_t_gb2312  // date line on home page

// ---------------- data ----------------
constexpr int MAX_SESSIONS = 12;
constexpr int MAX_NOTIFS = 8;
constexpr int MAX_MODELS = 12;

struct Session {
  char agent[8];
  char state[8];
  char project[24];
  char title[64];
  char model[16];
  int  age;
};
struct Notif {
  char agent[8];
  char project[24];
  char title[64];
  int  age;
  bool seen;
};
struct ModelRow {
  char agent[8];
  char model[24];
  double tokens;
};
struct BoardData {
  char clock[8];
  char host[24];
  char datecn[20];
  int  n;
  Session s[MAX_SESSIONS];
  int  nn;
  Notif notif[MAX_NOTIFS];
  int  nm;
  ModelRow model[MAX_MODELS];
  bool hasCodex; float gpt5h, gptWeek; int32_t gptResetIn; int32_t gptSnapAge; char gptPlan[8];
  bool hasClaude; float claudeTok; float claudeCost; int claudeSessions;
  bool hasPi; float piTok; int piSessions;
  int procClaude, procCodex, procPi; bool chatgptApp;
};
BoardData data;
uint32_t lastDataMs = 0;

#if USE_WIFI
const uint32_t POLL_MS = 10000;
const uint32_t REPAINT_MS = 60000;
uint32_t lastPoll = 0, lastPaint = 0;
String lastHash;
#endif

int page = 0;                       // 0 notifs, 1 sessions, 2 models

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

void fmtTokens(double v, char* out, size_t n) {
  if (v >= 1e9)      snprintf(out, n, "%.2fG", v / 1e9);
  else if (v >= 1e6) snprintf(out, n, "%.1fM", v / 1e6);
  else if (v >= 1e3) snprintf(out, n, "%.0fk", v / 1e3);
  else               snprintf(out, n, "%d", (int)v);
}

void drawClipped(int x, int y, const char* s, int maxW) {
  static char buf[96];
  snprintf(buf, sizeof(buf), "%s", s);
  int len = strlen(buf);
  while (len > 0 && u8g2.getUTF8Width(buf) > maxW) {
    int i = len - 1;
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

// 14x11 agent icons at (x, yTop). claude = 8-ray starburst, codex = terminal
// ">_ box", pi = Greek π, chatgpt = speech bubble. Honors current draw color.
void drawAgentIcon(int x, int yTop, const char* a) {
  if (!strcmp(a, "claude")) {
    int cx = x + 6, cy = yTop + 5;
    u8g2.drawLine(cx - 5, cy, cx + 5, cy);
    u8g2.drawLine(cx, cy - 5, cx, cy + 5);
    u8g2.drawLine(cx - 3, cy - 3, cx + 3, cy + 3);
    u8g2.drawLine(cx - 3, cy + 3, cx + 3, cy - 3);
  } else if (!strcmp(a, "codex")) {
    u8g2.drawFrame(x, yTop, 13, 11);
    u8g2.drawLine(x + 3, yTop + 3, x + 6, yTop + 5);
    u8g2.drawLine(x + 6, yTop + 5, x + 3, yTop + 7);
    u8g2.drawLine(x + 7, yTop + 7, x + 10, yTop + 7);
  } else if (!strcmp(a, "pi")) {
    u8g2.drawLine(x + 1, yTop + 2, x + 12, yTop + 2);
    u8g2.drawLine(x + 2, yTop + 2, x + 2, yTop + 10);
    u8g2.drawLine(x + 11, yTop + 2, x + 11, yTop + 10);
  } else if (!strcmp(a, "chatgpt")) {
    u8g2.drawRFrame(x, yTop + 1, 13, 8, 2);
    u8g2.drawLine(x + 3, yTop + 9, x + 3, yTop + 11);
    u8g2.drawLine(x + 3, yTop + 11, x + 6, yTop + 9);
  } else {
    u8g2.drawFrame(x, yTop, 11, 11);
    u8g2.drawLine(x + 2, yTop + 5, x + 8, yTop + 5);
  }
}

// icon + short name; returns x after the tag
int drawAgentTag(int x, int baseline, const char* a) {
  drawAgentIcon(x, baseline - 10, a);
  u8g2.setFont(F_ASCII);
  u8g2.drawStr(x + 17, baseline, agentShort(a));
  return x + 17 + u8g2.getStrWidth(agentShort(a));
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
  strlcpy(nd.datecn, doc["date_cn"] | "", sizeof(nd.datecn));
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
    int32_t ts = gpt["ts"] | 0;
    nd.gptResetIn = resets > gen ? resets - gen : 0;
    nd.gptSnapAge = ts ? (gen > ts ? gen - ts : 0) : 0;
    strlcpy(nd.gptPlan, gpt["plan"] | "?", sizeof(nd.gptPlan));
  }
  JsonObject cl = doc["quota"]["claude"];
  nd.hasClaude = !cl.isNull();
  if (nd.hasClaude) {
    nd.claudeTok = (float)((double)(cl["today_tokens"] | 0));
    nd.claudeCost = (float)((double)(cl["today_cost"] | 0));
    nd.claudeSessions = cl["sessions_today"] | 0;
  }
  JsonObject pi = doc["quota"]["pi"];
  nd.hasPi = !pi.isNull();
  if (nd.hasPi) {
    nd.piTok = (float)((double)(pi["today_tokens"] | 0));
    nd.piSessions = pi["sessions_today"] | 0;
  }

  int i = 0;
  for (JsonObject item : doc["shown"].as<JsonArray>()) {
    if (i >= MAX_SESSIONS) break;
    strlcpy(nd.s[i].agent, item["agent"] | "?", sizeof(nd.s[i].agent));
    strlcpy(nd.s[i].state, item["state"] | "?", sizeof(nd.s[i].state));
    strlcpy(nd.s[i].project, item["project"] | "?", sizeof(nd.s[i].project));
    strlcpy(nd.s[i].title, item["title"] | "", sizeof(nd.s[i].title));
    strlcpy(nd.s[i].model, item["model"] | "", sizeof(nd.s[i].model));
    nd.s[i].age = item["age"] | 0;
    i++;
  }
  nd.n = i;

  i = 0;
  for (JsonObject item : doc["notifs"].as<JsonArray>()) {
    if (i >= MAX_NOTIFS) break;
    strlcpy(nd.notif[i].agent, item["agent"] | "?", sizeof(nd.notif[i].agent));
    strlcpy(nd.notif[i].project, item["project"] | "?", sizeof(nd.notif[i].project));
    strlcpy(nd.notif[i].title, item["title"] | "", sizeof(nd.notif[i].title));
    nd.notif[i].age = item["age"] | 0;
    nd.notif[i].seen = item["seen"] | false;
    i++;
  }
  nd.nn = i;

  i = 0;
  for (JsonObject item : doc["models"].as<JsonArray>()) {
    if (i >= MAX_MODELS) break;
    strlcpy(nd.model[i].agent, item["agent"] | "?", sizeof(nd.model[i].agent));
    strlcpy(nd.model[i].model, item["model"] | "?", sizeof(nd.model[i].model));
    nd.model[i].tokens = (double)(item["tokens"] | 0);
    i++;
  }
  nd.nm = i;

  data = nd;
  lastDataMs = millis();
  return true;
}

// ---------------- ui: shared header/footer ----------------
void drawHeader() {
  char buf[64];
  u8g2.setFont(F_HEAD);
  u8g2.drawUTF8(4, 15, "AgentBoard");
  u8g2.setFont(F_ASCII);
  if (lastDataMs) {
    snprintf(buf, sizeof(buf), "%s %s", data.host, data.clock);
    u8g2.drawStr(W - u8g2.getStrWidth(buf) - 4, 15, buf);
  }
  u8g2.drawHLine(0, 20, W);
}

void drawFooter(int activePage) {
  char buf[72];
  u8g2.setFont(F_ASCII);
  u8g2.drawHLine(0, H - 20, W);
  // page tabs, active one inverted
  const char* tabs[3] = {"1 通知", "2 会话", "3 模型"};
  int x = 6;
  for (int i = 0; i < 3; i++) {
    u8g2.setFont(F_CJK);
    int w = u8g2.getUTF8Width(tabs[i]) + 8;
    if (i == activePage) {
      u8g2.drawBox(x, H - 19, w, 19);
      u8g2.setDrawColor(0);
      u8g2.drawUTF8(x + 4, H - 6, tabs[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawUTF8(x + 4, H - 6, tabs[i]);
    }
    x += w + 6;
  }
  // right side: counts
  u8g2.setFont(F_ASCII);
  if (activePage == 1) {
    int nRun = 0, nIdle = 0;
    for (int i = 0; i < data.n; i++) {
      if (!strcmp(data.s[i].state, "running")) nRun++;
      else if (!strcmp(data.s[i].state, "idle")) nIdle++;
    }
    snprintf(buf, sizeof(buf), "run %d idle %d", nRun, nIdle);
  } else {
    snprintf(buf, sizeof(buf), "c%d x%d p%d %s", data.procClaude, data.procCodex,
             data.procPi, data.chatgptApp ? "GPT.app" : "");
  }
  u8g2.drawStr(W - u8g2.getStrWidth(buf) - 4, H - 6, buf);
}

// quota block shared by the home page (drawn at a fixed y at the bottom)
void drawQuota(int yTop) {
  char buf[72];
  int y = yTop;
  u8g2.drawHLine(0, y - 6, W);
  if (data.hasCodex) {
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(4, y + 10, "ChatGPT");
    u8g2.setFont(F_CJK);
    snprintf(buf, sizeof(buf), "(%s)", data.gptPlan);
    u8g2.drawUTF8(50, y + 10, buf);
    u8g2.setFont(F_ASCII);
    drawBar(90, y + 2, 56, 9, data.gptResetIn > 0 ? data.gpt5h : 0);
    if (data.gptResetIn > 0) {
      snprintf(buf, sizeof(buf), "%.0f%%", data.gpt5h);
      u8g2.drawStr(152, y + 10, buf);
      u8g2.setFont(F_CJK);
      u8g2.drawUTF8(178, y + 10, "重置");
      u8g2.setFont(F_ASCII);
      if (data.gptResetIn >= 3600) {
        snprintf(buf, sizeof(buf), "%ldh%02dm",
                 (long)(data.gptResetIn / 3600), (long)((data.gptResetIn % 3600) / 60));
      } else {
        snprintf(buf, sizeof(buf), "%ldm", (long)(data.gptResetIn / 60));
      }
      u8g2.drawStr(202, y + 10, buf);
    } else {
      // window already reset: the percent above is from the old window
      u8g2.setFont(F_CJK);
      u8g2.drawUTF8(152, y + 10, "已重置·待刷新");
    }
    // snapshot age so stale numbers are obvious
    u8g2.setFont(F_ASCII);
    char age[12];
    if (data.gptSnapAge < 120)       snprintf(age, sizeof(age), "now");
    else if (data.gptSnapAge < 3600) snprintf(age, sizeof(age), "%ldm", (long)(data.gptSnapAge / 60));
    else                             snprintf(age, sizeof(age), "%ldh", (long)(data.gptSnapAge / 3600));
    snprintf(buf, sizeof(buf), "%s", age);
    u8g2.drawStr(262, y + 10, buf);
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(288, y + 10, "每周");
    u8g2.setFont(F_ASCII);
    drawBar(314, y + 2, 56, 9, data.gptWeek);
    snprintf(buf, sizeof(buf), "%.0f%%", data.gptWeek);
    u8g2.drawStr(376, y + 10, buf);
    y += 18;
  }
  if (data.hasClaude) {
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(4, y + 10, "CLAUDE");
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(46, y + 10, "今日");
    u8g2.setFont(F_ASCII);
    char tok[16];
    fmtTokens(data.claudeTok, tok, sizeof(tok));
    snprintf(buf, sizeof(buf), "%s tok", tok);
    u8g2.drawStr(76, y + 10, buf);
    snprintf(buf, sizeof(buf), "$%.2f", data.claudeCost);
    u8g2.drawStr(146, y + 10, buf);
    snprintf(buf, sizeof(buf), "%d sess", data.claudeSessions);
    u8g2.drawStr(200, y + 10, buf);
    y += 16;
  }
  if (data.hasPi) {
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(4, y + 10, "PI");
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(24, y + 10, "今日");
    u8g2.setFont(F_ASCII);
    char tok[16];
    fmtTokens(data.piTok, tok, sizeof(tok));
    snprintf(buf, sizeof(buf), "%s tok", tok);
    u8g2.drawStr(56, y + 10, buf);
    snprintf(buf, sizeof(buf), "%d sess", data.piSessions);
    u8g2.drawStr(126, y + 10, buf);
  }
}

// ---------------- ui: page 0 notifications + big clock + quota ----------------
void renderNotifs() {
  u8g2.clearBuffer();
  char buf[72];
  drawHeader();

  // quota block pinned above the footer: codex 18 + claude 16 + pi ~16 + gaps
  int quotaTop = H - 22 - 18 - 16 - 16 - 8;      // ~ 300-80 = 220
  int notifBottom = quotaTop - 8;

  if (data.nn == 0) {
    // clock-first: big time centered in the free area
    u8g2.setFont(F_BIG);
    u8g2.drawStr(10, 132, data.clock);
    u8g2.setFont(F_MID);
    u8g2.drawUTF8(12, 168, data.datecn);
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(12, 190, "no pending alerts");
  } else {
    // compact clock top-left, date to its right, notifications below
    u8g2.setFont(F_BIG);
    u8g2.drawStr(8, 80, data.clock);
    int cw = u8g2.getStrWidth(data.clock);
    u8g2.setFont(F_MID);
    u8g2.drawUTF8(16 + cw, 52, data.datecn);
    u8g2.setFont(F_CJK);
    snprintf(buf, sizeof(buf), "通知 %d 条", data.nn);
    u8g2.drawUTF8(16 + cw, 80, buf);

    int y = 96;
    for (int i = 0; i < data.nn && y < notifBottom; i++) {
      Notif& nf = data.notif[i];
      if (!nf.seen) {
        // fresh: solid inverted block, two lines
        int bh = 40;
        if (y + bh > notifBottom) break;
        u8g2.drawBox(4, y, W - 8, bh);
        u8g2.setDrawColor(0);
        u8g2.setFont(F_ASCII);
        drawAgentIcon(10, y + 4, nf.agent);
        int x = 10 + 17 + u8g2.getStrWidth(agentShort(nf.agent)) + 6;
        u8g2.setFont(F_CJK);
        drawClipped(x, y + 14, nf.project, 170);
        u8g2.drawUTF8(230, y + 14, "跑完等你");
        u8g2.setFont(F_ASCII);
        fmtAge(nf.age, buf, sizeof(buf));
        u8g2.drawStr(W - u8g2.getStrWidth(buf) - 12, y + 14, buf);
        u8g2.setFont(F_CJK);
        drawClipped(12, y + 32, nf.title, W - 24);
        u8g2.setDrawColor(1);
        y += bh + 4;
      } else {
        // viewed on the computer: outline row, single line
        if (y + 20 > notifBottom) break;
        u8g2.drawFrame(4, y, W - 8, 20);
        u8g2.setFont(F_ASCII);
        drawAgentIcon(9, y + 4, nf.agent);
        int x = 9 + 17 + u8g2.getStrWidth(agentShort(nf.agent)) + 6;
        u8g2.setFont(F_CJK);
        drawClipped(x, y + 14, nf.project, 110);
        x += 116;
        u8g2.drawUTF8(x, y + 14, "已读");
        x += 30;
        fmtAge(nf.age, buf, sizeof(buf));
        u8g2.setFont(F_ASCII);
        int ageW = u8g2.getStrWidth(buf);
        u8g2.drawStr(W - ageW - 12, y + 14, buf);
        u8g2.setFont(F_CJK);
        drawClipped(x, y + 14, nf.title, W - 12 - ageW - 12 - x);
        y += 24;
      }
    }
  }

  drawQuota(quotaTop);
  drawFooter(0);
  u8g2.sendBuffer();
}

// ---------------- ui: page 1 sessions (list only) ----------------
void renderSessions() {
  u8g2.clearBuffer();
  char buf[72];
  drawHeader();

  int y = 36;
  int rowsLeft = (H - 26 - y) / 19;
  for (int i = 0; i < data.n && rowsLeft > 0; i++, rowsLeft--) {
    Session& s = data.s[i];
    int x = drawAgentTag(4, y, s.agent);
    x += 5;
    u8g2.setFont(F_CJK);
    drawClipped(x, y, s.project, 92);
    x += 96;
    u8g2.setFont(F_ASCII);
    fmtAge(s.age, buf, sizeof(buf));
    int ageW = u8g2.getStrWidth(buf);
    u8g2.drawStr(W - ageW - 6, y, buf);
    drawStateGlyph(W - ageW - 22, y, s.state);
    u8g2.setFont(F_CJK);
    drawClipped(x, y, s.title, W - 28 - ageW - 6 - x);
    y += 19;
  }

  drawFooter(1);
  u8g2.sendBuffer();
}

// ---------------- ui: page 2 model usage ----------------
void renderModels() {
  u8g2.clearBuffer();
  char buf[72];
  drawHeader();

  u8g2.setFont(F_CJK);
  u8g2.drawUTF8(4, 36, "今日各模型 token 用量");
  u8g2.drawHLine(0, 42, W);

  int y = 58;
  for (int i = 0; i < data.nm; i++) {
    if (y > H - 30) break;
    ModelRow& mr = data.model[i];
    drawAgentTag(4, y, mr.agent);
    u8g2.setFont(F_ASCII);
    u8g2.drawStr(74, y, mr.model);
    char tok[16];
    fmtTokens(mr.tokens, tok, sizeof(tok));
    snprintf(buf, sizeof(buf), "%s tok", tok);
    u8g2.drawStr(W - u8g2.getStrWidth(buf) - 8, y, buf);
    // proportional bar under the row
    if (data.model[0].tokens > 0) {
      drawBar(74, y + 2, 180, 5, mr.tokens / data.model[0].tokens * 100.0f);
    }
    y += 18;
  }
  if (data.nm == 0) {
    u8g2.setFont(F_CJK);
    u8g2.drawUTF8(12, 70, "今日暂无用量数据");
  }
  drawFooter(2);
  u8g2.sendBuffer();
}

void render() {
  switch (page) {
    case 0: renderNotifs(); break;
    case 1: renderSessions(); break;
    default: renderModels(); break;
  }
}

bool renderIfChanged(const char* newHash) {
  static char lastHash[512] = "";
  if (strcmp(lastHash, newHash) == 0) return false;
  strlcpy(lastHash, newHash, sizeof(lastHash));
  render();
  return true;
}

void buildHash(char* out, size_t n) {
  snprintf(out, n, "%d|%s|%s|%.0f|%.0f|%d|%d|%d|", page, data.clock, data.datecn,
           data.gpt5h, data.gptWeek,
           (int)data.claudeTok, (int)data.piTok,
           data.procClaude + data.procCodex + data.procPi);
  size_t len = strlen(out);
  for (int i = 0; i < data.n && len < n - 40; i++) {
    snprintf(out + len, n - len, "%s%d|%.14s|", data.s[i].state, data.s[i].age / 60,
             data.s[i].title);
    len = strlen(out);
  }
  for (int i = 0; i < data.nn && len < n - 40; i++) {
    snprintf(out + len, n - len, "N%s%d%d|%.14s|", data.notif[i].agent,
             data.notif[i].seen ? 1 : 0, data.notif[i].age / 60,
             data.notif[i].title);
    len = strlen(out);
  }
}

// ---------------- button ----------------
void handleButton() {
  static uint32_t pressStart = 0;
  static bool longFired = false;
  // either KEY (GPIO18) or BOOT (GPIO0), pressed = LOW
  bool pressed = digitalRead(KEY_PIN) == LOW || digitalRead(BOOT_PIN) == LOW;
  uint32_t now = millis();

  if (pressed && pressStart == 0) {
    pressStart = now;
    longFired = false;
  } else if (pressed && pressStart && !longFired && now - pressStart > 800) {
    longFired = true;
    Serial.print("ACK\n");          // tell the Mac to clear notifications
    // immediate visual confirmation the press registered
    if (lastDataMs) {
      u8g2.clearBuffer();
      u8g2.drawBox(0, 0, W, H);
      u8g2.setDrawColor(0);
      u8g2.setFont(F_CJK);
      u8g2.drawUTF8(60, H / 2 + 6, "已发送清除，通知将消失…");
      u8g2.setDrawColor(1);
      u8g2.sendBuffer();
    }
  } else if (!pressed && pressStart) {
    if (!longFired && now - pressStart > 30 && now - pressStart < 800) {
      if (page == 0) {
        Serial.print("SEEN\n");      // leaving the notification page = viewed
      }
      page = (page + 1) % 3;
      if (lastDataMs) render();
    }
    pressStart = 0;
  }
}

// ---------------- setup / loop ----------------
#if USE_WIFI
bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
String httpBody;
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
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(BOOT_PIN, INPUT_PULLUP);
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
  handleButton();
  delay(20);
#else
  // ---- USB serial push: drain continuously, act on complete lines ----
  static String line;
  bool gotLine = false;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { gotLine = true; break; }
    line += c;
    if (line.length() > 16384) line = "";
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
  handleButton();
  static uint32_t lastTick = 0;
  if (lastDataMs && millis() - lastTick > 60000) {
    lastTick = millis();
    render();     // keep relative ages honest
  }
  delay(Serial.available() ? 0 : 5);
#endif
}
