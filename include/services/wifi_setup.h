#include <stdint.h>
#pragma once

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();

// 0 unless BOOT has been held for at least 10 seconds.
uint8_t wifiResetCountdownSeconds();

/** Bounded Wi-Fi configuration pause; non-blocking and self-expiring. */
bool wifiRadarWorkPaused();
/** Non-blocking short display-only pause following a radar web response. */
bool wifiDisplayYieldForWeb();
/** Non-blocking gate: false if a save is active or TLS is briefly deferred. */
bool wifiTryBeginAdsbFetch();
/** Release the gate after the entire HTTPS request/cleanup. */
void wifiEndAdsbFetch();
