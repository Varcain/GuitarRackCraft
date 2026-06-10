// AhbSpike.cpp — Phase 0 GO/NO-GO spike for the GPU X-server upgrade.
//
// Question it answers: does the cross-driver, cross-API buffer+fence chain that
// the zero-copy present depends on actually work on THIS device?
//
//   Turnip(Vulkan, via libadrenotools — exactly how wine/DXVK gets Turnip)
//     -> allocate an AHardwareBuffer, import as a VkImage, clear it to a known
//        colour, export a VK_KHR_external_fence_fd sync-fd
//   -> (optionally round-trip the AHB handle over a socketpair, SCM_RIGHTS)
//   -> Adreno(system EGL) import the AHB as an EGLImage, wait on the sync-fd via
//        EGL_ANDROID_native_fence_sync, bind as a GL texture, read it back
//   -> assert the pixels equal the cleared colour.
//
// Runs IN the app process (untrusted_app) so it has GPU access and the
// adrenotools linker-namespace works, same as the wine subprocess. Logs every
// step + a final PASS/FAIL with the failing stage. Throwaway diagnostic.
//
// If this PASSES, the AHB+EGLImage zero-copy path (plan Phases 1-4) is viable.
// If it FAILS, the failing stage tells us which primitive/driver gap kills it.

#include "AhbSpike.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

#define TAG "AhbSpike"

// Mirror every line to BOTH logcat and a file — the app's audio engine floods
// logcat and evicts these within ~1s, so the file is the reliable record.
static FILE* g_spikeLog = nullptr;
static void spikeLog(int prio, const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    __android_log_write(prio, TAG, buf);
    if (g_spikeLog) { fputs(buf, g_spikeLog); fputc('\n', g_spikeLog); fflush(g_spikeLog); }
}
#define LOGI(...) spikeLog(ANDROID_LOG_INFO,  __VA_ARGS__)
#define LOGE(...) spikeLog(ANDROID_LOG_ERROR, __VA_ARGS__)

// One place to bail with a clear failing-stage message. `stage` is always a
// string literal so it concatenates into the format string (keeps %-args in
// order; no %s consuming `stage` out of sequence).
#define FAIL(stage, ...) do { LOGE("FAIL @ " stage ": " __VA_ARGS__); LOGE("=== AHB SPIKE RESULT: FAIL (" stage ") ==="); return false; } while (0)
#define VKCHECK(stage, expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) FAIL(stage, "%s = VkResult %d", #expr, (int)_r); } while (0)

namespace {

constexpr int kW = 256, kH = 256;
// Distinct per-channel clear colour so a mismatch is unambiguous.
constexpr float kClearR = 0.25f, kClearG = 0.50f, kClearB = 0.75f, kClearA = 1.0f;
constexpr uint8_t kExpR = 64, kExpG = 128, kExpB = 191, kExpA = 255;  // ~clear*255

bool hasExt(const char* exts, const char* want) {
    if (!exts || !want) return false;
    size_t n = strlen(want);
    const char* p = exts;
    while ((p = strstr(p, want))) {
        char before = (p == exts) ? ' ' : p[-1];
        char after  = p[n];
        if ((before == ' ' || before == '\0') && (after == ' ' || after == '\0')) return true;
        p += n;
    }
    return false;
}

// ---- adrenotools Turnip loader (mirrors wine patch 0030) -------------------
typedef void* (*PFN_adrenotools_open_libvulkan)(int, int, const char*, const char*,
                                                const char*, const char*, const char*, void**);

void* openTurnipVulkan(const char* hookDir, const char* driverDir, const char* driverName) {
    void* at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_GLOBAL);
    if (!at) { LOGE("dlopen libadrenotools.so failed: %s", dlerror()); return nullptr; }
    auto open_vk = (PFN_adrenotools_open_libvulkan)dlsym(at, "adrenotools_open_libvulkan");
    if (!open_vk) { LOGE("dlsym adrenotools_open_libvulkan failed"); return nullptr; }
    // flags=RTLD_NOW, features=1 (ADRENOTOOLS_DRIVER_CUSTOM), tmp=NULL, hookDir,
    // driverDir, driverName, fileRedirect=NULL, reserved=NULL
    void* h = open_vk(RTLD_NOW, 1, nullptr, hookDir, driverDir, driverName, nullptr, nullptr);
    if (!h) { LOGE("adrenotools_open_libvulkan returned NULL (hook=%s driver=%s/%s)",
                   hookDir, driverDir, driverName); return nullptr; }
    LOGI("Turnip libvulkan opened via adrenotools (driver=%s)", driverName);
    return h;
}

