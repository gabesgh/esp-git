#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <qrcode.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#include "settings_page.h"

#ifdef CONFIG_DIST
#include "config.dist.h"
#else
#include "config.h"
#endif
#include "provision.h"

// bumped by scripts/release.sh; the device compares this against the manifest
#define FW_VERSION "1.0.2"

TFT_eSPI tft;
SPIClass touchBus(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// landscape. 53 weeks only fits across the long edge.
static const int W = 320;
static const int H = 240;

enum View { VIEW_HEATMAP = 0, VIEW_COUNTER, VIEW_YEARS, VIEW_REPO, VIEW_CONTROLS, VIEW_COUNT };
static int view = VIEW_HEATMAP;

enum Effect { FX_FLASH = 0, FX_CONFETTI, FX_RING, FX_COUNT };
static int effect = PUSH_EFFECT;

// backlight runs on PWM so it can be dimmed. never allowed to reach zero: a
// black panel is indistinguishable from a dead one, and you would be swiping
// blind trying to get it back.
static const int BL_CHANNEL = 0;
static int brightness = 255;

//what the odometer is currently showing. -1 means it has never painted, which
//is the difference between "first boot" and "a push just landed".
static long shownTotal = -1;

struct Stat {
    char label[24];
    char value[16];
    char tone[10];
};

struct Note {
    char icon[10];
    char text[40];
};

// where the notes list sits under the grid
static const int NOTES_X    = 42;
static const int NOTES_Y    = 122;
static const int NOTES_STEP = 24;

struct Data {
    char cal[400];
    int  calLen = 0;
    long total = 0;
    char from[12] = "";
    char to[12] = "";

    char labels[6][8];
    long values[6];
    int  nSeries = 0;

    Stat stats[6];
    int  nStats = 0;

    Note notes[4];
    int  nNotes = 0;

    bool valid = false;
};
static Data data;

static uint32_t lastStats = 0;
static uint32_t lastPulse = 0;
static uint32_t lastOta = 0;
static long     seenSeq = -1;
static bool     dirty = true;

// Every draw starts by clearing the screen to black, so a repaint is visible as
// a flash. Fetching does not mean anything changed: the contribution graph moves
// a few times a day and the poll runs every thirty seconds. Repainting on every
// fetch made the display blink at anyone sitting near it, for nothing.
static uint32_t renderedHash = 0;
static bool     contentChanged = false;   // set by fetchStats, read by loop

static uint32_t fnv1a(const char *s, uint32_t h = 2166136261u) {
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static uint32_t fnv1aNum(long v, uint32_t h) {
    char b[24];
    snprintf(b, sizeof(b), "%ld", v);
    return fnv1a(b, h);
}
static char     status[48] = "booting";

// GitHub's dark-theme ramp. reads far better than the light one on a lit panel.
static uint16_t levelColor(int lvl) {
    switch (lvl) {
        case 1:  return tft.color565(0x0e, 0x44, 0x29);
        case 2:  return tft.color565(0x00, 0x6d, 0x32);
        case 3:  return tft.color565(0x26, 0xa6, 0x41);
        case 4:  return tft.color565(0x39, 0xd3, 0x53);
        default: return tft.color565(0x16, 0x1b, 0x22);
    }
}

static const uint16_t BG    = 0x0000;
static const uint16_t FG    = 0xFFFF;
static const uint16_t MUTED = 0x8410;

static void flashPush();

// Two things notice a push independently: the counter the git hook bumps, and
// the contribution total going up. That redundancy is deliberate, since the
// counter needs durable storage and the total does not, so between them the
// display reacts whether or not anyone set up a database.
//
// It does mean one push trips both. This gate makes that one celebration: the
// first signal wins and the second finds the door already answered. Anything
// inside the window is the same push arriving twice, not two pushes.
static uint32_t lastCelebration = 0;
static const uint32_t CELEBRATE_WINDOW_MS = 25000;

static void celebrate(const char *why) {
    if (lastCelebration && millis() - lastCelebration < CELEBRATE_WINDOW_MS) {
        Serial.printf("[fx] %s, but already celebrated %lums ago, skipping\n",
                      why, (unsigned long)(millis() - lastCelebration));
        return;
    }
    lastCelebration = millis();
    Serial.printf("[fx] celebrating: %s\n", why);
    flashPush();
}

/* ---------------------------------------------------------------- net */

static bool httpGet(const char *path, String &out) {
    String url = String(cfgHost()) + path;
    HTTPClient http;
    bool ok = false;

    if (url.startsWith("https")) {
        WiFiClientSecure *tls = new WiFiClientSecure;
        // no cert pinning. the payload is public data and the device token only
        // reads counters, so a pinned CA that expires is the bigger risk here.
        tls->setInsecure();
        if (http.begin(*tls, url)) {
            http.addHeader("x-device-token", cfgToken());
            http.setTimeout(12000);
            int code = http.GET();
            if (code == 200) { out = http.getString(); ok = true; }
            else Serial.printf("[net] GET %s -> %d\n", path, code);
            http.end();
        }
        delete tls;

    } else {
        WiFiClient plain;
        if (http.begin(plain, url)) {
            http.addHeader("x-device-token", cfgToken());
            http.setTimeout(12000);
            int code = http.GET();
            if (code == 200) { out = http.getString(); ok = true; }
            else Serial.printf("[net] GET %s -> %d\n", path, code);
            http.end();
        }
    }

    return ok;
}

static bool fetchStats() {
    String body;
    if (!httpGet("/api/stats", body)) { strcpy(status, "stats fetch failed"); return false; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[json] %s\n", err.c_str());
        strcpy(status, "bad json");
        return false;
    }

    JsonObject g = doc["panels"]["github"];
    if (g.isNull()) { strcpy(status, "no github panel"); return false; }

    const char *cal = g["cal"] | "";
    data.calLen = min((int)strlen(cal), (int)sizeof(data.cal) - 1);
    memcpy(data.cal, cal, data.calLen);
    data.cal[data.calLen] = 0;

    data.total = g["total"] | 0L;
    strlcpy(data.from, g["from"] | "", sizeof(data.from));
    strlcpy(data.to,   g["to"]   | "", sizeof(data.to));

    data.nSeries = 0;
    JsonArray labels = g["labels"], values = g["values"];
    for (size_t i = 0; i < labels.size() && data.nSeries < 6; i++) {
        strlcpy(data.labels[data.nSeries], labels[i] | "", 8);
        data.values[data.nSeries] = values[i] | 0L;
        data.nSeries++;
    }

    data.nStats = 0;
    for (JsonArray s : g["stats"].as<JsonArray>()) {
        if (data.nStats >= 6) break;
        Stat &st = data.stats[data.nStats];
        strlcpy(st.label, s[0] | "", sizeof(st.label));

        // value arrives as a number most of the time, but keep strings working
        if (s[1].is<const char *>()) strlcpy(st.value, s[1] | "", sizeof(st.value));
        else snprintf(st.value, sizeof(st.value), "%ld", (long)(s[1] | 0L));

        strlcpy(st.tone, s[2] | "neutral", sizeof(st.tone));
        data.nStats++;
    }

    data.nNotes = 0;
    for (JsonArray n : g["notes"].as<JsonArray>()) {
        if (data.nNotes >= 4) break;
        Note &note = data.notes[data.nNotes];
        strlcpy(note.icon, n[0] | "today", sizeof(note.icon));

        // fonts here are ascii only, so fold anything above 0x7e rather than
        // letting it draw as noise
        const char *src = n[1] | "";
        size_t o = 0;
        for (size_t i = 0; src[i] && o < sizeof(note.text) - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c >= 0x20 && c <= 0x7e) note.text[o++] = c;
            else if (c >= 0xC0) note.text[o++] = '-';
        }
        note.text[o] = 0;
        data.nNotes++;
    }

    // A rising total is itself a push signal, and unlike the pulse counter it
    // needs nothing stored anywhere. The counter gives ~10s latency but only
    // survives if the server has durable storage; this is slower but cannot be
    // lost, so the two together degrade instead of failing.
    static long prevTotal = -1;
    bool grew = (prevTotal >= 0 && data.total > prevTotal);
    prevTotal = data.total;

    // Everything the screens actually draw, and nothing else. The response also
    // carries a timestamp, which changes every time and would defeat the point.
    uint32_t h = fnv1a(data.cal);
    h = fnv1aNum(data.total, h);
    h = fnv1a(data.from, h);
    h = fnv1a(data.to, h);
    for (int i = 0; i < data.nSeries; i++) { h = fnv1a(data.labels[i], h); h = fnv1aNum(data.values[i], h); }
    for (int i = 0; i < data.nStats; i++)  { h = fnv1a(data.stats[i].label, h); h = fnv1a(data.stats[i].value, h); }
    for (int i = 0; i < data.nNotes; i++)  { h = fnv1a(data.notes[i].icon, h);  h = fnv1a(data.notes[i].text, h); }

    contentChanged = (h != renderedHash);
    renderedHash = h;

    data.valid = true;
    snprintf(status, sizeof(status), "ok  heap %ukB", ESP.getFreeHeap() / 1024);

    if (grew) {
        Serial.printf("[total] %ld contributions, up from before\n", data.total);
        celebrate("contribution total rose");
    }
    Serial.printf("[stats] %d days, total %ld, %d series, %d stats, heap %u%s\n",
                  data.calLen, data.total, data.nSeries, data.nStats, ESP.getFreeHeap(),
                  contentChanged ? ", changed" : ", no change");
    return true;
}

static void checkPulse() {
    String body;
    if (!httpGet("/api/pulse", body)) return;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return;

    long seq = doc["seq"] | 0L;
    if (seenSeq < 0) { seenSeq = seq; return; }   // first read just syncs

    // A counter that went backwards means the server restarted and lost its
    // place, not that a push was undone. Resync, otherwise the device sits there
    // waiting to be told about push number 12635 and ignores everything until
    // the new counter catches up, which is to say forever.
    if (seq < seenSeq) {
        Serial.printf("[pulse] counter reset %ld -> %ld, resyncing\n", seenSeq, seq);
        seenSeq = seq;
        return;
    }

    if (seq > seenSeq) {
        Serial.printf("[pulse] %ld -> %ld, push landed\n", seenSeq, seq);
        seenSeq = seq;
        celebrate("push counter moved");
        lastStats = 0;    // pull fresh numbers straight after a push
    }
}

/* ------------------------------------------------------------- render */

// No titles. On a screen this size the label eats room the data wants, and by
// the second day you know which screen you are on. Only surfaces a warning when
// the network has gone, since that is the one thing you cannot infer by looking.
static void header(const char *) {
    tft.fillRect(0, 0, W, 20, BG);
    if (WiFi.isConnected()) return;

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(tft.color565(0xd2, 0x99, 0x2e), BG);
    tft.drawString("no wifi", W - 8, 5, 2);
}

static uint32_t lastInteraction = 0;
static bool     dotsShown = false;

static void dots() {
    // hidden unless you have touched the thing recently, see DOTS_TIMEOUT_MS
    if (millis() - lastInteraction > DOTS_TIMEOUT_MS) { dotsShown = false; return; }

    int y = H - 12;
    int cx = W / 2 - (VIEW_COUNT * 12) / 2 + 6;
    for (int i = 0; i < VIEW_COUNT; i++) {
        uint16_t c = (i == view) ? FG : MUTED;
        tft.fillCircle(cx + i * 12, y, (i == view) ? 3 : 2, c);
    }
    dotsShown = true;
}

static void hideDots() {
    tft.fillRect(0, H - 20, W, 20, BG);
    dotsShown = false;
}

/* --------------------------------------------------------------- icons */

// The fonts here are ascii only, so an emoji would render as noise. These are
// drawn instead: small, flat, and legible at 14px.
static void drawIcon(const char *name, int cx, int cy) {
    const uint16_t ORANGE = tft.color565(0xff, 0x7b, 0x24);
    const uint16_t GOLD   = tft.color565(0xf1, 0xc4, 0x0f);
    const uint16_t AMBER  = tft.color565(0xd2, 0x99, 0x2e);

    if (!strcmp(name, "flame")) {
        tft.fillTriangle(cx, cy - 7, cx - 5, cy + 5, cx + 5, cy + 5, ORANGE);
        tft.fillTriangle(cx, cy - 1, cx - 3, cy + 5, cx + 3, cy + 5, GOLD);

    } else if (!strcmp(name, "up")) {
        tft.fillTriangle(cx, cy - 6, cx - 6, cy + 4, cx + 6, cy + 4, levelColor(4));

    } else if (!strcmp(name, "down")) {
        tft.fillTriangle(cx, cy + 6, cx - 6, cy - 4, cx + 6, cy - 4, AMBER);

    } else if (!strcmp(name, "star")) {
        // four-point spark. a real five-pointer at this size is mush.
        tft.fillTriangle(cx, cy - 7, cx - 3, cy, cx + 3, cy, GOLD);
        tft.fillTriangle(cx, cy + 7, cx - 3, cy, cx + 3, cy, GOLD);
        tft.fillTriangle(cx - 7, cy, cx, cy - 3, cx, cy + 3, GOLD);
        tft.fillTriangle(cx + 7, cy, cx, cy - 3, cx, cy + 3, GOLD);

    } else if (!strcmp(name, "trophy")) {
        tft.fillRect(cx - 5, cy - 6, 10, 7, GOLD);
        tft.drawFastHLine(cx - 7, cy - 5, 2, GOLD);
        tft.drawFastHLine(cx + 6, cy - 5, 2, GOLD);
        tft.fillRect(cx - 1, cy + 1, 3, 4, GOLD);
        tft.fillRect(cx - 4, cy + 5, 9, 2, GOLD);

    } else {
        // "today" - a calendar block
        tft.drawRect(cx - 6, cy - 6, 13, 13, FG);
        tft.fillRect(cx - 6, cy - 6, 13, 4, FG);
        tft.fillRect(cx - 3, cy + 1, 3, 3, levelColor(4));
    }
}

static void drawHeatmap() {
    tft.fillScreen(BG);
    header("contributions");

    const int cell = 4, gap = 1, step = cell + gap;
    const int cols = 53, rows = 7;
    const int gw = cols * step - gap;          // 264
    const int x0 = (W - gw) / 2;               // 28
    const int y0 = 50;

    for (int i = 0; i < data.calLen && i < cols * rows; i++) {
        int c = i / rows;
        int r = i % rows;
        int lvl = data.cal[i] - '0';
        if (lvl < 0 || lvl > 4) lvl = 0;
        tft.fillRect(x0 + c * step, y0 + r * step, cell, cell, levelColor(lvl));
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(FG, BG);
    char buf[40];
    snprintf(buf, sizeof(buf), "%ld contributions", data.total);
    tft.drawString(buf, x0, 20, 4);

    tft.setTextColor(MUTED, BG);
    snprintf(buf, sizeof(buf), "%s  to  %s", data.from, data.to);
    tft.drawString(buf, x0, y0 + rows * step + 8, 1);

    // notes list in the band under the grid, icon then text. left aligned as a
    // block so the icons line up in a column rather than drifting with the text.
    int ny = NOTES_Y;
    for (int i = 0; i < data.nNotes; i++) {
        drawIcon(data.notes[i].icon, NOTES_X + 8, ny);

        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(i == 0 ? FG : MUTED, BG);
        tft.drawString(data.notes[i].text, NOTES_X + 24, ny, 2);

        ny += NOTES_STEP;
    }
    tft.setTextDatum(TL_DATUM);

    // legend, right aligned as one measured block. hardcoding the offsets ran
    // "more" off the panel edge, so let the font decide the widths.
    const int lessW = tft.textWidth("less", 1);
    const int moreW = tft.textWidth("more", 1);
    const int swW = 5 * step - gap;
    const int lx = W - 8 - (lessW + 4 + swW + 4 + moreW);
    const int ly = y0 + rows * step + 6;

    tft.drawString("less", lx, ly, 1);
    for (int i = 0; i < 5; i++)
        tft.fillRect(lx + lessW + 4 + i * step, ly, cell, cell, levelColor(i));
    tft.drawString("more", lx + lessW + 4 + swW + 4, ly, 1);

    dots();
}

static void drawYears() {
    tft.fillScreen(BG);
    header("per year");

    if (data.nSeries == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(MUTED, BG);
        tft.drawString("no data", W / 2, H / 2, 2);
        dots();
        return;
    }

    long peak = 1;
    for (int i = 0; i < data.nSeries; i++) peak = max(peak, data.values[i]);

    const int base = H - 34;
    const int top = 52;
    const int span = base - top;
    const int slot = W / data.nSeries;
    const int bw = min(46, slot - 16);

    for (int i = 0; i < data.nSeries; i++) {
        int h = (int)((float)data.values[i] / peak * span);
        if (h < 2) h = 2;
        int cx = slot * i + slot / 2;

        tft.fillRect(cx - bw / 2, base - h, bw, h, levelColor(i % 2 ? 3 : 4));

        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(MUTED, BG);
        tft.drawString(data.labels[i], cx, base + 6, 2);

        char v[12];
        snprintf(v, sizeof(v), "%ld", data.values[i]);
        tft.setTextColor(FG, BG);
        // keep the count above the bar unless the bar is tall enough to sit inside
        tft.drawString(v, cx, (h > 26) ? (base - h + 6) : (base - h - 18), 2);
    }

    dots();
}

static void drawRepo() {
    tft.fillScreen(BG);
    header("repo");

    if (data.nStats == 0) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(MUTED, BG);
        tft.drawString("no stats", W / 2, H / 2, 2);
        dots();
        return;
    }

    int y = 40;
    for (int i = 0; i < data.nStats; i++) {
        Stat &s = data.stats[i];

        uint16_t c = FG;
        if (!strcmp(s.tone, "good")) c = levelColor(4);
        else if (!strcmp(s.tone, "warn")) c = tft.color565(0xd2, 0x99, 0x2e);
        else if (!strcmp(s.tone, "bad")) c = tft.color565(0xf8, 0x51, 0x49);

        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(MUTED, BG);
        tft.drawString(s.label, 16, y + 6, 2);

        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(c, BG);
        tft.drawString(s.value, W - 16, y, 4);

        y += 34;
        if (y > H - 40) break;
    }

    dots();
}

static void paintCounter(long value, uint16_t colour) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(colour, BG);
    tft.drawNumber(value, W / 2, 108, 8);
}

