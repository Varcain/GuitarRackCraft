// AhbSpike.h — Phase 0 GO/NO-GO spike (see AhbSpike.cpp).
#pragma once

// Run the cross-driver AHardwareBuffer + fence interop spike in the calling
// (app) process. hookDir = app nativeLibraryDir, driverDir = <wineRoot>/turnip/,
// driverName = the Turnip HAL soname (e.g. "vulkan.ad07xx.so"). Logs every stage
// to logcat tag "AhbSpike" and a final PASS/FAIL. Returns true on PASS.
bool runAhbSpike(const char* hookDir, const char* driverDir, const char* driverName, const char* logPath);
