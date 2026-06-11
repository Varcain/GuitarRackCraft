/* vstpoc: a drop-in libvulkan.so that Mesa/zink dlopen("libvulkan.so") picks up
 * (mesa dir is first on its search path). It forwards vkGetInstanceProcAddr to
 * Turnip, loaded as the Android-HAL driver via libadrenotools (KGSL) — the only
 * way Turnip works on this device (/dev/dri/renderD128 is permission-denied,
 * /dev/kgsl-3d0 is world-rw). zink (via the kopper/galliumvk DRI path) dlsyms
 * BOTH vkGetInstanceProcAddr AND vkGetDeviceProcAddr from the lib it opens
 * (zink_screen.c get_instance/get_device proc addr) — forwarding only the
 * instance one made zink bail with "ZINK: failed to get proc address", so we
 * resolve and re-export both from the Turnip handle.
 * HOOKDIR/DRIVERDIR/DRIVERNAME come from the VSTPOC_ADRENOTOOLS_* env that
 * WineHostProcess already sets (same as DXVK's win32u/vulkan.c). */
#include <dlfcn.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>

typedef void *(*PFN_open_vk)(int, int, const char *, const char *, const char *,
                             const char *, const char *, void **);
typedef void *(*PFN_gipa)(void *instance, const char *name);
typedef void *(*PFN_gdpa)(void *device, const char *name);

static PFN_gipa g_real;
static PFN_gdpa g_real_gdpa;
static int g_tried;

static void shim_log(const char *s) { (void)write(2, s, __builtin_strlen(s)); }

static void shim_init(void) {
    if (g_tried) return;
    g_tried = 1;
    void *at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_GLOBAL);
    if (!at) { shim_log("vstpoc-vkshim: dlopen libadrenotools FAILED\n"); return; }
    PFN_open_vk open_vk = (PFN_open_vk)dlsym(at, "adrenotools_open_libvulkan");
    if (!open_vk) { shim_log("vstpoc-vkshim: no adrenotools_open_libvulkan\n"); return; }
    void *h = open_vk(RTLD_NOW, 1 /*ADRENOTOOLS_DRIVER_CUSTOM*/, NULL,
                      getenv("VSTPOC_ADRENOTOOLS_HOOKDIR"),
                      getenv("VSTPOC_ADRENOTOOLS_DRIVERDIR"),
                      getenv("VSTPOC_ADRENOTOOLS_DRIVERNAME"),
                      NULL, NULL);
    if (!h) { shim_log("vstpoc-vkshim: adrenotools_open_libvulkan FAILED\n"); return; }
    g_real = (PFN_gipa)dlsym(h, "vkGetInstanceProcAddr");
    shim_log(g_real ? "vstpoc-vkshim: Turnip vkGetInstanceProcAddr OK\n"
                    : "vstpoc-vkshim: no vkGetInstanceProcAddr in Turnip\n");
    /* zink also dlsyms vkGetDeviceProcAddr directly. Prefer the lib's own
     * export; fall back to resolving it through vkGetInstanceProcAddr. */
    g_real_gdpa = (PFN_gdpa)dlsym(h, "vkGetDeviceProcAddr");
    if (!g_real_gdpa && g_real)
        g_real_gdpa = (PFN_gdpa)g_real(NULL, "vkGetDeviceProcAddr");
    shim_log(g_real_gdpa ? "vstpoc-vkshim: Turnip vkGetDeviceProcAddr OK\n"
                         : "vstpoc-vkshim: no vkGetDeviceProcAddr in Turnip\n");
}

__attribute__((visibility("default")))
void *vkGetInstanceProcAddr(void *instance, const char *name) {
    if (!g_tried) shim_init();
    return g_real ? g_real(instance, name) : NULL;
}

__attribute__((visibility("default")))
void *vkGetDeviceProcAddr(void *device, const char *name) {
    if (!g_tried) shim_init();
    return g_real_gdpa ? g_real_gdpa(device, name) : NULL;
}