static void drawCounter(bool animate) {
    tft.fillScreen(BG);
    header("total contributions");

    long target = data.total;

    if (animate && shownTotal >= 0 && target != shownTotal) {
        // wind back a little so even a +1 gets a visible spin, then ease into
        // the real figure. an odometer that only ever moves one digit is dull.
        long diff = target - shownTotal;
        long span = max(10L, min(diff, 40L));
        long from = target - span;

        for (long i = 0; i <= span; i++) {
            paintCounter(from + i, levelColor(4));
            // ease out: quick at first, crawling as it lands
            int wait = 14 + (int)((float)i / span * (float)i / span * 90);
            delay(wait);
        }
        //settle
        paintCounter(target, FG);

    } else {
        paintCounter(target, FG);
    }

    shownTotal = target;

    char buf[48];
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(MUTED, BG);
    if (data.nStats && !strcmp(data.stats[0].label, "today"))
        snprintf(buf, sizeof(buf), "%s today", data.stats[0].value);
    else
        snprintf(buf, sizeof(buf), "%s to %s", data.from, data.to);
    tft.drawString(buf, W / 2, 176, 4);

    dots();
}

// What the hardware actually does. Worth a screen of its own because none of it
// is discoverable: there is no label on a bare board, and the two buttons on it
// are unmarked.
static void drawControls() {
    tft.fillScreen(BG);
    header("");

    // The one screen that gets a title, because it is the only one carrying
    // something you cannot work out by looking: where the settings actually are.
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(levelColor(4), BG);
    tft.drawString(WiFi.isConnected() ? WiFi.localIP().toString() : String("not connected"),
                   18, 4, 2);

    struct Row { const char *what; const char *does; };
    const Row rows[] = {
        { "tap right",   "next screen" },
        { "tap left",    "previous screen" },
        { "drag down",   "dim" },
        { "drag up",     "brighten" },
        { "BOOT 3s",     "forget wifi, redo setup" },
        { "RST",         "restart" },
    };

    int y = 30;
    for (const Row &r : rows) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(FG, BG);
        tft.drawString(r.what, 18, y, 2);

        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(MUTED, BG);
        tft.drawString(r.does, W - 18, y, 2);
        y += 26;
    }

    // the board has no power switch, and people look for one
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(MUTED, BG);
    tft.drawString("no power button. Unplug to switch off", 18, y + 6, 1);

    dots();
}

