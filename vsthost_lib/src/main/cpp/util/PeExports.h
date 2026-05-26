// PE32/PE32+ export-directory parser.
//
// Built so the Android side can probe a freshly-installed .dll for the
// VST2 VSTPluginMain entry point WITHOUT actually loading it through
// wine — discovery after an installer run scans potentially dozens of
// candidate DLLs (Common Files\VST, Program Files\Vstplugins, …) and a
// full LoadLibrary per candidate would mean firing up wine + FEX-Emu
// for each, taking minutes. Reading the export table from the PE bytes
// is microseconds per file.

#pragma once

#include <string>

namespace vstpoc::pe {

/** Result of inspecting a candidate PE file. */
struct ExportInfo {
    bool valid = false;             // file is a parseable PE32/PE32+
    bool isDll = false;             // PE characteristic IMAGE_FILE_DLL
    bool is64Bit = false;           // PE32+ vs PE32
    bool hasVstPluginMain = false;  // exports "VSTPluginMain" or "main"
    bool hasVst3Factory = false;    // exports "GetPluginFactory" (VST3 — not supported)
};

/** mmap the file at `path` (read-only), walk its PE optional-header
 *  export directory, and report what's there. Safe to call on any
 *  file; returns valid=false for non-PE inputs without crashing. */
ExportInfo inspect(const std::string& path);

}  // namespace vstpoc::pe
