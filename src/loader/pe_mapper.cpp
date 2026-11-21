#include "scarlettx/loader/pe_mapper.h"

#include "guest_modules.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

#if defined(_WIN32)
// Host-process registrations made for a mapped image; undone in Unmap().
struct LoaderState {
    guest::TlsBinding tls;
    PRUNTIME_FUNCTION function_table = nullptr;
    uint8_t* registered_base = nullptr;
};
#endif

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string MakeNamedKey(const std::string& dll, const std::string& sym) {
    return ToLowerAscii(dll) + "!" + sym;
}

std::string MakeOrdinalKey(const std::string& dll, uint16_t ordinal) {
    return ToLowerAscii(dll) + "!#" + std::to_string(ordinal);
}

#if defined(_WIN32)
using GuestAtExitFn = void(__cdecl*)();

std::mutex& GuestAtExitMutex() {
    static std::mutex m;
    return m;
}

std::vector<GuestAtExitFn>& GuestAtExitTable() {
    static std::vector<GuestAtExitFn> table;
    return table;
}

int __cdecl GuestIsolatedAtExit(GuestAtExitFn fn) {
    if (!fn) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(GuestAtExitMutex());
    GuestAtExitTable().push_back(fn);
    return 0;
}

void RunAndClearGuestAtExitInRange(const uint8_t* base, size_t size) {
    if (!base || size == 0) {
        return;
    }
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base);
    const uintptr_t hi = lo + size;
    std::vector<GuestAtExitFn> to_run;
    {
        std::lock_guard<std::mutex> lock(GuestAtExitMutex());
        auto& table = GuestAtExitTable();
        for (auto it = table.rbegin(); it != table.rend();) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(*it);
            if (addr >= lo && addr < hi) {
                to_run.push_back(*it);
                it = std::vector<GuestAtExitFn>::reverse_iterator(table.erase(std::next(it).base()));
            } else {
                ++it;
            }
        }
    }
    for (GuestAtExitFn fn : to_run) {
        fn();
    }
}

std::string& GuestShimDirectory() {
    static std::string dir;
    return dir;
}

bool IsHleShimFilename(const std::string& lower_name) {
    static const char* const kShims[] = {
        "xgameruntime.dll",
        "d3d12_xs.dll",
        "xg_xs.dll",
        "acphal.dll",
        "gameinput.dll",
        "xaudio2_9.dll",
        "dstorage_xs.dll",
        "bink2scarlett.dll",
        "amd_fidelityfx_loader_xbox.dll",
        "amd_fidelityfx_upscaler_xbox.dll",
        "libhttpclient.gdk.dll",
        "xcurl.dll",
    };
    for (const char* s : kShims) {
        if (lower_name == s) return true;
    }
    return false;
}

std::string ExtractBasenameLower(const std::string& path_str) {
    const size_t pos = path_str.find_last_of("\\/");
    const std::string base = (pos == std::string::npos) ? path_str : path_str.substr(pos + 1);
    return ToLowerAscii(base);
}

HMODULE TryLoadGuestShimA(const char* name) {
    if (!name || !*name) return nullptr;
    const std::string& shim_dir = GuestShimDirectory();
    if (shim_dir.empty()) return nullptr;
    const std::string base = ExtractBasenameLower(name);
    if (!IsHleShimFilename(base)) return nullptr;
    const auto candidate = std::filesystem::path(shim_dir) / base;
    if (std::filesystem::exists(candidate)) {
        HMODULE mod = LoadLibraryA(candidate.string().c_str());
        const DWORD error = mod ? 0 : GetLastError();
        if (mod) {
            std::fprintf(stderr, "[ScarlettX][loader] Redirected dynamic load '%s' -> '%s' (handle=%p)\n",
                         name, candidate.string().c_str(), static_cast<void*>(mod));
        } else {
            std::fprintf(stderr, "[ScarlettX][loader] Redirected dynamic load '%s' -> '%s' FAILED (error %lu)\n",
                         name, candidate.string().c_str(), error);
            SetLastError(error);
        }
        return mod;
    }
    return nullptr;
}