static void render() {
    switch (view) {
        case VIEW_HEATMAP: drawHeatmap(); break;
        case VIEW_COUNTER: drawCounter(true); break;
        case VIEW_YEARS:   drawYears();   break;
        case VIEW_CONTROLS: drawControls(); break;
        default:           drawRepo();    break;
    }
    dirty = false;
}

/* ------------------------------------------------------------- effects */

static const char *effectName(int e) {
    switch (e) {
        case FX_FLASH:    return "flash";
        case FX_CONFETTI: return "confetti";
        default:          return "ring";
    }
}

// full green wash. loudest of the three, impossible to miss from across a room.
static void fxFlash() {
    for (int i = 4; i >= 1; i--) {
        tft.fillScreen(levelColor(i));
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_BLACK, levelColor(i));
        tft.drawString("PUSHED", W / 2, H / 2, 6);
        delay(110);
    }
}

// particles fall over whatever is already on screen. each one erases its own
// previous position, so the view underneath survives well enough for the couple
// of seconds this runs before the full repaint.
static void fxConfetti() {
    const int N = 54;
    int px[N], py[N], vx[N], vy[N];
    uint16_t col[N];

    for (int i = 0; i < N; i++) {
        px[i] = random(W - 4);
        py[i] = random(-H, 0);
        vy[i] = random(4, 12);
        vx[i] = random(-2, 3);
        col[i] = levelColor(random(1, 5));
    }

    for (int f = 0; f < 78; f++) {
        for (int i = 0; i < N; i++) {
            tft.fillRect(px[i], py[i], 4, 4, BG);

            px[i] += vx[i];
            py[i] += vy[i];
            if (py[i] > H) { py[i] = random(-50, -4); px[i] = random(W - 4); }
            if (px[i] < 0) px[i] = W - 5;
            if (px[i] > W - 5) px[i] = 0;

            if (py[i] >= 0) tft.fillRect(px[i], py[i], 4, 4, col[i]);
        }
        delay(14);
    }

    for (int i = 0; i < N; i++) tft.fillRect(px[i], py[i], 4, 4, BG);
}