// ---- Vulkan function table (loaded through the Turnip handle's gipa) --------
struct Vk {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumPhys = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysProps = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueProps = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkGetDeviceQueue getQueue = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkAllocateMemory allocMem = nullptr;
    PFN_vkBindImageMemory bindImage = nullptr;
    PFN_vkCreateCommandPool createCmdPool = nullptr;
    PFN_vkAllocateCommandBuffers allocCmd = nullptr;
    PFN_vkBeginCommandBuffer beginCmd = nullptr;
    PFN_vkEndCommandBuffer endCmd = nullptr;
    PFN_vkCmdClearColorImage cmdClear = nullptr;
    PFN_vkCmdPipelineBarrier cmdBarrier = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkWaitForFences waitFences = nullptr;
    // ext
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProps = nullptr;
    PFN_vkGetFenceFdKHR getFenceFd = nullptr;
};

bool loadVk(Vk& vk, void* handle, VkInstance inst, VkDevice dev) {
    if (!vk.gipa) {
        vk.gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
        if (!vk.gipa) { LOGE("dlsym vkGetInstanceProcAddr failed"); return false; }
    }
#define LI(name, field) vk.field = (PFN_##name)vk.gipa(inst, #name); if (!vk.field) { LOGE("gipa %s NULL", #name); return false; }
#define LD(name, field) vk.field = (PFN_##name)vk.gdpa(dev, #name); if (!vk.field) { LOGE("gdpa %s NULL", #name); return false; }
    if (!inst) { // instance-less bootstrap
        LI(vkCreateInstance, createInstance);
        return true;
    }
    if (!dev) { // instance-level
        LI(vkEnumeratePhysicalDevices, enumPhys);
        LI(vkGetPhysicalDeviceProperties, getPhysProps);
        LI(vkGetPhysicalDeviceMemoryProperties, getMemProps);
        LI(vkGetPhysicalDeviceQueueFamilyProperties, getQueueProps);
        LI(vkEnumerateDeviceExtensionProperties, enumDevExt);
        LI(vkCreateDevice, createDevice);
        LI(vkGetDeviceProcAddr, gdpa);
        return true;
    }
    // device-level
    LD(vkGetDeviceQueue, getQueue);
    LD(vkCreateImage, createImage);
    LD(vkAllocateMemory, allocMem);
    LD(vkBindImageMemory, bindImage);
    LD(vkCreateCommandPool, createCmdPool);
    LD(vkAllocateCommandBuffers, allocCmd);
    LD(vkBeginCommandBuffer, beginCmd);
    LD(vkEndCommandBuffer, endCmd);
    LD(vkCmdClearColorImage, cmdClear);
    LD(vkCmdPipelineBarrier, cmdBarrier);
    LD(vkQueueSubmit, queueSubmit);
    LD(vkCreateFence, createFence);
    LD(vkWaitForFences, waitFences);
    LD(vkGetAndroidHardwareBufferPropertiesANDROID, getAhbProps);
    LD(vkGetFenceFdKHR, getFenceFd);
#undef LI
#undef LD
    return true;
}

} // namespace

