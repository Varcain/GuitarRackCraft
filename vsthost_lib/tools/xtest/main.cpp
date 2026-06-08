/* Desktop GUI test harness for the Android in-process X11 server.
 *
 * Brings up ONLY the TCP X-protocol server thread (startServer -> serverLoop,
 * binds 127.0.0.1:6000+displayNumber). Never calls attachSurface, so the
 * EGL/GLES/ANativeWindow compositor path (all stubbed) is never executed.
 * Instead we read the rendered pixels directly out of the X server's
 * framebuffer (snapshotFramebuffer) and blit them into a real desktop SDL2
 * window, and forward mouse input back into the server via injectTouch.
 *
 * This lets us bring up and click on a Windows VST plugin's UI on a Linux PC,
 * driving the exact same X server code that runs in-process on Android.
 *
 * A wine VST host should connect to DISPLAY=127.0.0.1:1 (printed at startup).
 *
 * Usage: xtest [width height]
 *   SDL_VIDEODRIVER=dummy   -> headless init (CI / no display); idles.
 */

#include "X11NativeDisplay.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace guitarrackcraft;

static const int kDisplayNumber = 1;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    int initW = 1024, initH = 768;
    if (argc >= 3) { initW = atoi(argv[1]); initH = atoi(argv[2]); }

    // --- 1. Bring up the X server ------------------------------------------
    X11NativeDisplay* disp = getOrCreateX11Display(kDisplayNumber);
    if (!disp) {
        fprintf(stderr, "FATAL: getOrCreateX11Display(%d) returned null\n", kDisplayNumber);
        return 1;
    }
    if (!disp->startServer(initW, initH)) {
        fprintf(stderr, "FATAL: startServer(%d, %d) failed\n", initW, initH);
        return 1;
    }
    int port = disp->getActualPort();
    int dispNo = port >= 6000 ? port - 6000 : kDisplayNumber;
    printf("XTEST_READY port=%d display=:%d screen=%dx%d\n", port, dispNo, initW, initH);
    printf("XTEST_DISPLAY=127.0.0.1:%d\n", dispNo);
    printf("Set the wine VST host to DISPLAY=127.0.0.1:%d\n", dispNo);
    fflush(stdout);

    // --- 2. Bring up SDL ----------------------------------------------------
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "FATAL: SDL_Init failed: %s\n", SDL_GetError());
        destroyX11Display(kDisplayNumber);
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "vstpoc X11 harness :1",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        initW, initH,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "FATAL: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        destroyX11Display(kDisplayNumber);
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        // dummy video driver / headless: software renderer fallback.
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "FATAL: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        destroyX11Display(kDisplayNumber);
        return 1;
    }

    // Streaming texture: ARGB8888 matches the X11 wire-order pixels we copy
    // out of the framebuffer. (If colors look swapped on a real run, swap to
    // SDL_PIXELFORMAT_BGRA8888 — known follow-up, see task notes.)
    int texW = initW, texH = initH;
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, texW, texH);
    if (!texture) {
        fprintf(stderr, "FATAL: SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        destroyX11Display(kDisplayNumber);
        return 1;
    }

    printf("XTEST_SDL_UP texture=%dx%d\n", texW, texH);
    fflush(stdout);

    // --- 3. Main loop -------------------------------------------------------
    // We keep the SDL window size == framebuffer size so window coords map
    // 1:1 to plugin coords (RenderCopy then stretches the texture to fill,
    // which is a no-op while they're equal). setSurfaceSize(w,h) keeps the X
    // server's injectTouch letterbox at 1:1 too.
    std::vector<uint32_t> buf;
    bool pluginSizeApplied = false;
    bool mouseDown = false;
    bool running = true;

    // Identity surface size so injectTouch maps 1:1 from the start.
    disp->setSurfaceSize(initW, initH);

    SDL_Event ev;
    while (running) {
        // 3a. Track the plugin's natural size; resize window+texture to match.
        int pw = 0, ph = 0;
        if (disp->getPluginSize(pw, ph) && pw > 0 && ph > 0) {
            if (!pluginSizeApplied || pw != texW || ph != texH) {
                SDL_SetWindowSize(window, pw, ph);
                SDL_Texture* nt = SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, pw, ph);
                if (nt) {
                    SDL_DestroyTexture(texture);
                    texture = nt;
                    texW = pw;
                    texH = ph;
                    // 1:1 input mapping at the new size.
                    disp->setSurfaceSize(pw, ph);
                    pluginSizeApplied = true;
                    printf("XTEST_RESIZE plugin=%dx%d\n", pw, ph);
                    fflush(stdout);
                }
            }
        }

        // 3b. Pull the latest framebuffer and present it.
        int fw = 0, fh = 0;
        if (disp->snapshotFramebuffer(buf, fw, fh) && fw > 0 && fh > 0) {
            // If the framebuffer grew/shrank vs the texture (e.g. slot
            // stacking) reallocate the texture to its exact dimensions so
            // the pitch is right and nothing is clipped.
            if (fw != texW || fh != texH) {
                SDL_Texture* nt = SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, fw, fh);
                if (nt) {
                    SDL_DestroyTexture(texture);
                    texture = nt;
                    texW = fw;
                    texH = fh;
                }
            }
            if (fw == texW && fh == texH) {
                SDL_UpdateTexture(texture, nullptr, buf.data(), fw * (int)sizeof(uint32_t));
            }
            // Headless verification: every ~2s log framebuffer stats and dump
            // a PPM so renders can be inspected without eyes on the SDL window.
            static int frameN = 0;
            if (++frameN % 120 == 1 && !buf.empty()) {
                size_t nonWhite = 0, nonPh = 0;
                for (uint32_t px : buf) {
                    if ((px & 0xFFFFFFu) != 0xFFFFFFu) nonWhite++;
                    if (px != 0xFF302020u) nonPh++;
                }
                uint32_t c = buf[(size_t)(fh/2)*fw + fw/2];
                fprintf(stderr, "XTEST_FB %dx%d center=%08x nonWhite=%zu/%zu nonPlaceholder=%zu\n",
                        fw, fh, c, nonWhite, buf.size(), nonPh);
                FILE* f = fopen("/tmp/xtest/fb.ppm", "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", fw, fh);
                    for (uint32_t px : buf) {
                        unsigned char rgb[3] = {(unsigned char)((px>>16)&0xFF),
                                                (unsigned char)((px>>8)&0xFF),
                                                (unsigned char)(px&0xFF)};
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                }
            }
        }
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);  // stretch to window
        SDL_RenderPresent(renderer);

        // 3b'. Scripted injection for autonomous/headless testing. Each frame,
        // read /tmp/xtest/inject.txt; for every line "<action> <x> <y>" call
        // injectTouch (0=down,1=up,2=move,3=rclick), then truncate the file.
        // Drive a click with:  printf '0 128 200\n1 128 200\n' > /tmp/xtest/inject.txt
        {
            FILE* inj = fopen("/tmp/xtest/inject.txt", "r");
            if (inj) {
                int a, ix, iy;
                int n = 0;
                while (fscanf(inj, "%d %d %d", &a, &ix, &iy) == 3) {
                    disp->injectTouch(a, ix, iy);
                    if (a == 0) mouseDown = true; else if (a == 1) mouseDown = false;
                    fprintf(stderr, "XTEST_INJECT action=%d (%d,%d)\n", a, ix, iy);
                    ++n;
                }
                fclose(inj);
                if (n > 0) { FILE* t = fopen("/tmp/xtest/inject.txt", "w"); if (t) fclose(t); }
            }
        }

        // 3c. Handle input/quit. Window coords == framebuffer coords (1:1).
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    // Ignore: :10 / WSLg can send spurious QUIT/CLOSE during a
                    // plugin session, which was killing the harness mid-test and
                    // breaking the plugin's X connection. The harness now stays
                    // alive until the process is killed (the driver kills the task).
                    fprintf(stderr, "XTEST_IGNORE_QUIT (staying alive)\n");
                    break;
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_CLOSE)
                        fprintf(stderr, "XTEST_IGNORE_CLOSE (staying alive)\n");
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        mouseDown = true;
                        disp->injectTouch(0, ev.button.x, ev.button.y);  // down
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        mouseDown = false;
                        disp->injectTouch(1, ev.button.x, ev.button.y);  // up
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (mouseDown)
                        disp->injectTouch(2, ev.motion.x, ev.motion.y);  // move
                    break;
                default:
                    break;
            }
        }

        SDL_Delay(16);  // ~60 fps
    }

    // --- 4. Teardown --------------------------------------------------------
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    destroyX11Display(kDisplayNumber);
    printf("XTEST_EXIT\n");
    fflush(stdout);
    return 0;
}