// border pulse. the quiet one: content stays readable the whole time, which is
// what you want if the display is somewhere you actually work.
static void fxRing() {
    for (int p = 0; p < 4; p++) {
        for (int t = 0; t < 7; t++)
            tft.drawRect(t, t, W - 2 * t, H - 2 * t, levelColor(4 - t / 2));
        delay(130);
        for (int t = 0; t < 7; t++)
            tft.drawRect(t, t, W - 2 * t, H - 2 * t, BG);
        delay(85);
    }
}

static void flashPush() {
    switch (effect) {
        case FX_FLASH:    fxFlash();    break;
        case FX_CONFETTI: fxConfetti(); break;
        default:          fxRing();     break;
    }
    dirty = true;
}

/* -------------------------------------------------------------- touch */

/* ---------------------------------------------------------- brightness */

static Preferences prefs;

static void applyBrightness(int v) {
    brightness = constrain(v, BRIGHTNESS_MIN, 255);
    ledcWrite(BL_CHANNEL, brightness);
}

static void saveBrightness() {
    prefs.begin("pulse", false);
    prefs.putUChar("bright", (uint8_t)brightness);
    prefs.end();
}

// drawn over whatever is on screen while a drag is in progress, then wiped
static void brightnessBar() {
    const int bw = 180, bh = 12, bx = (W - bw) / 2, by = H / 2 - 6;
    int pct = (brightness - BRIGHTNESS_MIN) * 100 / (255 - BRIGHTNESS_MIN);

    tft.fillRect(bx - 4, by - 22, bw + 8, bh + 30, BG);
    tft.drawRect(bx, by, bw, bh, MUTED);
    tft.fillRect(bx + 2, by + 2, (bw - 4) * pct / 100, bh - 4, FG);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(MUTED, BG);
    char b[16];
    snprintf(b, sizeof(b), "%d%%", pct);
    tft.drawString(b, W / 2, by - 12, 2);
}

/* --------------------------------------------------------------- touch */

static bool     down = false;
static int      lastX = 0;
static int      startY = 0, lastY = 0;
static int      dragStartBright = 255;
static bool     dimming = false;
static uint32_t lastTap = 0;

// Navigation is by tap, not drag. Dragging on a resistive panel means keeping
// steady pressure the whole way across, which is fussy enough that it reads as
// broken. Tap the right half to go forward, left half to go back.
//
// The XPT2046 reports contact breaking and remaking as a finger settles, so
// without the debounce one press walks several screens at once.
static void trackTouch() {
    if (ts.touched()) {
        TS_Point p = ts.getPoint();
        int sx = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, W);
        int sy = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, H);

        if (!down) {
            down = true;
            startY = sy;
            dragStartBright = brightness;
            dimming = false;
        }

        lastX = sx;
        lastY = sy;

        // A vertical drag means brightness. Committing to that only once it has
        // travelled far enough keeps an ordinary tap from nudging the backlight
        // every time a finger rolls slightly on the way off the glass.
        int dy = lastY - startY;
        if (!dimming && abs(dy) >= VDRAG_MIN_PX) dimming = true;

        if (dimming) {
            // down dims. full height of the panel covers the whole range.
            applyBrightness(dragStartBright - (dy * (255 - BRIGHTNESS_MIN) / H));
            brightnessBar();
        }
        return;
    }

    if (!down) return;
    down = false;

    if (dimming) {
        dimming = false;
        saveBrightness();
        Serial.printf("[bright] %d\n", brightness);
        lastInteraction = millis();
        dirty = true;          // wipe the bar, repaint the view underneath
        return;
    }

    uint32_t now = millis();
    if (now - lastTap < 350) return;
    lastTap = now;

    bool fwd = lastX >= W / 2;
    view = fwd ? (view + 1) % VIEW_COUNT : (view + VIEW_COUNT - 1) % VIEW_COUNT;

    lastInteraction = now;
    Serial.printf("[touch] tap %s (x=%d) -> view %d\n", fwd ? "right" : "left", lastX, view);
    dirty = true;
}

/* -------------------------------------------------------- layout check */

static int fails = 0;

struct Box { int x, y, w, h; char name[28]; };
static Box boxes[24];
static int nBoxes = 0;

static void chkReset() { nBoxes = 0; fails = 0; }

static void chk(const char *what, int x, int y, int w, int h) {
    bool bad = (x < 0) || (y < 0) || (x + w > W) || (y + h > H);

    // Fitting the panel is not the same as not sitting on top of something
    // else. Checking only bounds is how a QR ended up drawn through the
    // wordmark while the check reported everything fine.
    const char *hit = nullptr;
    for (int i = 0; i < nBoxes && !hit; i++) {
        Box &b = boxes[i];
        if (x < b.x + b.w && b.x < x + w && y < b.y + b.h && b.y < y + h) hit = b.name;
    }

    if (bad || hit) fails++;
    if (nBoxes < 24) {
        Box &b = boxes[nBoxes];
        b.x = x; b.y = y; b.w = w; b.h = h;
        strlcpy(b.name, what, sizeof(b.name));
        nBoxes++;
    }

    Serial.printf("  %-26s x%3d y%3d w%3d h%3d  -> %d,%d  %s%s%s\n",
                  what, x, y, w, h, x + w, y + h,
                  bad ? "OVERFLOW" : (hit ? "OVERLAPS " : "ok"),
                  hit ? hit : "", (bad && hit) ? " +OVERLAP" : "");
}