// A title DLL loaded by the host loader still has to see the guest image when
// it asks which module it runs in.
HMODULE AdoptHostModule(HMODULE mod) {
    if (mod) {
        const DWORD saved_error = GetLastError();
        guest::PatchHostModuleForGuests(mod);
        SetLastError(saved_error);
    }
    return mod;
}

HMODULE WINAPI GuestLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (HMODULE mod = TryLoadGuestShimA(lpLibFileName)) {
        return mod;
    }
    return AdoptHostModule(LoadLibraryExA(lpLibFileName, hFile, dwFlags));
}

HMODULE WINAPI GuestLoadLibraryA(LPCSTR lpLibFileName) {
    if (HMODULE mod = TryLoadGuestShimA(lpLibFileName)) {
        return mod;
    }
    return AdoptHostModule(LoadLibraryA(lpLibFileName));
}

HMODULE WINAPI GuestLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName && *lpLibFileName) {
        std::string narrow;
        for (const wchar_t* p = lpLibFileName; *p; ++p) {
            narrow.push_back(*p < 128 ? static_cast<char>(*p) : '?');
        }
        if (HMODULE mod = TryLoadGuestShimA(narrow.c_str())) {
            return mod;
        }
    }
    return AdoptHostModule(LoadLibraryExW(lpLibFileName, hFile, dwFlags));
}

HMODULE WINAPI GuestLoadLibraryW(LPCWSTR lpLibFileName) {
    return GuestLoadLibraryExW(lpLibFileName, nullptr, 0);
}

DWORD SectionCharsToPageProtect(uint32_t ch) {
    const bool exec = (ch & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool read = (ch & IMAGE_SCN_MEM_READ) != 0;
    const bool write = (ch & IMAGE_SCN_MEM_WRITE) != 0;
    if (exec) {
        if (write) return PAGE_EXECUTE_READWRITE;
        if (read) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (write) return PAGE_READWRITE;
    if (read) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
#endif

}  // namespace

std::string ResolveApiSetDll(const std::string& dll_name) {
    const std::string lower = ToLowerAscii(dll_name);
    if (StartsWith(lower, "api-ms-win-crt-")) {
        return "ucrtbase.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-com-")) {
        return "combase.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-winrt-")) {
        return "combase.dll";
    }
    if (StartsWith(lower, "api-ms-win-security-")) {
        return "advapi32.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-registry-")) {
        return "advapi32.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-psm-appnotify-")) {
        return "twinapi.appcore.dll";
    }
    if (StartsWith(lower, "ext-ms-win-rtcore-ntuser-")) {
        return "user32.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-rtlsupport-")) {
        return "ntdll.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-synch-l1-2-")) {
        return "kernelbase.dll";
    }
    if (StartsWith(lower, "api-ms-win-core-") || StartsWith(lower, "ext-ms-win-core-")) {
        return "kernel32.dll";
    }
    return dll_name;
}

void HleSymbolRegistry::RegisterSymbol(const std::string& dll_name,
                                       const std::string& symbol_name,
                                       void* address) {
    named_symbols_[MakeNamedKey(dll_name, symbol_name)] = address;
}

void HleSymbolRegistry::RegisterOrdinal(const std::string& dll_name,
                                        uint16_t ordinal,
                                        void* address) {
    ordinal_symbols_[MakeOrdinalKey(dll_name, ordinal)] = address;
}

void* HleSymbolRegistry::LookupSymbol(const std::string& dll_name,
                                      const std::string& symbol_name) const {
    auto it = named_symbols_.find(MakeNamedKey(dll_name, symbol_name));
    return it == named_symbols_.end() ? nullptr : it->second;
}

void* HleSymbolRegistry::LookupOrdinal(const std::string& dll_name, uint16_t ordinal) const {
    auto it = ordinal_symbols_.find(MakeOrdinalKey(dll_name, ordinal));
    return it == ordinal_symbols_.end() ? nullptr : it->second;
}

MappedPeImage::~MappedPeImage() {
    Unmap();
}

