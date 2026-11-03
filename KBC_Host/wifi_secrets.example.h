// Copy this file to "wifi_secrets.h" (same folder) and fill in your Wi-Fi.
// The host then also joins that network, so a laptop on it can open
// http://kbc.local without leaving its normal Wi-Fi.
// Leave both empty (or delete wifi_secrets.h) for a standalone host.
// wifi_secrets.h is in .gitignore, so your password never gets committed.
#pragma once
#define HOME_WIFI_SSID "YourWiFiName"
#define HOME_WIFI_PASS "YourWiFiPassword"