// measure everything we draw against the 320x240 panel. cheaper than squinting.
static void layoutCheck() {
    char buf[48];
    Serial.printf("[layout] panel %dx%d rotation %d\n", W, H, TFT_ROTATION);

    chkReset();

    Serial.println(" view 0 heatmap:");
    const int cell = 4, gap = 1, step = cell + gap, cols = 53, rows = 7;
    const int gw = cols * step - gap, x0 = (W - gw) / 2, y0 = 50;
    chk("grid", x0, y0, gw, rows * step - gap);

    snprintf(buf, sizeof(buf), "%ld contributions", data.total);
    chk("title(f4)", x0, 20, tft.textWidth(buf, 4), tft.fontHeight(4));

    snprintf(buf, sizeof(buf), "%s  to  %s", data.from, data.to);
    chk("daterange(f1)", x0, y0 + rows * step + 8, tft.textWidth(buf, 1), tft.fontHeight(1));

    const int lessW = tft.textWidth("less", 1), moreW = tft.textWidth("more", 1);
    const int swW = 5 * step - gap;
    const int lx = W - 8 - (lessW + 4 + swW + 4 + moreW);
    const int ly = y0 + rows * step + 6;
    chk("legend 'less'", lx, ly, lessW, tft.fontHeight(1));
    chk("legend swatches", lx + lessW + 4, ly, swW, cell);
    chk("legend 'more'", lx + lessW + 4 + swW + 4, ly, moreW, tft.fontHeight(1));

    int ny = NOTES_Y;
    for (int i = 0; i < data.nNotes; i++) {
        snprintf(buf, sizeof(buf), "note[%d] %s", i, data.notes[i].icon);
        int tw = tft.textWidth(data.notes[i].text, 2);
        chk(buf, NOTES_X - 8, ny - 8, 24 + tw, 16);
        ny += NOTES_STEP;
    }

    chkReset();
    Serial.println(" view 1 counter:");
    {
        int nw = tft.textWidth(String(data.total).c_str(), 8);
        chk("odometer(f8)", W / 2 - nw / 2, 108 - tft.fontHeight(8) / 2, nw, tft.fontHeight(8));
        const char *sub = data.nStats ? data.stats[0].value : data.to;
        snprintf(buf, sizeof(buf), "%s today", sub);
        int sw = tft.textWidth(buf, 4);
        chk("counter sub(f4)", W / 2 - sw / 2, 176 - tft.fontHeight(4) / 2, sw, tft.fontHeight(4));
    }

    chkReset();
    Serial.println(" view 2 years:");
    if (data.nSeries) {
        const int base = H - 34, top = 52, slot = W / data.nSeries;
        const int bw = min(46, slot - 16);
        for (int i = 0; i < data.nSeries; i++) {
            int cx = slot * i + slot / 2;
            snprintf(buf, sizeof(buf), "bar[%s]", data.labels[i]);
            chk(buf, cx - bw / 2, top, bw, base - top);
            snprintf(buf, sizeof(buf), "%ld", data.values[i]);
            int vw = tft.textWidth(buf, 2);
            snprintf(buf, sizeof(buf), "value[%d]", i);
            chk(buf, cx - vw / 2, base - (base - top), vw, tft.fontHeight(2));
            int lw = tft.textWidth(data.labels[i], 2);
            snprintf(buf, sizeof(buf), "label[%d]", i);
            chk(buf, cx - lw / 2, base + 6, lw, tft.fontHeight(2));
        }
    }

    chkReset();
    Serial.println(" view 4 controls:");
    {
        String ipnow = WiFi.isConnected() ? WiFi.localIP().toString() : String("not connected");
        chk("ip title(f2)", 18, 4, tft.textWidth(ipnow.c_str(), 2), tft.fontHeight(2));
        int cy = 30;
        const char *rows[] = { "tap right", "tap left", "drag down", "drag up", "BOOT 3s", "RST" };
        for (int i = 0; i < 6; i++) {
            chk(rows[i], 18, cy, tft.textWidth(rows[i], 2), tft.fontHeight(2));
            cy += 26;
        }
        chk("settings hint", 18, cy + 4, tft.textWidth("settings in a browser, or esp-git.local", 1), tft.fontHeight(1));
        chk("power note", 18, cy + 17, tft.textWidth("no power button, unplug to switch off", 1), tft.fontHeight(1));
    }
    chkReset();

    Serial.println(" view 2 repo:");
    int y = 40;
    for (int i = 0; i < data.nStats; i++) {
        chk(data.stats[i].label, 16, y + 6, tft.textWidth(data.stats[i].label, 2), tft.fontHeight(2));
        int vw = tft.textWidth(data.stats[i].value, 4);
        chk("  value", W - 16 - vw, y, vw, tft.fontHeight(4));
        y += 34;
        if (y > H - 40) { Serial.printf("  (clipped after %d rows)\n", i + 1); break; }
    }

    chk("page dots", W / 2 - (VIEW_COUNT * 12) / 2, H - 15, VIEW_COUNT * 12, 6);

    Serial.printf("[layout] %s (%d overflow)\n", fails ? "FAIL" : "PASS", fails);
}

/* ------------------------------------------------------ remote control */

static bool httpPost(const char *path, const String &json) {
    String url = String(cfgHost()) + path;
    WiFiClientSecure tls;
    WiFiClient plain;
    WiFiClient *client;

    if (url.startsWith("https")) { tls.setInsecure(); client = &tls; }
    else                         { client = &plain; }

    HTTPClient http;
    if (!http.begin(*client, url)) return false;

    http.addHeader("x-device-token", cfgToken());
    http.addHeader("content-type", "application/json");
    http.setTimeout(10000);

    int code = http.POST(json);
    http.end();

    return code == 200;
}

static void report(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    JsonDocument d;
    d["report"] = buf;
    String out;
    serializeJson(d, out);
    httpPost("/api/command", out);
}

static void otaCheck(bool verbose);

// Same trick as the pulse: the device asks for work rather than being reachable.
// Anything that can queue a command can drive the screen from anywhere, without
// the device ever accepting an inbound connection.
static void checkCommand() {
    String body;
    if (!httpGet("/api/command", body)) return;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return;

    const char *cmd = doc["cmd"] | "";
    if (!cmd[0]) return;

    Serial.printf("[cmd] remote: %s\n", cmd);
    lastInteraction = millis();

    if (!strncmp(cmd, "view:", 5)) {
        int v = atoi(cmd + 5);
        if (v >= 0 && v < VIEW_COUNT) { view = v; dirty = true; report("view %d", v); }
        else report("bad view %d", v);

    } else if (!strncmp(cmd, "effect:", 7)) {
        int e = atoi(cmd + 7);
        if (e >= 0 && e < FX_COUNT) { effect = e; report("effect %s", effectName(e)); }

    } else if (!strncmp(cmd, "brightness:", 11)) {
        applyBrightness(atoi(cmd + 11));
        saveBrightness();
        report("brightness %d", brightness);

    } else if (!strcmp(cmd, "flash")) {
        flashPush();
        report("played %s", effectName(effect));

    } else if (!strcmp(cmd, "refetch")) {
        lastStats = 0;
        report("refetch queued");

    } else if (!strcmp(cmd, "status")) {
        report("v%s ip %s rssi %d view %d seq %ld heap %u total %ld bright %d",
               FW_VERSION, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
               view, seenSeq, ESP.getFreeHeap(), data.total, brightness);

    } else if (!strcmp(cmd, "update")) {
        report("checking for update, running %s", FW_VERSION);
        otaCheck(true);

    } else if (!strcmp(cmd, "reboot")) {
        report("rebooting");
        delay(300);
        ESP.restart();

    } else {
        report("unknown command: %s", cmd);
    }
}

/* ----------------------------------------------------------------- ota */

static void otaProgress(int done, int total) {
    static int lastPct = -1;
    int pct = total ? (done * 100 / total) : 0;
    if (pct == lastPct) return;
    lastPct = pct;

    const int bw = 240, bh = 16, bx = (W - bw) / 2, by = 140;
    tft.drawRect(bx - 2, by - 2, bw + 4, bh + 4, MUTED);
    tft.fillRect(bx, by, bw * pct / 100, bh, levelColor(4));

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(FG, BG);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", pct);
    tft.drawString(b, W / 2, by + 40, 4);

    if (pct % 10 == 0) Serial.printf("[ota] %d%%\n", pct);
}

// Pull model: the device asks whether there is something newer and fetches it
// itself. Nothing has to reach in from outside, so this works from any network
// without port forwarding or a static address.
static char availableVersion[16] = "";
static bool otaForced = false;

