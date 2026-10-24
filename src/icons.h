#pragma once
#include "common.h"

void IconsInit(HINSTANCE instance);
void IconsShutdown();

// Draws the tray icon for a device (nullptr = "unknown" icon). Caller owns the HICON.
HICON DrawTrayIcon(const DeviceState* device, bool numeric, bool lightTheme, int sizePx);

bool IsLightTaskbar();
int TrayIconSize();
