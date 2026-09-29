/* Desktop stub for <android/hardware_buffer.h>
 * Only what the X server's AHB side channel / GPU-present code references.
 * The stubs never produce a buffer (allocate/recv fail), so the AHB paths
 * stay inert in the harness. */
#ifndef XTEST_STUB_ANDROID_HARDWARE_BUFFER_H
#define XTEST_STUB_ANDROID_HARDWARE_BUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

typedef struct ARect {
    int32_t left, top, right, bottom;
} ARect;

#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM   1
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN   (3ULL << 4)
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)

int  AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
void AHardwareBuffer_acquire(AHardwareBuffer* buffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
int  AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                          const ARect* rect, void** outVirtualAddress);
int  AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence);
int  AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer* buffer, int socketFd);
int  AHardwareBuffer_recvHandleFromUnixSocket(int socketFd, AHardwareBuffer** outBuffer);

#ifdef __cplusplus
}
#endif

#endif /* XTEST_STUB_ANDROID_HARDWARE_BUFFER_H */
