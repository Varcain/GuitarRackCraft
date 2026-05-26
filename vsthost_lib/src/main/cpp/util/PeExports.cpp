#include "PeExports.h"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vstpoc::pe {

namespace {

// PE constants. Avoid pulling in <windows.h> on the Android NDK side —
// the wire format is fully documented and stable, copy just what we need.
constexpr uint16_t kDosMagic   = 0x5A4D;  // 'MZ'
constexpr uint32_t kPeMagic    = 0x00004550;  // 'PE\0\0'
constexpr uint16_t kOptMagic32 = 0x010B;
constexpr uint16_t kOptMagic64 = 0x020B;
constexpr uint16_t kCharFileDll = 0x2000;
constexpr int kDirEntryExport  = 0;       // IMAGE_DIRECTORY_ENTRY_EXPORT

// Section header (40 bytes) — only the bits we need for RVA→file
// offset translation.
struct Section {
    uint32_t virtualAddress;
    uint32_t sizeOfRawData;
    uint32_t pointerToRawData;
};

// IMAGE_EXPORT_DIRECTORY layout (selected fields).
struct ExportDir {
    uint32_t exportFlags;
    uint32_t timeDateStamp;
    uint16_t majorVersion;
    uint16_t minorVersion;
    uint32_t nameRva;
    uint32_t ordinalBase;
    uint32_t addressTableEntries;
    uint32_t numberOfNamePointers;
    uint32_t exportAddressTableRva;
    uint32_t namePointerRva;
    uint32_t ordinalTableRva;
};

template <typename T>
bool readAt(const uint8_t* base, size_t size, size_t off, T& out) {
    if (off > size || size - off < sizeof(T)) return false;
    std::memcpy(&out, base + off, sizeof(T));
    return true;
}

// Translate an RVA (relative virtual address, used inside the in-memory
// image) to an offset inside the on-disk file. Walks the section table.
bool rvaToFileOffset(const uint8_t* base, size_t size,
                     uint32_t sectionTableOff, uint16_t numSections,
                     uint32_t rva, size_t& out) {
    for (uint16_t i = 0; i < numSections; ++i) {
        size_t off = sectionTableOff + size_t(i) * 40;
        Section s{};
        // VirtualAddress at +12, SizeOfRawData at +16, PointerToRawData at +20.
        if (!readAt(base, size, off + 12, s.virtualAddress)) return false;
        if (!readAt(base, size, off + 16, s.sizeOfRawData)) return false;
        if (!readAt(base, size, off + 20, s.pointerToRawData)) return false;
        if (rva >= s.virtualAddress && rva < s.virtualAddress + s.sizeOfRawData) {
            out = s.pointerToRawData + (rva - s.virtualAddress);
            return true;
        }
    }
    return false;
}

bool readNameAt(const uint8_t* base, size_t size, size_t off,
                std::string& out, size_t cap = 96) {
    if (off >= size) return false;
    size_t end = off;
    size_t limit = std::min(size, off + cap);
    while (end < limit && base[end] != 0) ++end;
    if (end >= size) return false;
    out.assign(reinterpret_cast<const char*>(base + off), end - off);
    return true;
}

}  // namespace

ExportInfo inspect(const std::string& path) {
    ExportInfo info{};
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return info;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 64) {
        ::close(fd);
        return info;
    }
    size_t size = static_cast<size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) return info;
    const uint8_t* base = static_cast<const uint8_t*>(mapped);

    auto cleanup = [&]() { ::munmap(mapped, size); };

    // DOS header check.
    uint16_t dosMagic = 0;
    if (!readAt(base, size, 0, dosMagic) || dosMagic != kDosMagic) {
        cleanup();
        return info;
    }
    uint32_t peOff = 0;
    if (!readAt(base, size, 0x3c, peOff) || peOff + 24 > size) {
        cleanup();
        return info;
    }

    // PE signature + COFF header.
    uint32_t peSig = 0;
    if (!readAt(base, size, peOff, peSig) || peSig != kPeMagic) {
        cleanup();
        return info;
    }
    uint16_t machine = 0, numSections = 0, optHeaderSize = 0, characteristics = 0;
    readAt(base, size, peOff + 4, machine);
    readAt(base, size, peOff + 6, numSections);
    readAt(base, size, peOff + 20, optHeaderSize);
    readAt(base, size, peOff + 22, characteristics);
    (void)machine;
    info.isDll = (characteristics & kCharFileDll) != 0;

    // Optional header.
    size_t optOff = peOff + 24;
    uint16_t optMagic = 0;
    if (!readAt(base, size, optOff, optMagic)) { cleanup(); return info; }
    info.is64Bit = (optMagic == kOptMagic64);
    if (optMagic != kOptMagic32 && optMagic != kOptMagic64) {
        cleanup();
        return info;
    }
    info.valid = true;

    // Data directories start at different offsets in PE32 vs PE32+:
    //   PE32: optOff + 96
    //   PE32+: optOff + 112
    // Each entry is 8 bytes (rva + size). Export is index 0.
    size_t dataDirOff = optOff + (info.is64Bit ? 112 : 96);
    uint32_t exportRva = 0, exportSize = 0;
    if (!readAt(base, size, dataDirOff + 0, exportRva) ||
        !readAt(base, size, dataDirOff + 4, exportSize)) {
        cleanup();
        return info;
    }
    if (exportRva == 0 || exportSize == 0) {
        // No export directory — not necessarily an error (e.g. an EXE).
        cleanup();
        return info;
    }

    // Section table follows the optional header.
    uint32_t sectionTableOff = optOff + optHeaderSize;

    // Map export-directory RVA to file offset, then read its name-pointer
    // table and walk strings.
    size_t expDirOff = 0;
    if (!rvaToFileOffset(base, size, sectionTableOff, numSections,
                         exportRva, expDirOff)) {
        cleanup();
        return info;
    }
    if (expDirOff + sizeof(ExportDir) > size) { cleanup(); return info; }

    ExportDir ed{};
    std::memcpy(&ed, base + expDirOff, sizeof(ed));

    size_t namePtrOff = 0;
    if (!rvaToFileOffset(base, size, sectionTableOff, numSections,
                         ed.namePointerRva, namePtrOff)) {
        cleanup();
        return info;
    }
    if (namePtrOff + size_t(ed.numberOfNamePointers) * 4 > size) {
        cleanup();
        return info;
    }

    // Iterate name pointers and check each against our targets. Use a
    // sane cap on names checked (some real plugins export thousands of
    // VST3 helpers — keep this bounded).
    const uint32_t cap = std::min<uint32_t>(ed.numberOfNamePointers, 8192u);
    for (uint32_t i = 0; i < cap; ++i) {
        uint32_t nameRva = 0;
        if (!readAt(base, size, namePtrOff + size_t(i) * 4, nameRva)) break;
        size_t nameOff = 0;
        if (!rvaToFileOffset(base, size, sectionTableOff, numSections,
                             nameRva, nameOff)) {
            continue;
        }
        std::string name;
        if (!readNameAt(base, size, nameOff, name)) continue;
        if (name == "VSTPluginMain" || name == "main") {
            info.hasVstPluginMain = true;
        } else if (name == "GetPluginFactory") {
            info.hasVst3Factory = true;
        }
        if (info.hasVstPluginMain && info.hasVst3Factory) break;
    }

    cleanup();
    return info;
}

}  // namespace vstpoc::pe