void MappedPeImage::Unmap() {
#if defined(_WIN32)
    if (mapped_base_ && size_of_image_ > 0) {
        RunAndClearGuestAtExitInRange(mapped_base_, size_of_image_);
    }
    if (mapped_base_ && is_dll_ && dll_attached_ && entry_point_rva_ != 0) {
        using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto fn = reinterpret_cast<DllMainFn>(mapped_base_ + entry_point_rva_);
        fn(reinterpret_cast<HINSTANCE>(mapped_base_), DLL_PROCESS_DETACH, nullptr);
        dll_attached_ = false;
    }
    if (auto* state = static_cast<LoaderState*>(loader_state_)) {
        if (state->registered_base) {
            guest::UnregisterModule(state->registered_base);
        }
        if (state->function_table) {
            RtlDeleteFunctionTable(state->function_table);
        }
        guest::DetachTls(&state->tls);
        delete state;
        loader_state_ = nullptr;
    }
    for (void* mod : loaded_modules_) {
        if (mod) {
            FreeLibrary(static_cast<HMODULE>(mod));
        }
    }
    loaded_modules_.clear();
    if (mapped_base_) {
        VirtualFree(mapped_base_, 0, MEM_RELEASE);
        mapped_base_ = nullptr;
    }
#endif
    preferred_image_base_ = 0;
    size_of_image_ = 0;
    entry_point_rva_ = 0;
    export_dir_rva_ = 0;
    export_dir_size_ = 0;
    relocations_applied_ = 0;
    is_dll_ = false;
    dll_attached_ = false;
    resolutions_.clear();
}

