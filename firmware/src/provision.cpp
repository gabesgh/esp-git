#include "provision.h"
#ifdef CONFIG_DIST
#include "config.dist.h"
#else
#include "config.h"
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "portal_page.h"
#include <ArduinoJson.h>

static Preferences store;
static WebServer  web(80);
static DNSServer  dns;

static char  gotSsid[40] = "";
static char  gotPass[72] = "";
static char  gotHost[96] = "";
static char  gotToken[80] = "";
static int   gotEffect = -1;

// callback so the display can show what the portal is doing without this file
// needing to know anything about TFT_eSPI
static void (*onState)(const char *line1, const char *line2) = nullptr;

static void (*onTick)() = nullptr;

void provisionUI(void (*cb)(const char *, const char *)) { onState = cb; }
void provisionTick(void (*cb)()) { onTick = cb; }

static void (*onPortal)(const char *, const char *) = nullptr;
void provisionPortalUI(void (*cb)(const char *, const char *)) { onPortal = cb; }

// waits for the join, giving the caller a beat to animate roughly 3x a second
static bool waitJoin(uint32_t ms) {
    uint32_t t0 = millis(), last = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) {
        if (onTick && millis() - last > 380) { last = millis(); onTick(); }
        delay(40);
    }
    return WiFi.isConnected();
}

static void say(const char *a, const char *b) { if (onState) onState(a, b); }

