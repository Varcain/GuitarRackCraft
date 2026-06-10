// AhbChannelTest.h — Phase 2 synthetic side-channel client (throwaway).
//
// Validates the AF_UNIX AHB side-channel end-to-end WITHOUT wine: connect to the
// X server's abstract socket for `displayNumber`, allocate + CPU-fill a gradient
// AHardwareBuffer, REGISTER + PRESENT it, hold it for a few seconds (so the
// compositor samples it), then UNREGISTER. The X server's editor layer should
// show the gradient (distinct from the Phase 1 direct-hook gradient), proving
// the socket transport + SCM_RIGHTS AHB-handle passing work.
//
// Runs in the app process (same process as the server — AF_UNIX loopback). The
// real Phase 3 producer is the wine subprocess over this same socket.
#ifndef GUITARRACKCRAFT_AHB_CHANNEL_TEST_H
#define GUITARRACKCRAFT_AHB_CHANNEL_TEST_H

namespace guitarrackcraft {

// Connect to display `displayNumber`'s AHB side-channel and drive one
// REGISTER → PRESENT → (hold holdMs) → UNREGISTER cycle with a synthetic
// gradient AHB. Logs each step to `logPath` (logcat floods under audio).
// Returns true if the full cycle completed. Blocking — run on a worker thread.
bool runAhbChannelTest(int displayNumber, int holdMs, const char* logPath);

} // namespace guitarrackcraft

#endif
