// Minimal VST 2.4 AEffect declarations.
//
// The official Steinberg VST2 SDK was withdrawn in 2022. The struct layout
// and opcode constants below are the de-facto interface shared by every
// open host (LinVst, vestige, JUCE's stub, etc.). All function pointers are
// Microsoft x64 ABI — plugins built with mingw-w64 use this convention,
// and we annotate every call site so GCC marshals args correctly when the
// caller is System V (e.g. our host running on Linux x86_64 under Box64).
//
// Used by:
//   - external/vst2/test_plugin.c   (mingw-w64 PE32+ DLL)
//   - external/vst_guest/vst_guest.c (no-libc x86_64 Linux ELF host)

#ifndef VSTPOC_VST2_H
#define VSTPOC_VST2_H

#include <stdint.h>

#define kEffectMagic 0x56737450  /* 'VstP' */

typedef struct AEffect AEffect;

#ifndef VST_CALL
#define VST_CALL __attribute__((ms_abi))
#endif

typedef intptr_t (VST_CALL *AEffectDispatcherProc)(
    AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef void (VST_CALL *AEffectProcessProc)(
    AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames);
typedef void (VST_CALL *AEffectSetParameterProc)(
    AEffect* effect, int32_t index, float parameter);
typedef float (VST_CALL *AEffectGetParameterProc)(
    AEffect* effect, int32_t index);
typedef intptr_t (VST_CALL *AudioMasterCallback)(
    AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);

struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;            /* deprecated; use processReplacing */
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float   ioRatio;
    void*   object;
    void*   user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessProc processDoubleReplacing;
    char    future[56];                    /* padding to ~200 bytes */
};

/* Editor rect (filled by the plugin in response to effEditGetRect; the
 * plugin owns the storage and returns a pointer-to-pointer via the `ptr`
 * argument of dispatcher). */
typedef struct {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
} ERect;

/* effFlags bits */
#define effFlagsHasEditor       (1 << 0)
#define effFlagsCanReplacing    (1 << 4)
#define effFlagsProgramChunks   (1 << 5)
#define effFlagsIsSynth         (1 << 8)
#define effFlagsNoSoundInStop   (1 << 9)

/* Plugin-side opcodes (effXxx) */
#define effOpen               0
#define effClose              1
#define effSetProgram         2
#define effGetProgram         3
#define effGetProgramName     5
#define effGetParamLabel      6
#define effGetParamDisplay    7
#define effGetParamName       8
#define effSetSampleRate     10
#define effSetBlockSize      11
#define effMainsChanged      12
#define effEditGetRect       13
#define effEditOpen          14
#define effEditClose         15
#define effEditIdle          19
#define effGetChunk          23
#define effSetChunk          24
#define effProcessEvents     25
#define effGetEffectName     45
#define effGetVendorString   47
#define effGetProductString  48
#define effGetVendorVersion  49
#define effCanDo             51
#define effStartProcess      71
#define effStopProcess       72

/* Host-side opcodes (audioMasterXxx) */
#define audioMasterAutomate           0
#define audioMasterVersion            1
#define audioMasterCurrentId          2
#define audioMasterIdle               3
#define audioMasterPinConnected       4
#define audioMasterWantMidi           6
#define audioMasterGetTime            7
#define audioMasterProcessEvents      8
#define audioMasterIOChanged         13
#define audioMasterSizeWindow        15
#define audioMasterGetSampleRate     16
#define audioMasterGetBlockSize      17
#define audioMasterGetInputLatency   18
#define audioMasterGetOutputLatency  19
#define audioMasterGetCurrentProcessLevel 23
#define audioMasterGetAutomationState 24
#define audioMasterGetVendorString   32
#define audioMasterGetProductString  33
#define audioMasterGetVendorVersion  34
#define audioMasterCanDo             37
#define audioMasterGetLanguage       38

#endif /* VSTPOC_VST2_H */