// Only ever move forward. Asking whether the offered version *differs* is not
// the same as asking whether it is *newer*, and the difference is a board that
// quietly reinstalls an older build over whatever you just flashed, every time
// it checks. Anything unparseable counts as not newer, because refusing to
// update is recoverable over usb and downgrading in a loop is not.
static bool isNewer(const char *offered, const char *running) {
    int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    sscanf(offered, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(running, "%d.%d.%d", &b[0], &b[1], &b[2]);

    for (int i = 0; i < 3; i++) {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return false;   // equal, or a suffix we cannot compare
}

static void otaCheck(bool verbose) {
    String body;
    if (!httpGet("/api/firmware?meta=1", body)) {
        if (verbose) Serial.println("[ota] no manifest");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) return;

    // Refuse anything not built for this board. These images assume a plain
    // ESP32 with a 4MB layout and a specific pin map; the same firmware built
    // for another profile will flash perfectly and then not come back.
    const char *board = doc["board"] | "";
    if (!board[0]) {
        Serial.println("[ota] manifest does not say which board it is for, refusing");
        availableVersion[0] = 0;
        return;
    }
    if (strcmp(board, BOARD_ID)) {
        Serial.printf("[ota] image is for %s, this is %s. refusing.\n", board, BOARD_ID);
        availableVersion[0] = 0;
        return;
    }

    const char *ver = doc["version"] | "";
    if (!ver[0] || !isNewer(ver, FW_VERSION)) {
        availableVersion[0] = 0;
        if (verbose) {
            if (ver[0] && strcmp(ver, FW_VERSION))
                Serial.printf("[ota] server has %s, running %s. not newer, staying put.\n",
                              ver, FW_VERSION);
            else
                Serial.printf("[ota] running %s, nothing newer\n", FW_VERSION);
        }
        return;
    }

    strlcpy(availableVersion, ver, sizeof(availableVersion));

    if (!cfgAutoUpdate() && !otaForced) {
        Serial.printf("[ota] %s available, not installing. auto update is off.\n", ver);
        return;
    }
    otaForced = false;

    // Belt and braces. The name could be reused carelessly; the silicon cannot
    // lie about what it is.
    if (ESP.getFlashChipSize() < 4 * 1024 * 1024) {
        Serial.printf("[ota] only %u bytes of flash, refusing\n", ESP.getFlashChipSize());
        return;
    }

    Serial.printf("[ota] %s -> %s, downloading %ld bytes\n",
                  FW_VERSION, ver, (long)(doc["size"] | 0L));

    tft.fillScreen(BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(FG, BG);
    tft.drawString("updating", W / 2, 96, 4);
    tft.setTextColor(MUTED, BG);
    tft.drawString(ver, W / 2, 196, 2);

    // Driving Update directly rather than going through HTTPUpdate, because that
    // class gives no way to set a request header on this core version, and the
    // alternative was putting the token in the query string where it would end
    // up in every access log along the way.
    String url = String(cfgHost()) + "/api/firmware";
    WiFiClientSecure tls;
    WiFiClient plain;
    WiFiClient *client;   // WiFiClientSecure derives from this, so one pointer covers both

    if (url.startsWith("https")) { tls.setInsecure(); client = &tls; }
    else                          { client = &plain; }

    HTTPClient http;
    if (!http.begin(*client, url)) { Serial.println("[ota] begin failed"); return; }

    http.addHeader("x-device-token", cfgToken());
    http.setTimeout(20000);

    int code = http.GET();
    int len = http.getSize();

    if (code != 200 || len <= 0) {
        Serial.printf("[ota] download failed: http %d len %d\n", code, len);
        http.end();
        dirty = true;
        return;
    }

    Update.onProgress(otaProgress);

    if (!Update.begin(len)) {
        Serial.printf("[ota] not enough room: %s\n", Update.errorString());
        http.end();
        dirty = true;
        return;
    }

    size_t written = Update.writeStream(http.getStream());
    http.end();

    if (written == (size_t)len && Update.end(true)) {
        Serial.println("[ota] written, rebooting into the new image");
        delay(400);
        ESP.restart();
    }

    Serial.printf("[ota] failed after %u/%d bytes: %s\n",
                  (unsigned)written, len, Update.errorString());
    tft.fillScreen(BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tft.color565(0xf8, 0x51, 0x49), BG);
    tft.drawString("update failed", W / 2, H / 2, 4);
    delay(2500);
    dirty = true;
}

/* -------------------------------------------------------- touch probe */

// raw 12-bit read straight off the controller. lets us tell "nobody is touching
// it" apart from "the chip isn't on the bus", which ts.touched() alone cannot.
static uint16_t xptRead(uint8_t cmd) {
    touchBus.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(TOUCH_CS, LOW);
    touchBus.transfer(cmd);
    uint8_t hi = touchBus.transfer(0x00);
    uint8_t lo = touchBus.transfer(0x00);
    digitalWrite(TOUCH_CS, HIGH);
    touchBus.endTransaction();
    return (((uint16_t)hi << 8) | lo) >> 3;
}

static void xptPresence() {
    // TEMP0 and VBAT are internal to the chip, so they answer whether or not
    // anything is pressing on the panel.
    uint16_t temp = xptRead(0x87);
    uint16_t vbat = xptRead(0xA7);
    uint16_t z1   = xptRead(0xB7);
    uint16_t z2   = xptRead(0xC7);

    Serial.printf("[xpt] TEMP0=%4u VBAT=%4u Z1=%4u Z2=%4u\n", temp, vbat, z1, z2);

    bool stuckLow  = (temp == 0 && vbat == 0 && z1 == 0 && z2 == 0);
    bool stuckHigh = (temp >= 4090 && vbat >= 4090 && z1 >= 4090 && z2 >= 4090);

    if (stuckLow)       Serial.println("[xpt] all zero - MISO not reaching the chip, check pin 39 / CS 33");
    else if (stuckHigh) Serial.println("[xpt] all rails high - MISO floating, chip probably not selected");
    else                Serial.println("[xpt] controller is responding on the bus");
}

// 12 seconds of raw samples. a panel that is wired correctly but untouched sits
// near z=0; readings pinned at 0 or 4095 for the whole window mean the SPI pins
// are wrong rather than the panel being idle.
static void touchProbe() {
    xptPresence();

    Serial.println("[touch] probing 12s - press and drag the screen now");

    uint32_t end = millis() + 12000;
    int hits = 0, zmax = 0;
    int xmin = 9999, xmax = -1, ymin = 9999, ymax = -1;
    uint32_t lastPrint = 0;

    while (millis() < end) {
        if (ts.touched()) {
            TS_Point p = ts.getPoint();
            hits++;
            zmax = max(zmax, (int)p.z);
            xmin = min(xmin, (int)p.x); xmax = max(xmax, (int)p.x);
            ymin = min(ymin, (int)p.y); ymax = max(ymax, (int)p.y);

            if (millis() - lastPrint > 250) {
                lastPrint = millis();
                Serial.printf("  raw x=%4d y=%4d z=%4d   -> screen x=%3d\n",
                              p.x, p.y, p.z, map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, W));
            }
        }
        delay(10);
    }

    Serial.printf("[touch] samples=%d zmax=%d\n", hits, zmax);
    if (!hits) {
        Serial.println("[touch] nothing detected. either untouched, or CS/IRQ pins are wrong.");
    } else {
        Serial.printf("[touch] raw x %d..%d   y %d..%d\n", xmin, xmax, ymin, ymax);
        Serial.printf("[touch] configured x range %d..%d %s\n", TOUCH_MIN_X, TOUCH_MAX_X,
                      (xmin < TOUCH_MIN_X || xmax > TOUCH_MAX_X)
                          ? "<-- observed values fall outside, recalibrate" : "(fits)");
        int travel = abs(map(xmax, TOUCH_MIN_X, TOUCH_MAX_X, 0, W) -
                         map(xmin, TOUCH_MIN_X, TOUCH_MAX_X, 0, W));
        Serial.printf("[touch] max horizontal travel %dpx, swipe needs %d\n", travel, SWIPE_MIN_PX);
    }
}

/* --------------------------------------------------------- serial cmds */

// lets the screens be exercised without a finger on the glass, which is the only
// way to soak-test view cycling over a long run.
static void serialCmds() {
    if (!Serial.available()) return;
    char c = Serial.read();
    lastInteraction = millis();

    switch (c) {
        case 'n': view = (view + 1) % VIEW_COUNT;              dirty = true; break;
        case 'p': view = (view + VIEW_COUNT - 1) % VIEW_COUNT; dirty = true; break;
        case '0': view = 0; dirty = true; break;
        case '1': view = 1; dirty = true; break;
        case '2': view = 2; dirty = true; break;
        case '3': view = 3; dirty = true; break;
        case '4': view = 4; dirty = true; break;
        case 'f': flashPush(); break;
        case 'e': effect = (effect + 1) % FX_COUNT;
                  Serial.printf("[fx] %s\n", effectName(effect)); flashPush(); break;
        case 'r': lastStats = 0; break;
        case 'h': Serial.printf("[heap] free %u  min %u  largest %u\n",
                                ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                                ESP.getMaxAllocHeap()); break;
        case 'm': layoutCheck(); break;
        case '+': applyBrightness(brightness + 25); saveBrightness();
                  Serial.printf("[bright] %d\n", brightness); break;
        case '-': applyBrightness(brightness - 25); saveBrightness();
                  Serial.printf("[bright] %d\n", brightness); break;
        case 'u': otaCheck(true); break;
        case 't': touchProbe(); break;
        case 'w': Serial.printf("[wifi] %s ip %s rssi %d  view %d  seq %ld  %s\n",
                                WiFi.isConnected() ? "up" : "down",
                                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                                view, seenSeq, status); break;
        case '?': Serial.println("n/p next,prev  0-2 view  f flash  r refetch  h heap  m layout  w status  e effect  u update  +/- brightness"); break;
        default: return;
    }

    if (c >= '0' && c <= '4') Serial.printf("[cmd] view %d\n", view);
}

/* ---------------------------------------------------------- boot screen */

// Boot is the one time the device has nothing useful to show, so it shows what
// it is doing instead. Each stage lands as its own line, which also makes a
// failure obvious: whatever the last line says is what it got stuck on.
static const char *BOOT_STEPS[] = { "display", "wifi", "server", "data" };
static const int   BOOT_COUNT   = 4;

// Filler for the seconds spent waiting on wifi. Half of these describe nothing
// the device is actually doing, which is the point. Keep them ascii and short
// enough to fit at font 2, and keep the honest detail line underneath them.
static const char *PHRASES[] = {
    "combobulating",
    "analyzing",
    "inferring",
    "reticulating splines",
    "counting squares",
    "tallying commits",
    "unfolding the year",
    "consulting the graph",
    "warming the cache",
    "aligning pixels",
    "negotiating",
    "herding packets",
};
static const int PHRASE_COUNT = sizeof(PHRASES) / sizeof(PHRASES[0]);

static int phraseIdx = 0;
static int phraseDots = 0;
static const int PHRASE_Y = 168;

// Redraws only its own strip. Calling the full boot paint on a timer would make
// the whole screen flicker several times a second.
static void bootTick() {
    char buf[40];
    const char *p = PHRASES[phraseIdx % PHRASE_COUNT];
    snprintf(buf, sizeof(buf), "%s%.*s", p, phraseDots, "...");

    tft.fillRect(0, PHRASE_Y - 12, W, 24, BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(FG, BG);
    tft.drawString(buf, W / 2, PHRASE_Y, 2);

    if (++phraseDots > 3) {
        phraseDots = 0;
        phraseIdx++;          // new phrase once the dots have run their cycle
    }
}

// A phone camera pointed at this joins the setup network directly, which skips
// the part where someone has to find an unfamiliar SSID in their wifi list. The
// captive portal then opens on its own once they are on it.
// remembered so the portal screen can be put back after an effect paints over it
static char portalAp[40] = "";
static char portalIp[20] = "";

static void drawJoinQR(const char *ap, const char *ip) {
    strlcpy(portalAp, ap, sizeof(portalAp));
    strlcpy(portalIp, ip, sizeof(portalIp));

    char payload[80];
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ap, setupPassword());

    QRCode qr;
    uint8_t buf[qrcode_getBufferSize(3)];
    qrcode_initText(&qr, buf, 3, ECC_LOW, payload);

    tft.fillScreen(BG);

    // Wordmark first, and everything else is placed under it rather than
    // centred in what is left. Centring is what put the QR through the
    // lettering: both fitted the screen, they just did not fit each other.
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(FG, BG);
    tft.setTextSize(2);
    tft.drawString(BOOT_NAME, 18, 20, 4);
    tft.setTextSize(1);
    const int nameBottom = 20 + tft.fontHeight(4) * 2;

    const int scale = 4;
    const int dim = qr.size * scale;
    const int qx = W - dim - 16;
    const int qy = nameBottom + 14;

    tft.fillRect(qx - 6, qy - 6, dim + 12, dim + 12, TFT_WHITE);
    for (uint8_t y = 0; y < qr.size; y++)
        for (uint8_t x = 0; x < qr.size; x++)
            if (qrcode_getModule(&qr, x, y))
                tft.fillRect(qx + x * scale, qy + y * scale, scale, scale, TFT_BLACK);

    // left column, kept clear of the QR block
    int ty = qy;
    tft.setTextColor(MUTED, BG);
    tft.drawString("scan to set up", 18, ty, 2);
    ty += 22;
    tft.drawString("or join", 18, ty, 1);
    ty += 13;
    tft.setTextColor(FG, BG);
    tft.drawString(ap, 18, ty, 2);
    ty += 26;
    tft.setTextColor(MUTED, BG);
    tft.drawString("password", 18, ty, 1);
    ty += 13;
    tft.setTextColor(levelColor(4), BG);
    tft.drawString(setupPassword(), 18, ty, 4);

    tft.setTextColor(MUTED, BG);
    tft.drawString(ip, 18, H - 16, 1);
}

// provision.cpp calls this with whatever the portal is doing
static void bootPortalLine(const char *a, const char *b) {
    tft.fillRect(0, PHRASE_Y - 14, W, 60, BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(FG, BG);
    tft.drawString(a, W / 2, PHRASE_Y, 2);
    if (b && b[0]) {
        tft.setTextColor(MUTED, BG);
        tft.drawString(b, W / 2, PHRASE_Y + 24, 2);
    }
}

static void bootScreen(int stage, const char *detail, bool failed) {
    tft.fillScreen(BG);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(FG, BG);
    tft.setTextSize(2);
    tft.drawString(BOOT_NAME, W / 2, 54, 4);
    tft.setTextSize(1);

    tft.setTextColor(MUTED, BG);
    tft.drawString(FW_VERSION, W / 2, 88, 2);

    const int bw = 220, bh = 8, bx = (W - bw) / 2, by = 122;
    tft.drawRect(bx, by, bw, bh, MUTED);

    int done = failed ? stage : stage + 1;
    if (done > BOOT_COUNT) done = BOOT_COUNT;
    tft.fillRect(bx + 2, by + 2, (bw - 4) * done / BOOT_COUNT, bh - 4,
                 failed ? tft.color565(0xf8, 0x51, 0x49) : levelColor(4));

    // step ticks
    int sx = bx;
    int step = bw / BOOT_COUNT;
    for (int i = 0; i < BOOT_COUNT; i++) {
        uint16_t c = (i < stage) ? levelColor(4)
                   : (i == stage) ? (failed ? tft.color565(0xf8, 0x51, 0x49) : FG)
                   : MUTED;
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(c, BG);
        tft.drawString(BOOT_STEPS[i], sx + step / 2, by + 18, 1);
        if (i < stage) tft.fillCircle(sx + step / 2, by + 36, 3, levelColor(4));
    }

    if (detail && detail[0]) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(failed ? tft.color565(0xf8, 0x51, 0x49) : MUTED, BG);
        tft.drawString(detail, W / 2, 196, 2);
    }
}

/* ------------------------------------------------------------ settings */

// A second, much smaller web server that runs while the board is doing its job.
// The setup portal is for first run and clearing wifi to reach it just to change
// a colour is a silly thing to ask of anyone.
static WebServer ui(80);
static bool uiUp = false;


static void uiState() {
    JsonDocument d;
    d["version"]    = FW_VERSION;
    d["ip"]         = WiFi.localIP().toString();
    d["rssi"]       = WiFi.RSSI();
    d["total"]      = data.total;
    d["uptime"]     = millis();
    d["effect"]     = effect;
    d["brightness"] = brightness;
    d["view"]       = view;
    d["ssid"]       = WiFi.SSID();
    d["autoUpdate"] = cfgAutoUpdate();
    d["available"]  = availableVersion;
    d["board"]      = BOARD_ID;
    d["rotation"]   = cfgRotation();
    String out; serializeJson(d, out);
    ui.send(200, "application/json", out);
}

static void uiSettings() {
    JsonDocument d;
    deserializeJson(d, ui.arg("plain"));

    if (d["effect"].is<int>()) {
        int e = d["effect"];
        if (e >= 0 && e < FX_COUNT) { effect = e; saveEffect(e); }
    }
    if (d["brightness"].is<int>()) {
        applyBrightness(d["brightness"]);
        saveBrightness();
    }
    if (d["rotation"].is<int>()) {
        int r = d["rotation"];
        if (r == 1 || r == 3) {
            saveRotation(r);
            tft.setRotation(r);
            ts.setRotation(r);       // the panel and the glass have to agree
            tft.fillScreen(BG);
            dirty = true;
        }
    }
    if (d["view"].is<int>()) {
        int v = d["view"];
        if (v >= 0 && v < VIEW_COUNT) { view = v; dirty = true; lastInteraction = millis(); }
    }

    if (d["autoUpdate"].is<bool>()) saveAutoUpdate(d["autoUpdate"]);

    const char *ssid = d["ssid"] | "";
    if (ssid[0]) saveWifi(ssid, d["pass"] | "");

    const char *host  = d["host"]  | "";
    const char *token = d["token"] | "";
    if (host[0] || token[0]) saveServerConfig(host, token);

    ui.send(200, "application/json", "{\"ok\":true}");

    if (d["restart"] | false) { delay(300); ESP.restart(); }
}

static void uiPreview() {
    JsonDocument d;
    deserializeJson(d, ui.arg("plain"));
    int e = d["effect"] | 0;
    ui.send(200, "application/json", "{\"ok\":true}");

    if (e >= 0 && e < FX_COUNT) {
        int was = effect;
        effect = e;
        flashPush();
        effect = was;
    }
}

static void startUI() {
    if (uiUp || !WiFi.isConnected()) return;

    // so it can be reached by name as well as by address, which matters because
    // dhcp will hand it a different one eventually
    if (MDNS.begin("esp-git")) MDNS.addService("http", "tcp", 80);

    ui.on("/", []() { ui.send_P(200, "text/html", SETTINGS_PAGE); });
    ui.on("/api/state", uiState);
    ui.on("/api/settings", HTTP_POST, uiSettings);
    ui.on("/api/preview", HTTP_POST, uiPreview);
    ui.on("/api/scan", []() { String j; scanNetworks(j); ui.send(200, "application/json", j); });
    ui.on("/api/update", HTTP_POST, []() {
        ui.send(200, "application/json", "{\"ok\":true}");
        otaForced = true;
        otaCheck(true);
    });
    ui.begin();
    uiUp = true;

    Serial.printf("[ui] settings at http://%s  or  http://esp-git.local\n",
                  WiFi.localIP().toString().c_str());
}

/* --------------------------------------------------------------- main */

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[boot] esp-git");
    randomSeed(esp_random());

    tft.init();
    tft.setRotation(cfgRotation());
    tft.fillScreen(BG);

    // Has to come after tft.init(). TFT_eSPI drives TFT_BL as a plain output
    // during init, which detaches the pin from the LEDC peripheral, so setting
    // PWM up first silently does nothing.
    pinMode(0, INPUT_PULLUP);          // BOOT button
    ledcSetup(BL_CHANNEL, 5000, 8);
    ledcAttachPin(TFT_BL, BL_CHANNEL);

    prefs.begin("pulse", true);
    applyBrightness(prefs.getUChar("bright", 255));
    prefs.end();

    effect = cfgEffect();

    touchBus.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchBus);
    ts.setRotation(cfgRotation());   // or taps land mirrored when flipped

    bootScreen(0, "panel ready", false);

    // 1: wifi. credentials come from NVS, or the portal if there are none.
    phraseIdx = random(PHRASE_COUNT);
    bootScreen(1, "", false);
    provisionUI(bootPortalLine);
    provisionTick(bootTick);
    provisionPortalUI(drawJoinQR);
    provisionPreviewUI([](int e) {
        int was = effect;
        effect = e;
        flashPush();
        effect = was;
        // the effect painted over the setup screen, so put it back rather than
        // letting render() draw a view the portal is not finished with
        dirty = false;
        if (portalAp[0]) drawJoinQR(portalAp, portalIp);
    });

    if (!setupWifi()) {
        Serial.println("[wifi] setup never completed");
        bootScreen(1, "wifi setup incomplete", true);
        delay(2500);
        return;
    }

    Serial.printf("[wifi] %s  rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    bootScreen(2, WiFi.localIP().toString().c_str(), false);

    if (!strlen(cfgToken()) || !strlen(cfgHost())) {
        Serial.println("[cfg] no server configured, back to setup");
        tft.fillScreen(BG);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(FG, BG);
        tft.drawString("no server set", W / 2, 100, 4);
        tft.setTextColor(MUTED, BG);
        tft.drawString("hold BOOT 3s to redo setup", W / 2, 140, 2);
        delay(6000);
        forgetWifi();
    }

    // 2: can we actually reach the endpoint
    {
        String probe;
        bootTick();
        if (httpGet("/api/pulse", probe)) {
            bootScreen(3, "fetching", false);
            bootTick();
        } else {
            bootScreen(2, "server unreachable", true);
            delay(2500);
        }
    }

    // 3: first payload. lastStats stays 0 so loop() pulls it immediately.
    if (fetchStats()) {
        bootScreen(4, "ready", false);
        lastStats = millis() ? millis() : 1;
        dirty = true;
        delay(600);
    }

    Serial.printf("[fw] version %s\n", FW_VERSION);
    Serial.println("[cmd] ? for commands");

    if (WiFi.isConnected()) otaCheck(true);
}

// Hold to start over. Configuration lives on the settings page at the board's
// own address, so the button only has to cover the case where the board cannot
// reach the network at all and that page is therefore out of reach too.
static void checkResetButton() {
    static uint32_t heldSince = 0;

    if (digitalRead(0) == LOW) {                 // BOOT is active low
        if (!heldSince) heldSince = millis();
        uint32_t held = millis() - heldSince;

        if (held > 3000) {
            Serial.println("[btn] held, clearing wifi and returning to setup");
            tft.fillScreen(BG);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(FG, BG);
            tft.drawString("starting over", W / 2, H / 2, 4);
            delay(700);
            forgetWifi();                        // reboots into first run setup

        } else if (held > 600) {
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(MUTED, BG);
            char b[28];
            snprintf(b, sizeof(b), "hold %ds to start over ", (int)((3000 - held) / 1000) + 1);
            tft.drawString(b, W / 2, H - 34, 2);
        }

    } else if (heldSince) {
        heldSince = 0;
        dirty = true;                            // wipe the countdown
    }
}

void loop() {
    serialCmds();
    checkResetButton();
    if (uiUp) ui.handleClient(); else startUI();
    trackTouch();

    uint32_t now = millis();

    if (WiFi.isConnected()) {
        if (lastStats == 0 || now - lastStats >= STATS_INTERVAL_MS) {
            lastStats = now ? now : 1;
            // fetching is not a reason to repaint. changing is.
            if (fetchStats() && contentChanged) dirty = true;
        }

        if (now - lastOta >= OTA_INTERVAL_MS) {
            lastOta = now;
            otaCheck(false);
        }

        if (now - lastPulse >= PULSE_INTERVAL_MS) {
            lastPulse = now;
            checkPulse();
            checkCommand();
        }

    } else if (now - lastPulse >= 5000) {
        lastPulse = now;
        WiFi.reconnect();
    }

    if (dirty) render();
    else if (dotsShown && millis() - lastInteraction > DOTS_TIMEOUT_MS) hideDots();

    delay(20);
}