// Avoids characters that are painful to read off a screen and retype: no O/0,
// no I/l/1. Eight characters is the WPA2 minimum.
const char *setupPassword() {
    static char pw[9] = "";
    if (pw[0]) return pw;

    static const char AB[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint64_t mac = ESP.getEfuseMac();

    for (int i = 0; i < 8; i++) {
        pw[i] = AB[mac % (sizeof(AB) - 1)];
        mac /= (sizeof(AB) - 1);
        if (!mac) mac = ESP.getEfuseMac() >> (i + 3);   // keep it varying
    }
    pw[8] = 0;
    return pw;
}

bool hasStoredWifi() {
    store.begin("wifi", true);
    bool has = store.isKey("ssid");
    store.end();
    return has;
}

void forgetWifi() {
    store.begin("wifi", false);
    store.clear();
    store.end();
    delay(200);
    ESP.restart();
}

static char hostBuf[96];
static char tokenBuf[80];

const char *cfgHost() {
    if (hostBuf[0]) return hostBuf;
    store.begin("cfg", true);
    String v = store.getString("host", "");
    store.end();
    strlcpy(hostBuf, v.length() ? v.c_str() : PULSE_HOST, sizeof(hostBuf));
    return hostBuf;
}

const char *cfgToken() {
    if (tokenBuf[0]) return tokenBuf;
    store.begin("cfg", true);
    String v = store.getString("token", "");
    store.end();
    strlcpy(tokenBuf, v.length() ? v.c_str() : DEVICE_TOKEN, sizeof(tokenBuf));
    return tokenBuf;
}

static int effectBuf = -1;

int cfgEffect() {
    if (effectBuf >= 0) return effectBuf;
    store.begin("cfg", true);
    effectBuf = store.getInt("effect", PUSH_EFFECT);
    store.end();
    return effectBuf;
}

void saveServerConfig(const char *host, const char *token) {
    store.begin("cfg", false);
    if (host && host[0])   { store.putString("host", host);   hostBuf[0] = 0; }
    if (token && token[0]) { store.putString("token", token); tokenBuf[0] = 0; }
    store.end();
}

void saveEffect(int e) {
    store.begin("cfg", false);
    store.putInt("effect", e);
    store.end();
    effectBuf = -1;
}

bool cfgAutoUpdate() {
    store.begin("cfg", true);
    bool on = store.getBool("autoup", false);
    store.end();
    return on;
}

void saveAutoUpdate(bool on) {
    store.begin("cfg", false);
    store.putBool("autoup", on);
    store.end();
}

void saveWifi(const char *ssid, const char *pass) {
    store.begin("wifi", false);
    store.putString("ssid", ssid);
    store.putString("pass", pass ? pass : "");
    store.end();
}

int scanNetworks(String &out) {
    int n = WiFi.scanNetworks();
    out = "[";
    for (int i = 0; i < n && i < 24; i++) {
        if (i) out += ",";
        String ss = WiFi.SSID(i);
        ss.replace("\\", "\\\\");
        ss.replace("\"", "\\\"");
        out += "{\"s\":\"" + ss + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
    }
    out += "]";
    WiFi.scanDelete();
    return n;
}

int cfgRotation() {
    store.begin("cfg", true);
    int r = store.getInt("rot", TFT_ROTATION);
    store.end();
    return (r == 1 || r == 3) ? r : TFT_ROTATION;
}

void saveRotation(int r) {
    if (r != 1 && r != 3) return;
    store.begin("cfg", false);
    store.putInt("rot", r);
    store.end();
}

static void (*onPreview)(int) = nullptr;
void provisionPreviewUI(void (*cb)(int)) { onPreview = cb; }

// Plays the chosen effect on the panel there and then. Picking a celebration
// from a list of three words is guesswork; watching one happen is not.
static void handlePreview() {
    JsonDocument d;
    deserializeJson(d, web.arg("plain"));
    int e = d["effect"] | 0;
    web.send(200, "application/json", "{\"ok\":true}");
    if (onPreview && e >= 0 && e <= 2) onPreview(e);
}

bool hasStoredConfig() {
    store.begin("cfg", true);
    bool ok = store.isKey("host") && store.isKey("token");
    store.end();
    return ok;
}

/* ------------------------------------------------------------- portal */

enum PortalState { P_FORM, P_TRYING, P_JOINED, P_DONE };
static PortalState pstate = P_FORM;

static void handleRoot() { web.send_P(200, "text/html", PORTAL_PAGE); }

static void handleScan() {
    int n = WiFi.scanNetworks();
    String out = "[";
    for (int i = 0; i < n && i < 24; i++) {
        if (i) out += ",";
        String ss = WiFi.SSID(i);
        ss.replace("\\", "\\\\");
        ss.replace("\"", "\\\"");
        out += "{\"s\":\"" + ss + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
    }
    web.send(200, "application/json", out + "]");
    WiFi.scanDelete();
}

// Starts the join and returns straight away. The board stays in AP mode
// throughout, so the phone keeps this page open and watches it happen instead
// of being dropped and left to guess.
static void handleWifi() {
    JsonDocument d;
    deserializeJson(d, web.arg("plain"));
    strlcpy(gotSsid, d["ssid"] | "", sizeof(gotSsid));
    strlcpy(gotPass, d["pass"] | "", sizeof(gotPass));

    Serial.printf("[portal] trying %s\n", gotSsid);
    WiFi.begin(gotSsid, gotPass);
    pstate = P_TRYING;
    web.send(200, "application/json", "{\"ok\":true}");
}

static void handleStatus() {
    const char *w = pstate == P_JOINED ? "ok"
                  : pstate == P_TRYING ? "trying" : "idle";
    String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
    web.send(200, "application/json",
             String("{\"wifi\":\"") + w + "\",\"ip\":\"" + ip + "\"}");
}

// Proves the pair actually works before storing it. Otherwise a typo is a
// silent failure found minutes later, on a screen with no keyboard.
// Split out so it can be driven from serial as well as from the form. Debugging
// this through a phone was guesswork: the failure and the only log line that
// would explain it were arriving on two different channels.
static int probeServer(const String &host, const String &token) {
    Serial.printf("[probe] %s  sta=%d ip=%s heap=%u\n", host.c_str(),
                  WiFi.status(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
    Serial.flush();
    Serial.printf("[portal] checking %s  sta=%d heap=%u\n",
                  host.c_str(), WiFi.status(), ESP.getFreeHeap());

    // The captive portal DNS server answers every lookup with our own address.
    // That is the whole trick, and it also means the board cannot resolve a real
    // hostname while it is running. Stop it for the duration of the check.
    dns.stop();

    // TLS wants tens of kilobytes and the web server task has a small stack, so
    // this goes on the heap. On the stack it does not fail cleanly, it just
    // fails.
    WiFiClientSecure *tls = nullptr;
    WiFiClient plain;
    WiFiClient *cl;
    if (host.startsWith("https")) {
        tls = new WiFiClientSecure;
        tls->setInsecure();
        tls->setTimeout(15);
        cl = tls;
    } else {
        cl = &plain;
    }

    HTTPClient http;
    int code = -1;
    if (http.begin(*cl, host + "/api/pulse")) {
        http.addHeader("x-device-token", token);
        http.setTimeout(15000);
        code = http.GET();
        http.end();
    } else {
        Serial.println("[portal] http.begin failed");
    }

    if (tls) delete tls;
    dns.start(53, "*", WiFi.softAPIP());

    Serial.printf("[probe] -> %d  heap now %u\n", code, ESP.getFreeHeap());
    Serial.flush();

    return code;
}

static const char *explain(int code) {
    if (code == 200) return nullptr;
    if (code == 401) return "reached it, but that token was rejected";
    if (code == 404) return "reached it, but it is not running this server";
    if (code == 302 || code == 307) return "that url is behind a login wall, try another of its addresses";
    if (code < 0) return "could not reach that url";
    return "unexpected reply from that url";
}

static void handleServer() {
    JsonDocument d;
    deserializeJson(d, web.arg("plain"));
    String host = String(d["host"] | "");
    String token = String(d["token"] | "");

    while (host.endsWith("/")) host.remove(host.length() - 1);

    if (!host.startsWith("http")) {
        web.send(200, "application/json",
                 "{\"ok\":false,\"error\":\"url should start with https://\"}");
        return;
    }

    int code = probeServer(host, token);
    const char *err = explain(code);

    if (err) {
        JsonDocument r; r["ok"] = false; r["error"] = err;
        r["code"] = code;
        String out; serializeJson(r, out);
        web.send(200, "application/json", out);
        return;
    }

    strlcpy(gotHost, host.c_str(), sizeof(gotHost));
    strlcpy(gotToken, token.c_str(), sizeof(gotToken));
    gotEffect = d["effect"] | 1;
    web.send(200, "application/json", "{\"ok\":true}");
}

static void handleDone() {
    web.send(200, "application/json", "{\"ok\":true}");
    pstate = P_DONE;
}

static void handleNotFound() { handleRoot(); }

static bool joinStored() {
    store.begin("wifi", true);
    String s = store.getString("ssid", "");
    String p = store.getString("pass", "");
    store.end();
    if (!s.length()) return false;

    say("connecting", s.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(s.c_str(), p.c_str());

    return waitJoin(20000);
}

static void runPortal() {
    // AP and station at once. The phone stays attached to the board while the
    // board joins the house wifi, which is what lets each step be checked
    // before moving on to the next.
    //
    // The AP has to sit on a subnet the house is not already using. Espressif's
    // default is 192.168.4.1, which is an extremely common home range, and when
    // both interfaces land in the same space routing quietly collapses: packets
    // leave by the wrong one and every lookup fails. That presents as "cannot
    // reach that url" while the wifi step said it connected perfectly.
    //
    // 192.168.32.x is picked for being unusual in consumer kit.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 32, 1),
                      IPAddress(192, 168, 32, 1),
                      IPAddress(255, 255, 255, 0));
    WiFi.softAP(SETUP_AP_NAME, setupPassword());
    IPAddress ip = WiFi.softAPIP();

    dns.start(53, "*", ip);          // every lookup points back at us
    web.on("/", handleRoot);
    web.on("/scan", handleScan);
    web.on("/wifi", HTTP_POST, handleWifi);
    web.on("/status", handleStatus);
    web.on("/server", HTTP_POST, handleServer);
    web.on("/preview", HTTP_POST, handlePreview);
    web.on("/done", HTTP_POST, handleDone);
    web.onNotFound(handleNotFound);
    web.begin();

    if (onPortal) onPortal(SETUP_AP_NAME, ip.toString().c_str());
    else say("join wifi: " SETUP_AP_NAME, ip.toString().c_str());

    pstate = P_FORM;
    uint32_t tryStart = 0;

    Serial.println("[portal] up. press 'v' to test the compiled-in server from here.");

    while (pstate != P_DONE) {
        dns.processNextRequest();
        web.handleClient();

        if (Serial.available()) {
            char c = Serial.read();
            if (c == 'v') probeServer(PULSE_HOST, DEVICE_TOKEN);
            if (c == 'w') Serial.printf("[portal] sta=%d ip=%s heap=%u\n", WiFi.status(),
                                        WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
        }

        if (pstate == P_TRYING) {
            if (!tryStart) tryStart = millis();
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[portal] joined, %s\n", WiFi.localIP().toString().c_str());
                pstate = P_JOINED;
            } else if (millis() - tryStart > 22000) {
                Serial.println("[portal] join failed");
                pstate = P_FORM;
                tryStart = 0;
                WiFi.disconnect();
            }
        }
        delay(4);
    }

    delay(400);                       // let the browser get the reply out
    web.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);

    store.begin("wifi", false);
    store.putString("ssid", gotSsid);
    store.putString("pass", gotPass);
    store.end();

    // blank means "leave what was there", so someone can fix a wifi password
    // without retyping their token
    if (gotHost[0] || gotToken[0] || gotEffect >= 0) {
        store.begin("cfg", false);
        if (gotHost[0])  { store.putString("host", gotHost);   hostBuf[0] = 0; }
        if (gotToken[0]) { store.putString("token", gotToken); tokenBuf[0] = 0; }
        if (gotEffect >= 0) { store.putInt("effect", gotEffect); effectBuf = -1; }
        store.end();
    }
}

bool setupWifi() {
    if (hasStoredWifi() && joinStored()) return true;

    // A build that still carries credentials keeps working. They get promoted
    // into NVS on first boot so from then on it takes the same path as a board
    // that was set up through the portal.
    if (strlen(WIFI_SSID)) {
        say("connecting", WIFI_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        if (waitJoin(20000)) {
            store.begin("wifi", false);
            store.putString("ssid", WIFI_SSID);
            store.putString("pass", WIFI_PASS);
            store.end();
            return true;
        }
    }

    // Either nothing stored or the stored network is gone. Both want the portal,
    // and looping means a wrong password is recoverable without a cable.
    for (int attempt = 0; attempt < 5; attempt++) {
        runPortal();
        if (joinStored()) return true;
        say("could not join", "try again");
        delay(1200);
    }

    return false;
}
