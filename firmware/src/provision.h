#pragma once
#include <Arduino.h>

// WiFi credentials live in NVS, not in the build. A board with none starts an
// open AP and serves a page that writes them, so the same binary works on
// anyone's network and the password never leaves the device.
//
// setupWifi() returns true once connected. It only comes back false if the user
// never completed the portal, in which case there is nothing useful to show.
bool setupWifi();

// wipes stored credentials and reboots into the portal
void forgetWifi();

bool hasStoredWifi();

// The setup network's password. Derived from this board's MAC so it is stable
// across reboots and different on every unit, and shown on screen so the owner
// can join by hand if they would rather not scan.
const char *setupPassword();

// Server URL and device token, from NVS if the portal has set them, otherwise
// whatever was compiled in. A handed-out build has neither, which is the point:
// the same image has to work for someone pointing at their own deployment.
const char *cfgHost();
const char *cfgToken();
bool hasStoredConfig();

// which celebration plays when a push lands. 0 flash, 1 confetti, 2 ring.
int cfgEffect();

// lets the portal play an effect on the panel so it can be picked by eye
void provisionPreviewUI(void (*cb)(int effect));

// lets the caller draw portal state without this file knowing about the display
void provisionUI(void (*cb)(const char *line1, const char *line2));

// called repeatedly while we sit waiting, so the caller can animate something
void provisionTick(void (*cb)());

// fired once the setup AP is up, with the network name and the portal address
void provisionPortalUI(void (*cb)(const char *apName, const char *ip));

// write host and token from somewhere other than the setup portal
void saveServerConfig(const char *host, const char *token);
void saveEffect(int effect);

// off by default. silently replacing firmware on somebody else's desk is not a
// default anyone chose, so they have to ask for it.
bool cfgAutoUpdate();
void saveAutoUpdate(bool on);

// change network without clearing everything else
void saveWifi(const char *ssid, const char *pass);
int  scanNetworks(String &jsonOut);

// 1 is landscape, 3 is the same flipped end for end. Portrait is deliberately
// not offered: every screen is laid out for 320x240 and would need redesigning,
// not rotating.
int  cfgRotation();
void saveRotation(int rot);
