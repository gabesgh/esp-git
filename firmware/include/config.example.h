// copy to config.h and fill in. config.h is gitignored.
#pragma once

#define WIFI_SSID     ""
#define WIFI_PASS     ""

// your deployed vercel url, no trailing slash
#define PULSE_HOST    "https://your-project.vercel.app"

// matches DEVICE_TOKEN on the server. this is readable by anyone holding the
// board, which is fine, it only reads counters.
#define DEVICE_TOKEN  ""

// stats move slowly, the pulse is what needs to feel instant
#define STATS_INTERVAL_MS 30000
#define PULSE_INTERVAL_MS 10000

/* ------------------------------------------------------------------
   Board profile. Defaults are the Cheap Yellow Display
   (ESP32-2432S028R): ILI9341 240x320 + XPT2046 resistive touch.

   The display itself is configured through TFT_eSPI's own build flags,
   see platformio.ini. Only the extras live here.
   ------------------------------------------------------------------ */

#define TFT_ROTATION  1       // 1 = landscape, fits a 53-week grid properly
#define TFT_BL        21      // backlight

// XPT2046 sits on its own SPI bus on the CYD
#define TOUCH_CS      33
#define TOUCH_IRQ     36
#define TOUCH_SCK     25
#define TOUCH_MISO    39
#define TOUCH_MOSI    32

// raw touch range, calibrate with the sketch in docs/ if drags land off target
#define TOUCH_MIN_X   200
#define TOUCH_MAX_X   3700
#define TOUCH_MIN_Y   240
#define TOUCH_MAX_Y   3800

// navigation is by tap: right half of the screen goes forward, left half back.
// this is only still here because the touch probe reports drag travel against
// it when you are calibrating.
#define SWIPE_MIN_PX  40

// which animation plays when a push lands.
//   0 = flash     full green wash, loudest
//   1 = confetti  green particles over the current view
//   2 = ring      pulsing border, keeps the screen readable
#define PUSH_EFFECT 1

// how long the page dots linger after you touch the screen. they hide again
// so the display is clean when you are just glancing at it.
#define DOTS_TIMEOUT_MS 5000

// how often to ask the server whether there is a newer build. only the tiny
// manifest is fetched on each check; the image itself downloads on a change.
#define OTA_INTERVAL_MS 3600000

// vertical drag = brightness. floor is deliberately not zero, a black panel
// looks like a dead one and you would be swiping blind to recover it.
#define VDRAG_MIN_PX   25
#define BRIGHTNESS_MIN 25

// wordmark on the boot splash
#define BOOT_NAME "esp git"

// open network the board publishes when it has no wifi stored
#define SETUP_AP_NAME "esp-git-setup"

// Which board this image is built for. The update check refuses anything not
// built for the same one, so a server holding an image for a different profile
// cannot brick a board by handing it over.
#define BOARD_ID "cyd-esp32-4mb"