Status MappedPeImage::MapFile(const std::string& pe_path,
                              const DllRedirectionTable& redirects,
                              const HleSymbolRegistry& hle_registry,
                              const PeMapOptions& options) {
    Unmap();
#if !defined(_WIN32)
    (void)pe_path;
    (void)redirects;
    (void)hle_registry;
    (void)options;
    return Status::Error(StatusCode::NotImplemented,
                         "In-process PE64 mapping currently requires Windows x64");
#else
    std::ifstream ifs(pe_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open PE64 file: " + pe_path);
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) {
        return Status::Error(StatusCode::ParseError, "PE64 file smaller than DOS header");
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid DOS signature");
    }
    if (dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size()) {
        return Status::Error(StatusCode::ParseError, "Truncated NT64 headers");
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid PE signature");
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Expected x86-64 PE32+ image for Scarlett");
    }
    const size_t section_table = static_cast<size_t>(dos->e_lfanew) +
                                 offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                 nt->FileHeader.SizeOfOptionalHeader;
    if (section_table + nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) >
        raw.size()) {
        return Status::Error(StatusCode::ParseError, "Truncated section table");
    }

    preferred_image_base_ = nt->OptionalHeader.ImageBase;
    size_of_image_ = nt->OptionalHeader.SizeOfImage;
    entry_point_rva_ = nt->OptionalHeader.AddressOfEntryPoint;
    is_dll_ = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        const auto& exp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        export_dir_rva_ = exp.VirtualAddress;
        export_dir_size_ = exp.Size;
    }

    // 1. Allocate image memory. When force_relocate is set, reserve a dummy page
    //    at preferred_image_base_ only if needed, or simply pass nullptr to
    //    VirtualAlloc (and retry once if it happened to land on preferred base).
    void* base = nullptr;
    void* blocker = nullptr;
    if (!options.force_relocate) {
        base = VirtualAlloc(reinterpret_cast<LPVOID>(preferred_image_base_), size_of_image_,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!base) {
        base = VirtualAlloc(nullptr, size_of_image_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (options.force_relocate &&
            reinterpret_cast<uint64_t>(base) == preferred_image_base_) {
            blocker = base;
            base = VirtualAlloc(nullptr, size_of_image_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            VirtualFree(blocker, 0, MEM_RELEASE);
        }
    }
    if (!base) {
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc failed for guest PE64 image");
    }
    mapped_base_ = static_cast<uint8_t*>(base);
    if (WasRelocated() && (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0) {
        std::ostringstream msg;
        msg << "Image with stripped relocations cannot be mapped away from its preferred base 0x"
            << std::hex << preferred_image_base_;
        Unmap();
        return Status::Error(StatusCode::UnsupportedFormat, msg.str());
    }

    // 2. Copy headers and sections.
    const size_t headers_copy =
        std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, std::min<size_t>(raw.size(), size_of_image_));
    std::memcpy(mapped_base_, raw.data(), headers_copy);

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        if (sec.SizeOfRawData == 0) {
            continue;
        }
        if (static_cast<size_t>(sec.PointerToRawData) + sec.SizeOfRawData > raw.size() ||
            static_cast<size_t>(sec.VirtualAddress) + sec.SizeOfRawData > size_of_image_) {
            Unmap();
            return Status::Error(StatusCode::ParseError, "Section bounds exceed PE image size");
        }
        std::memcpy(mapped_base_ + sec.VirtualAddress,
                    raw.data() + sec.PointerToRawData,
                    sec.SizeOfRawData);
    }

    // 3. Apply base relocations if mapped away from preferred ImageBase.
    const int64_t delta = static_cast<int64_t>(MappedBase() - preferred_image_base_);
    if (delta != 0 && nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        const auto& reloc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_dir.VirtualAddress != 0 && reloc_dir.Size > 0) {
            if (static_cast<size_t>(reloc_dir.VirtualAddress) + reloc_dir.Size > size_of_image_) {
                Unmap();
                return Status::Error(StatusCode::ParseError,
                                     "Relocation directory exceeds PE image size");
            }
            size_t offset = 0;
            while (offset + sizeof(IMAGE_BASE_RELOCATION) <= reloc_dir.Size) {
                const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                    mapped_base_ + reloc_dir.VirtualAddress + offset);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
                    break;
                }
                if (block->SizeOfBlock > reloc_dir.Size - offset) {
                    Unmap();
                    return Status::Error(StatusCode::ParseError,
                                         "Relocation block exceeds relocation directory");
                }
                const size_t entry_count =
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                const auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
                for (size_t e = 0; e < entry_count; ++e) {
                    const uint16_t type = entries[e] >> 12;
                    const uint16_t rel_off = entries[e] & 0x0FFFu;
                    const size_t target_rva = static_cast<size_t>(block->VirtualAddress) + rel_off;
                    if (type == IMAGE_REL_BASED_ABSOLUTE) {
                        continue;
                    } else if (type == IMAGE_REL_BASED_DIR64) {
                        if (target_rva + sizeof(uint64_t) <= size_of_image_) {
                            uint64_t val = 0;
                            std::memcpy(&val, mapped_base_ + target_rva, sizeof(val));
                            val = static_cast<uint64_t>(static_cast<int64_t>(val) + delta);
                            std::memcpy(mapped_base_ + target_rva, &val, sizeof(val));
                            ++relocations_applied_;
                        }
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        if (target_rva + sizeof(uint32_t) <= size_of_image_) {
                            uint32_t val = 0;
                            std::memcpy(&val, mapped_base_ + target_rva, sizeof(val));
                            val = static_cast<uint32_t>(static_cast<int64_t>(val) + delta);
                            std::memcpy(mapped_base_ + target_rva, &val, sizeof(val));
                            ++relocations_applied_;
                        }
                    }
                }
                offset += block->SizeOfBlock;
            }
        }
    }

    // 4. Resolve Import Address Table (IAT):
    //    Order: HleSymbolRegistry -> redirected/API-set host DLL -> x86-64 StubThunkTable.
    if (!options.shim_directory.empty()) {
        GuestShimDirectory() = options.shim_directory;
    }
    stubs_.SetVerboseLogging(options.verbose_stub_logging);
    std::unordered_map<std::string, HMODULE> module_cache;

    auto load_target_module = [&](const std::string& target_dll, bool is_hle) -> HMODULE {
        const std::string key = ToLowerAscii(target_dll);
        auto it = module_cache.find(key);
        if (it != module_cache.end()) {
            return it->second;
        }
        HMODULE mod = nullptr;
        if (is_hle && !options.shim_directory.empty()) {
            const auto candidate = std::filesystem::path(options.shim_directory) / target_dll;
            if (std::filesystem::exists(candidate)) {
                mod = LoadLibraryA(candidate.string().c_str());
            }
        }
        if (!mod) {
            mod = LoadLibraryA(target_dll.c_str());
        }
        if (mod) {
            loaded_modules_.push_back(mod);
        }
        module_cache[key] = mod;
        return mod;
    };

    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        const auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imp_dir.VirtualAddress != 0) {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                mapped_base_ + imp_dir.VirtualAddress);
            for (; desc->Name != 0; ++desc) {
                const std::string guest_dll(reinterpret_cast<const char*>(mapped_base_ + desc->Name));
                std::string target_dll;
                bool is_hle = false;
                if (!redirects.Resolve(guest_dll, target_dll, is_hle)) {
                    target_dll = ResolveApiSetDll(guest_dll);
                    is_hle = false;
                }

                const auto* orig_thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                    mapped_base_ +
                    (desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk));
                auto* iat_thunk =
                    reinterpret_cast<IMAGE_THUNK_DATA64*>(mapped_base_ + desc->FirstThunk);

                for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++iat_thunk) {
                    const bool by_ordinal = IMAGE_SNAP_BY_ORDINAL64(orig_thunk->u1.Ordinal) != 0;
                    uint16_t ordinal = 0;
                    std::string sym_name;
                    if (by_ordinal) {
                        ordinal = static_cast<uint16_t>(IMAGE_ORDINAL64(orig_thunk->u1.Ordinal));
                    } else {
                        const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                            mapped_base_ + orig_thunk->u1.AddressOfData);
                        sym_name = reinterpret_cast<const char*>(ibn->Name);
                    }

                    PeImportResolution res;
                    res.dll_name = guest_dll;
                    res.target_dll = target_dll;
                    res.symbol_name = sym_name;
                    res.ordinal = ordinal;
                    res.by_ordinal = by_ordinal;

                    // (a) Check explicit in-process HLE symbol registry first.
                    void* resolved = by_ordinal ? hle_registry.LookupOrdinal(guest_dll, ordinal)
                                                : hle_registry.LookupSymbol(guest_dll, sym_name);
                    if (!resolved && ToLowerAscii(target_dll) != ToLowerAscii(guest_dll)) {
                        resolved = by_ordinal ? hle_registry.LookupOrdinal(target_dll, ordinal)
                                              : hle_registry.LookupSymbol(target_dll, sym_name);
                    }
                    if (resolved) {
                        res.kind = ImportResolutionKind::HleRegistry;
                    }

                    // (a.5) Isolate guest CRT atexit registrations and intercept dynamic
                    // LoadLibrary* calls so LOAD_LIBRARY_SEARCH_SYSTEM32 for xgameruntime.dll
                    // resolves to ScarlettX's shim directory.
                    if (!resolved && !by_ordinal) {
                        const std::string target_lower = ToLowerAscii(target_dll);
                        if ((target_lower == "ucrtbase.dll" || target_lower == "msvcrt.dll") &&
                            (sym_name == "_crt_atexit" || sym_name == "atexit" ||
                             sym_name == "_crt_at_quick_exit")) {
                            resolved = reinterpret_cast<void*>(&GuestIsolatedAtExit);
                            res.kind = ImportResolutionKind::HleRegistry;
                        } else if (!options.shim_directory.empty() &&
                                   (target_lower == "kernel32.dll" ||
                                    target_lower == "kernelbase.dll")) {
                            if (sym_name == "LoadLibraryExA") {
                                resolved = reinterpret_cast<void*>(&GuestLoadLibraryExA);
                                res.kind = ImportResolutionKind::HleRegistry;
                            } else if (sym_name == "LoadLibraryA") {
                                resolved = reinterpret_cast<void*>(&GuestLoadLibraryA);
                                res.kind = ImportResolutionKind::HleRegistry;
                            } else if (sym_name == "LoadLibraryExW") {
                                resolved = reinterpret_cast<void*>(&GuestLoadLibraryExW);
                                res.kind = ImportResolutionKind::HleRegistry;
                            } else if (sym_name == "LoadLibraryW") {
                                resolved = reinterpret_cast<void*>(&GuestLoadLibraryW);
                                res.kind = ImportResolutionKind::HleRegistry;
                            }
                        }
                    }

                    // (a.6) Module queries the OS loader cannot answer for a mapped image.
                    if (!resolved && !by_ordinal) {
                        if (void* replacement =
                                guest::LookupImportOverride(ToLowerAscii(target_dll), sym_name)) {
                            resolved = replacement;
                            res.kind = ImportResolutionKind::HleRegistry;
                        }
                    }

                    // (b) Check host / HLE shim DLL via LoadLibrary + GetProcAddress.
                    if (!resolved) {
                        HMODULE mod = load_target_module(target_dll, is_hle);
                        if (mod) {
                            FARPROC proc = by_ordinal
                                               ? GetProcAddress(mod, MAKEINTRESOURCEA(ordinal))
                                               : GetProcAddress(mod, sym_name.c_str());
                            if (proc) {
                                resolved = reinterpret_cast<void*>(proc);
                                res.kind = ImportResolutionKind::HostDll;
                            }
                        }
                    }

                    // (c) Fallback to dynamically generated x86-64 stub thunk.
                    if (!resolved) {
                        if (!options.auto_stub_missing) {
                            Unmap();
                            return Status::Error(
                                StatusCode::NotFound,
                                "Unresolved import: " + guest_dll + "!" +
                                    (by_ordinal ? ("#" + std::to_string(ordinal)) : sym_name));
                        }
                        uint32_t stub_id = 0;
                        Status st = stubs_.AllocateStub(guest_dll, sym_name, by_ordinal, ordinal,
                                                        options.default_stub_return, resolved,
                                                        &stub_id);
                        if (!st.ok()) {
                            Unmap();
                            return st;
                        }
                        res.kind = ImportResolutionKind::StubThunk;
                        res.stub_id = stub_id;
                    }

                    iat_thunk->u1.Function = reinterpret_cast<ULONGLONG>(resolved);
                    res.resolved_address = reinterpret_cast<uint64_t>(resolved);
                    resolutions_.push_back(std::move(res));
                }
            }
        }
    }

    // 4.5 Host-process registrations: implicit TLS (while the index variable is
    //     still writable), unwind tables, and the guest module registry.
    {
        auto* state = new LoaderState();
        loader_state_ = state;
        const auto& dirs = nt->OptionalHeader.DataDirectory;
        const DWORD dir_count = nt->OptionalHeader.NumberOfRvaAndSizes;
        if (dir_count > IMAGE_DIRECTORY_ENTRY_TLS && dirs[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress != 0) {
            Status tls_st = guest::AttachTls(
                mapped_base_, mapped_base_ + dirs[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress,
                !is_dll_, &state->tls);
            if (!tls_st.ok()) {
                Unmap();
                return tls_st;
            }
        }
        if (dir_count > IMAGE_DIRECTORY_ENTRY_EXCEPTION &&
            dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress != 0) {
            auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(
                mapped_base_ + dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress);
            const DWORD count = dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size / sizeof(RUNTIME_FUNCTION);
            if (count != 0 &&
                RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(mapped_base_))) {
                state->function_table = table;
            }
        }
        guest::RegisterModule(mapped_base_, size_of_image_,
                              std::filesystem::absolute(pe_path).string(), !is_dll_);
        state->registered_base = mapped_base_;
        for (void* mod : loaded_modules_) {
            guest::PatchHostModuleForGuests(mod);
        }
    }

    // 5. Apply per-section memory protections and flush instruction cache.
    DWORD old_prot = 0;
    VirtualProtect(mapped_base_, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old_prot);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        const size_t prot_size = std::max<size_t>(sec.Misc.VirtualSize, sec.SizeOfRawData);
        if (prot_size == 0) {
            continue;
        }
        const DWORD prot = SectionCharsToPageProtect(sec.Characteristics);
        VirtualProtect(mapped_base_ + sec.VirtualAddress, prot_size, prot, &old_prot);
    }
    FlushInstructionCache(GetCurrentProcess(), mapped_base_, size_of_image_);

    // 6. Execute TLS callbacks if present.
    if (options.run_tls_callbacks &&
        nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_TLS) {
        const auto& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tls_dir.VirtualAddress != 0) {
            const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
                mapped_base_ + tls_dir.VirtualAddress);
            if (tls->AddressOfCallBacks != 0) {
                const auto* cb = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
                while (cb && *cb) {
                    (*cb)(mapped_base_, DLL_PROCESS_ATTACH, nullptr);
                    ++cb;
                }
            }
            if (auto* state = static_cast<LoaderState*>(loader_state_)) {
                guest::EnableTlsCallbacks(&state->tls);
            }
        }
    }

    // 7. For DLL images, invoke DllMain(DLL_PROCESS_ATTACH).
    if (is_dll_ && entry_point_rva_ != 0) {
        using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto fn = reinterpret_cast<DllMainFn>(mapped_base_ + entry_point_rva_);
        if (!fn(reinterpret_cast<HINSTANCE>(mapped_base_), DLL_PROCESS_ATTACH, nullptr)) {
            Unmap();
            return Status::Error(StatusCode::DeviceError,
                                 "Guest DLL DllMain(DLL_PROCESS_ATTACH) returned FALSE");
        }
        dll_attached_ = true;
    }

    return Status::Ok();