bool runAhbSpike(const char* hookDir, const char* driverDir, const char* driverName, const char* logPath) {
    if (logPath && *logPath) g_spikeLog = fopen(logPath, "w");
    LOGI("=== AHB SPIKE START (hook=%s driver=%s/%s) ===", hookDir, driverDir, driverName);

    // ====================================================================
    // STAGE 1 — EGL consumer context + capability probe
    // ====================================================================
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) FAIL("egl-display", "eglGetDisplay");
    if (!eglInitialize(dpy, nullptr, nullptr)) FAIL("egl-init", "eglInitialize");

    const char* eglExts = eglQueryString(dpy, EGL_EXTENSIONS);
    LOGI("EGL_EXTENSIONS: %s", eglExts ? eglExts : "(null)");
    bool eglImgOk   = hasExt(eglExts, "EGL_ANDROID_image_native_buffer") && hasExt(eglExts, "EGL_KHR_image_base");
    bool eglFenceOk = hasExt(eglExts, "EGL_ANDROID_native_fence_sync");
    LOGI("EGL caps: image_native_buffer+image_base=%d  native_fence_sync=%d", eglImgOk, eglFenceOk);

    const EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_NONE };
    EGLConfig cfg; EGLint nc = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &nc) || nc < 1) FAIL("egl-config", "eglChooseConfig n=%d", nc);
    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) FAIL("egl-ctx", "eglCreateContext");
    const EGLint pbAttr[] = { EGL_WIDTH, kW, EGL_HEIGHT, kH, EGL_NONE };
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    if (pb == EGL_NO_SURFACE) FAIL("egl-pbuffer", "eglCreatePbufferSurface");
    if (!eglMakeCurrent(dpy, pb, pb, ctx)) FAIL("egl-makecurrent", "eglMakeCurrent");

    const char* glExts = (const char*)glGetString(GL_EXTENSIONS);
    bool glEglImgOk = hasExt(glExts, "GL_OES_EGL_image");
    LOGI("GL_RENDERER=%s  GL_OES_EGL_image=%d", glGetString(GL_RENDERER), glEglImgOk);

    auto eglGetNativeClientBufferANDROID_ =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    auto eglCreateImageKHR_  = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    auto eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    auto glEGLImageTargetTexture2DOES_ =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    auto eglCreateSyncKHR_      = (PFNEGLCREATESYNCKHRPROC)     eglGetProcAddress("eglCreateSyncKHR");
    auto eglClientWaitSyncKHR_  = (PFNEGLCLIENTWAITSYNCKHRPROC) eglGetProcAddress("eglClientWaitSyncKHR");
    auto eglDestroySyncKHR_     = (PFNEGLDESTROYSYNCKHRPROC)    eglGetProcAddress("eglDestroySyncKHR");
    if (!eglGetNativeClientBufferANDROID_ || !eglCreateImageKHR_ || !glEGLImageTargetTexture2DOES_)
        FAIL("egl-entrypoints", "missing AHB/EGLImage entrypoints (getNCB=%p createImg=%p target2D=%p)",
             (void*)eglGetNativeClientBufferANDROID_, (void*)eglCreateImageKHR_, (void*)glEGLImageTargetTexture2DOES_);
    if (!eglImgOk || !glEglImgOk) FAIL("egl-caps", "required EGL/GLES AHB-import extensions absent");

    // ====================================================================
    // STAGE 2 — Turnip Vulkan producer
    // ====================================================================
    void* vkHandle = openTurnipVulkan(hookDir, driverDir, driverName);
    if (!vkHandle) FAIL("turnip-open", "could not open Turnip libvulkan");

    Vk vk{};
    if (!loadVk(vk, vkHandle, VK_NULL_HANDLE, VK_NULL_HANDLE)) FAIL("vk-bootstrap", "load vkCreateInstance");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instExts[] = { "VK_KHR_get_physical_device_properties2",
                               "VK_KHR_external_memory_capabilities",
                               "VK_KHR_external_fence_capabilities" };
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 3; ici.ppEnabledExtensionNames = instExts;
    VkInstance inst = VK_NULL_HANDLE;
    VKCHECK("vk-instance", vk.createInstance(&ici, nullptr, &inst));
    if (!loadVk(vk, vkHandle, inst, VK_NULL_HANDLE)) FAIL("vk-inst-fns", "load instance fns");

    uint32_t nphys = 0; vk.enumPhys(inst, &nphys, nullptr);
    if (!nphys) FAIL("vk-phys", "no physical devices");
    std::vector<VkPhysicalDevice> phys(nphys); vk.enumPhys(inst, &nphys, phys.data());
    VkPhysicalDevice pd = phys[0];
    VkPhysicalDeviceProperties pdp; vk.getPhysProps(pd, &pdp);
    LOGI("Vulkan device: '%s' (driver should be Turnip)", pdp.deviceName);

    uint32_t nde = 0; vk.enumDevExt(pd, nullptr, &nde, nullptr);
    std::vector<VkExtensionProperties> de(nde); vk.enumDevExt(pd, nullptr, &nde, de.data());
    auto hasDevExt = [&](const char* n){ for (auto& e : de) if (!strcmp(e.extensionName, n)) return true; return false; };
    bool vkAhbOk   = hasDevExt("VK_ANDROID_external_memory_android_hardware_buffer");
    bool vkFenceOk = hasDevExt("VK_KHR_external_fence_fd");
    LOGI("Vulkan caps: AHB_external_memory=%d  external_fence_fd=%d  dedicated_alloc=%d  ycbcr=%d",
         vkAhbOk, vkFenceOk, hasDevExt("VK_KHR_dedicated_allocation"), hasDevExt("VK_KHR_sampler_ycbcr_conversion"));
    if (!vkAhbOk) FAIL("vk-caps", "Turnip lacks VK_ANDROID_external_memory_android_hardware_buffer");

    uint32_t nqf = 0; vk.getQueueProps(pd, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nqf); vk.getQueueProps(pd, &nqf, qf.data());
    uint32_t qfi = 0; bool foundQ = false;
    for (uint32_t i = 0; i < nqf; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; foundQ = true; break; }
    if (!foundQ) FAIL("vk-queue", "no graphics queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    std::vector<const char*> devExts = {
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
        "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
        "VK_KHR_bind_memory2", "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1",
        "VK_EXT_queue_family_foreign",
    };
    if (vkFenceOk) { devExts.push_back("VK_KHR_external_fence"); devExts.push_back("VK_KHR_external_fence_fd"); }
    // Drop any the device doesn't actually list, to avoid CreateDevice failure.
    std::vector<const char*> useExts;
    for (auto* e : devExts) if (hasDevExt(e)) useExts.push_back(e);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)useExts.size(); dci.ppEnabledExtensionNames = useExts.data();
    VkDevice dev = VK_NULL_HANDLE;
    VKCHECK("vk-device", vk.createDevice(pd, &dci, nullptr, &dev));
    if (!loadVk(vk, vkHandle, inst, dev)) FAIL("vk-dev-fns", "load device fns");
    VkQueue queue; vk.getQueue(dev, qfi, 0, &queue);

    // ====================================================================
    // STAGE 3 — allocate AHardwareBuffer
    // ====================================================================
    AHardwareBuffer_Desc adesc{};
    adesc.width = kW; adesc.height = kH; adesc.layers = 1;
    adesc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    adesc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&adesc, &ahb) != 0 || !ahb)
        FAIL("ahb-alloc", "AHardwareBuffer_allocate (color_output|sampled, RGBA8 %dx%d)", kW, kH);
    LOGI("AHardwareBuffer allocated %dx%d RGBA8", kW, kH);

    // ====================================================================
    // STAGE 4 — Vulkan import AHB as a VkImage and clear it
    // ====================================================================
    VkAndroidHardwareBufferFormatPropertiesANDROID afmt{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID aprops{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    aprops.pNext = &afmt;
    VKCHECK("vk-ahb-props", vk.getAhbProps(dev, ahb, &aprops));
    LOGI("AHB props: allocationSize=%llu memoryTypeBits=0x%x vkFormat=%d",
         (unsigned long long)aprops.allocationSize, aprops.memoryTypeBits, (int)afmt.format);

    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo imgci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgci.pNext = &emici;
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = (afmt.format != VK_FORMAT_UNDEFINED) ? afmt.format : VK_FORMAT_R8G8B8A8_UNORM;
    imgci.extent = {kW, kH, 1};
    imgci.mipLevels = 1; imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imgci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VKCHECK("vk-create-image", vk.createImage(dev, &imgci, nullptr, &image));

    uint32_t memType = 0; bool gotType = false;
    for (uint32_t i = 0; i < 32; i++) if (aprops.memoryTypeBits & (1u << i)) { memType = i; gotType = true; break; }
    if (!gotType) FAIL("vk-memtype", "no memory type in AHB memoryTypeBits");

    VkImportAndroidHardwareBufferInfoANDROID importInfo{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    importInfo.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image; dedicated.pNext = &importInfo;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &dedicated;
    mai.allocationSize = aprops.allocationSize;
    mai.memoryTypeIndex = memType;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VKCHECK("vk-import-mem", vk.allocMem(dev, &mai, nullptr, &mem));
    VKCHECK("vk-bind", vk.bindImage(dev, image, mem, 0));
    LOGI("Vulkan imported AHB as VkImage + bound memory");

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = qfi;
    VkCommandPool pool; VKCHECK("vk-cmdpool", vk.createCmdPool(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd; VKCHECK("vk-cmdalloc", vk.allocCmd(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK("vk-begin", vk.beginCmd(cmd, &bbi));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image; toDst.subresourceRange = range;
    vk.cmdBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
    VkClearColorValue clear{}; clear.float32[0]=kClearR; clear.float32[1]=kClearG; clear.float32[2]=kClearB; clear.float32[3]=kClearA;
    vk.cmdClear(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    // TRANSFER_DST -> GENERAL + release to FOREIGN queue (external consumer)
    VkImageMemoryBarrier toGen{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toGen.dstAccessMask = 0;
    toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.srcQueueFamilyIndex = qfi; toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    toGen.image = image; toGen.subresourceRange = range;
    vk.cmdBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGen);
    VKCHECK("vk-end", vk.endCmd(cmd));

    // Fence with sync-fd export if available.
    VkExportFenceCreateInfo efci{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
    efci.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkFenceOk) fci.pNext = &efci;
    VkFence fence; VKCHECK("vk-fence", vk.createFence(dev, &fci, nullptr, &fence));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VKCHECK("vk-submit", vk.queueSubmit(queue, 1, &si, fence));
    LOGI("Vulkan cleared AHB to (%.2f,%.2f,%.2f,%.2f), submitted", kClearR, kClearG, kClearB, kClearA);

    int syncFd = -1;
    if (vkFenceOk) {
        VkFenceGetFdInfoKHR gfd{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
        gfd.fence = fence; gfd.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        VkResult r = vk.getFenceFd(dev, &gfd, &syncFd);
        LOGI("vkGetFenceFdKHR -> fd=%d (VkResult %d)", syncFd, (int)r);
    }
    if (syncFd < 0) {
        // No exportable fence — fall back to a CPU wait so the comparison is still
        // valid (just not a cross-driver GPU fence). Records that fences are the gap.
        LOGI("no sync-fd; CPU vkWaitForFences fallback (cross-driver GPU fence UNAVAILABLE)");
        vk.waitFences(dev, 1, &fence, VK_TRUE, 2000000000ull);
    }

    // ====================================================================
    // STAGE 5 — EGL import the AHB, wait the fence, sample, read back
    // ====================================================================
    EGLClientBuffer clientBuf = eglGetNativeClientBufferANDROID_(ahb);
    if (!clientBuf) FAIL("egl-clientbuf", "eglGetNativeClientBufferANDROID returned NULL");
    const EGLint imgAttr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR eglImg = eglCreateImageKHR_(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuf, imgAttr);
    if (eglImg == EGL_NO_IMAGE_KHR) FAIL("egl-create-image", "eglCreateImageKHR(NATIVE_BUFFER_ANDROID) err=0x%x", eglGetError());
    LOGI("Adreno EGL imported AHB as EGLImage");

    bool fenceWaited = false;
    if (syncFd >= 0 && eglFenceOk && eglCreateSyncKHR_ && eglClientWaitSyncKHR_) {
        EGLint syncAttr[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, syncFd, EGL_NONE };
        EGLSyncKHR sync = eglCreateSyncKHR_(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, syncAttr);
        if (sync != EGL_NO_SYNC_KHR) {
            // eglCreateSync takes ownership of the fd on success.
            EGLint w = eglClientWaitSyncKHR_(dpy, sync, 0, 2000000000ull);
            fenceWaited = (w == EGL_CONDITION_SATISFIED_KHR);
            LOGI("EGL cross-driver fence wait -> %s (0x%x)", fenceWaited ? "SATISFIED" : "FAILED", w);
            if (eglDestroySyncKHR_) eglDestroySyncKHR_(dpy, sync);
        } else {
            LOGE("eglCreateSyncKHR(NATIVE_FENCE) failed err=0x%x; closing fd", eglGetError());
            close(syncFd);
        }
    }

    GLuint tex = 0; glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, (GLeglImageOES)eglImg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum gerr = glGetError();
    if (gerr != GL_NO_ERROR) FAIL("gl-eglimage-target", "glEGLImageTargetTexture2DOES glError=0x%x", gerr);

    // Read the AHB content by attaching its texture as an FBO colour attachment.
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbs != GL_FRAMEBUFFER_COMPLETE) FAIL("gl-fbo", "FBO incomplete 0x%x (AHB not colour-renderable?)", fbs);
    unsigned char px[4] = {0,0,0,0};
    glReadPixels(kW/2, kH/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gerr = glGetError();
    LOGI("readback center pixel = (%d,%d,%d,%d)  expected ~(%d,%d,%d,%d)  glError=0x%x",
         px[0],px[1],px[2],px[3], kExpR,kExpG,kExpB,kExpA, gerr);

    auto near = [](int a, int b){ int d = a-b; return d>=-3 && d<=3; };
    bool pixelOk = near(px[0],kExpR) && near(px[1],kExpG) && near(px[2],kExpB) && near(px[3],kExpA);

    // ====================================================================
    // VERDICT
    // ====================================================================
    LOGI("SUMMARY: vkAHB=%d vkFence=%d eglImg=%d eglFence=%d fenceWaited=%d pixelMatch=%d",
         vkAhbOk, vkFenceOk, eglImgOk, eglFenceOk, fenceWaited, pixelOk);
    if (!pixelOk) FAIL("pixel-verify", "AHB sample mismatch — Turnip write not visible to Adreno EGL");
    if (vkFenceOk && eglFenceOk && !fenceWaited)
        LOGE("WARN: pixel matched but the cross-driver fence wait did NOT report SATISFIED "
             "(timing-lucky; sync-fd path needs attention before production)");

    LOGI("=== AHB SPIKE RESULT: PASS%s ===", (vkFenceOk && eglFenceOk && fenceWaited)
                                              ? " (buffer + cross-driver fence both OK)"
                                              : " (buffer OK; fence path WEAK — see WARN/log)");
    // Intentionally leak GL/VK/AHB objects: throwaway one-shot diagnostic.
    return true;
}
