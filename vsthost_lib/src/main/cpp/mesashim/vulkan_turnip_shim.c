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

/* lavapipe (Mesa's software Vulkan) via the bundled Khronos loader. lavapipe is
 * an ICD (vk_icdGetInstanceProcAddr), so it's reached through the source loader
 * libvulkan.so.1 (a DISTINCT soname from this shim — no recursion; on
 * LD_LIBRARY_PATH in <wine>/turnip/) pointed at the lavapipe manifest via
 * VK_ICD_FILENAMES / VK_DRIVER_FILES. This is the UNIVERSAL fallback: pure
 * software, every Vulkan feature (incl. the robustness2/nullDescriptor DXVK needs
 * and Qualcomm/many vendors lack), works on any device/GPU — slow but correct.
 * VSTPOC_LAVAPIPE_ICD = absolute path to lvp_icd.aarch64.json (set by the host). */
static void *open_lavapipe(void) {
    const char *lvp = getenv("VSTPOC_LAVAPIPE_ICD");
    if (!lvp || !lvp[0]) { shim_log("vstpoc-vkshim: no VSTPOC_LAVAPIPE_ICD\n"); return NULL; }
    setenv("VK_ICD_FILENAMES", lvp, 1);  /* Khronos loader < 1.3.207 */
    setenv("VK_DRIVER_FILES",  lvp, 1);  /* Khronos loader >= 1.3.207 */
    /* zink's pdev selection (mesa patch 0001) computes:
     *   cpu = (LIBGL_ALWAYS_SOFTWARE || D3D_ALWAYS_SOFTWARE) && !VSTPOC_ZINK_FORCE_HW
     * LIBGL_ALWAYS_SOFTWARE=1 is set globally (the only way to reach the
     * surfaceless software loader → zink, with no DRM render node), and
     * VSTPOC_ZINK_FORCE_HW=1 is the global default so zink keeps the real HW
     * (Turnip) pdev. When we've deliberately loaded software lavapipe there is NO
     * hardware pdev, so force-HW makes zink reject lavapipe's CPU device and the
     * GL init wedges. Override it OFF here (this runs in the GL process before
     * zink's choose_pdev) so cpu=TRUE and zink selects lavapipe via its proper
     * zink_get_cpu_device_type path. LIBGL stays set by the host env. */
    setenv("VSTPOC_ZINK_FORCE_HW", "0", 1);
    void *h = dlopen("libvulkan.so.1", RTLD_NOW);
    shim_log(h ? "vstpoc-vkshim: lavapipe (software) via Khronos loader OK\n"
               : "vstpoc-vkshim: lavapipe Khronos loader dlopen FAILED\n");
    return h;
}

/* Load the Vulkan ICD, with a fallback chain so the GL editor isn't Adreno-locked:
 *   0. VSTPOC_FORCE_LAVAPIPE=1 → go straight to lavapipe (testing the fallback, or
 *      a device whose GPU Vulkan is known unusable for zink/DXVK).
 *   1. Turnip via libadrenotools (custom driver, KGSL) — the Adreno primary.
 *   2. The device's OWN Vulkan via adrenotools with no custom driver (feature
 *      flags 0) — namespace-aware, works on any vendor (Mali/PowerVR, or Adreno
 *      where Turnip/adrenotools is unavailable). This is what lets zink run
 *      off-Adreno (zink runs on any conformant Vulkan).
 *   3. dlopen the system libvulkan directly (absolute path so we don't re-open
 *      THIS shim, which mesa finds first as "libvulkan.so").
 *   4. lavapipe (software) — universal last resort if NO hardware Vulkan loads.
 * Returns a handle whose vkGetInstanceProcAddr we forward, or NULL. */
static void *load_vulkan(void) {
    void *h = NULL;
    const char *force = getenv("VSTPOC_FORCE_LAVAPIPE");
    if (force && force[0] == '1') {
        shim_log("vstpoc-vkshim: VSTPOC_FORCE_LAVAPIPE=1 — forcing software Vulkan\n");
        h = open_lavapipe();
        if (h) return h;
        shim_log("vstpoc-vkshim: forced lavapipe FAILED — falling through to hardware\n");
    }

    void *at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_GLOBAL);
    PFN_open_vk open_vk = at ? (PFN_open_vk)dlsym(at, "adrenotools_open_libvulkan") : NULL;
    if (!at)      shim_log("vstpoc-vkshim: dlopen libadrenotools FAILED\n");
    else if (!open_vk) shim_log("vstpoc-vkshim: no adrenotools_open_libvulkan\n");

    if (open_vk) {
        /* 1. Turnip (custom driver). */
        h = open_vk(RTLD_NOW, 1 /*ADRENOTOOLS_DRIVER_CUSTOM*/, NULL,
                    getenv("VSTPOC_ADRENOTOOLS_HOOKDIR"),
                    getenv("VSTPOC_ADRENOTOOLS_DRIVERDIR"),
                    getenv("VSTPOC_ADRENOTOOLS_DRIVERNAME"),
                    NULL, NULL);
        if (h) { shim_log("vstpoc-vkshim: Turnip (adrenotools custom driver) OK\n"); return h; }
        shim_log("vstpoc-vkshim: Turnip via adrenotools FAILED — trying system Vulkan\n");
        /* 2. System Vulkan via adrenotools, no custom driver (namespace-aware). */
        h = open_vk(RTLD_NOW, 0, NULL, getenv("VSTPOC_ADRENOTOOLS_HOOKDIR"),
                    NULL, NULL, NULL, NULL);
        if (h) { shim_log("vstpoc-vkshim: system Vulkan (adrenotools, no driver) OK\n"); return h; }
        shim_log("vstpoc-vkshim: system Vulkan via adrenotools FAILED — trying direct dlopen\n");
    }
    /* 3. System Vulkan loader directly (absolute path avoids re-opening this shim). */
    h = dlopen("/system/lib64/libvulkan.so", RTLD_NOW);
    if (h) { shim_log("vstpoc-vkshim: system Vulkan (/system/lib64/libvulkan.so) OK\n"); return h; }
    /* 4. lavapipe (software) — universal last resort. */
    shim_log("vstpoc-vkshim: no hardware Vulkan — trying lavapipe (software)\n");
    h = open_lavapipe();
    if (h) return h;
    shim_log("vstpoc-vkshim: NO Vulkan available (Turnip + system + lavapipe all failed)\n");
    return NULL;
}

static void shim_init(void) {
    if (g_tried) return;
    g_tried = 1;
    void *h = load_vulkan();
    if (!h) return;
    g_real = (PFN_gipa)dlsym(h, "vkGetInstanceProcAddr");
    shim_log(g_real ? "vstpoc-vkshim: vkGetInstanceProcAddr OK\n"
                    : "vstpoc-vkshim: no vkGetInstanceProcAddr in the loaded Vulkan\n");
    /* zink also dlsyms vkGetDeviceProcAddr directly. Prefer the lib's own export;
     * fall back to resolving it through vkGetInstanceProcAddr. */
    g_real_gdpa = (PFN_gdpa)dlsym(h, "vkGetDeviceProcAddr");
    if (!g_real_gdpa && g_real)
        g_real_gdpa = (PFN_gdpa)g_real(NULL, "vkGetDeviceProcAddr");
    shim_log(g_real_gdpa ? "vstpoc-vkshim: vkGetDeviceProcAddr OK\n"
                         : "vstpoc-vkshim: no vkGetDeviceProcAddr\n");
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
