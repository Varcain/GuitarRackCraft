/* Minimal win32 OpenGL app — run under wine to exercise the full
 * wine opengl32 -> winex11.drv -> GLX -> our X server path. Clears the window
 * GREEN and SwapBuffers; if our framebuffer turns green, wine GL works. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "gltestwin";
    wc.style = CS_OWNDC;
    RegisterClass(&wc);
    HWND hwnd = CreateWindow("gltestwin", "gltestwin",
                             WS_POPUP | WS_VISIBLE,
                             0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    fprintf(stderr, "ChoosePixelFormat -> %d\n", pf);
    if (!SetPixelFormat(dc, pf, &pfd)) { fprintf(stderr, "SetPixelFormat FAILED\n"); return 1; }

    HGLRC rc = wglCreateContext(dc);
    fprintf(stderr, "wglCreateContext -> %p\n", (void*)rc);
    if (!rc) { fprintf(stderr, "wglCreateContext FAILED\n"); return 1; }
    if (!wglMakeCurrent(dc, rc)) { fprintf(stderr, "wglMakeCurrent FAILED\n"); return 1; }
    fprintf(stderr, "GL_RENDERER=%s\n", (const char*)glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION=%s\n", (const char*)glGetString(GL_VERSION));

    for (int i = 0; i < 40; i++) {
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);   /* GREEN */
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(dc);
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        Sleep(60);
    }
    fprintf(stderr, "done, 40 green frames\n");
    return 0;
}