#endif
}

void* MappedPeImage::EntryPointAddress() const noexcept {
    if (!mapped_base_ || entry_point_rva_ == 0) {
        return nullptr;
    }
    return mapped_base_ + entry_point_rva_;
}

void* MappedPeImage::GetExportAddress(const std::string& export_name) const {
#if defined(_WIN32)
    if (!mapped_base_ || export_dir_rva_ == 0) {
        return nullptr;
    }
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(mapped_base_ + export_dir_rva_);
    const auto* names = reinterpret_cast<const uint32_t*>(mapped_base_ + exp->AddressOfNames);
    const auto* ordinals =
        reinterpret_cast<const uint16_t*>(mapped_base_ + exp->AddressOfNameOrdinals);
    const auto* funcs = reinterpret_cast<const uint32_t*>(mapped_base_ + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(mapped_base_ + names[i]);
        if (export_name == name) {
            const uint16_t ord_idx = ordinals[i];
            if (ord_idx < exp->NumberOfFunctions) {
                return mapped_base_ + funcs[ord_idx];
            }
        }
    }
#else
    (void)export_name;
#endif
    return nullptr;
}

void* MappedPeImage::GetExportAddress(uint16_t ordinal) const {
#if defined(_WIN32)
    if (!mapped_base_ || export_dir_rva_ == 0) {
        return nullptr;
    }
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(mapped_base_ + export_dir_rva_);
    if (ordinal < exp->Base) {
        return nullptr;
    }
    const uint32_t idx = static_cast<uint32_t>(ordinal - exp->Base);
    if (idx >= exp->NumberOfFunctions) {
        return nullptr;
    }
    const auto* funcs = reinterpret_cast<const uint32_t*>(mapped_base_ + exp->AddressOfFunctions);
    return mapped_base_ + funcs[idx];
#else
    (void)ordinal;
    return nullptr;
#endif
}

Status MappedPeImage::CallExportedEntry(const std::string& export_name, int& out_exit_code) const {
    out_exit_code = -1;
    void* addr = GetExportAddress(export_name);
    if (!addr) {
        return Status::Error(StatusCode::NotFound,
                             "Exported guest entry not found: " + export_name);
    }
    using GuestEntryFn = int (*)();
    auto fn = reinterpret_cast<GuestEntryFn>(addr);
    out_exit_code = fn();
    return Status::Ok();
}

Status MappedPeImage::CallEntryPoint(int& out_exit_code) const {
    out_exit_code = -1;
    void* addr = EntryPointAddress();
    if (!addr) {
        return Status::Error(StatusCode::NotFound, "PE image has no entry point");
    }
    using NativeEntryFn = int (*)();
    auto fn = reinterpret_cast<NativeEntryFn>(addr);
    out_exit_code = fn();
    return Status::Ok();
}

}  // namespace scarlettx
