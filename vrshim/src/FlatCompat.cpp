// FRIK-flat: implementation of vrshim/FlatCompat.h (address table lookups, log directory).
#include <ShlObj.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "vrshim/FlatCompat.h"

namespace vrshim
{
    namespace
    {
        constexpr AddrEntry TABLE[] = {
#include "FrikFlatAddresses.inl"
        };

        struct Lookup
        {
            std::uint64_t vr;
            bool isId;
            std::uint64_t flat;
            AddrStatus status;
            const char* where;
        };

        // function-local static: lookups run from other translation units' static initialisers
        std::mutex& lock()
        {
            static std::mutex m;
            return m;
        }

        std::vector<Lookup>& lookups()
        {
            static std::vector<Lookup> v;
            return v;
        }

        std::uintptr_t imageBase() noexcept
        {
            return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        }

        std::size_t imageSize() noexcept
        {
            const auto base = imageBase();
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            return nt->OptionalHeader.SizeOfImage;
        }
    }

    std::uint64_t lookupFlatRva(const std::uint64_t a_vr, const bool a_isId, const char* a_where) noexcept
    {
        const AddrEntry* hit = nullptr;
        for (const auto& e : TABLE) {
            if (e.vr == a_vr && e.isId == a_isId) {
                hit = &e;
                break;
            }
        }
        const std::uint64_t flat = hit && hit->status != AddrStatus::kUnresolved ? hit->flat : 0;
        try {
            std::scoped_lock l(lock());
            auto& v = lookups();
            const auto it = std::ranges::find_if(v, [&](const Lookup& x) {
                return x.vr == a_vr && x.isId == a_isId;
            });
            if (it == v.end()) {
                v.push_back({ a_vr, a_isId, flat, hit ? hit->status : AddrStatus::kUnresolved, hit ? hit->name : a_where });
            }
        } catch (...) {
        }
        return flat;
    }

    std::uintptr_t unresolvedCodeStub() noexcept
    {
        static const std::uintptr_t stub = [] {
            auto* p = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!p)
                return std::uintptr_t{ 0 };
            static constexpr std::uint8_t code[] = { 0x33, 0xC0, 0x0F, 0x57, 0xC0, 0xC3 }; // xor eax,eax; xorps xmm0,xmm0; ret
            std::memcpy(p, code, sizeof(code));
            DWORD old = 0;
            ::VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
            return reinterpret_cast<std::uintptr_t>(p);
        }();
        return stub;
    }

    std::uintptr_t unresolvedDataBlock() noexcept
    {
        alignas(64) static std::uint8_t block[64 * 1024]{};
        return reinterpret_cast<std::uintptr_t>(block);
    }

    std::uintptr_t flatAddress(const std::uint64_t a_vr, const bool a_isId, const char* a_where) noexcept
    {
        const auto rva = lookupFlatRva(a_vr, a_isId, a_where);
        if (rva) {
            return imageBase() + rva;
        }
        bool code = a_isId; // every id used here names a function
        for (const auto& e : TABLE) {
            if (e.vr == a_vr && e.isId == a_isId) {
                code = e.isCode;
                break;
            }
        }
        return code ? unresolvedCodeStub() : unresolvedDataBlock();
    }

    std::size_t FlatOffset::offset() const noexcept
    {
        return _address ? _address - imageBase() : 0;
    }

    bool inGameImage(const std::uintptr_t a_address) noexcept
    {
        const auto base = imageBase();
        return a_address >= base + 0x1000 && a_address < base + imageSize();
    }

    AddrStats addressStats() noexcept
    {
        AddrStats s;
        try {
            std::scoped_lock l(lock());
            for (const auto& x : lookups()) {
                ++s.total;
                if (x.flat) {
                    ++s.resolved;
                    if (x.status == AddrStatus::kMappedLow) {
                        ++s.low;
                    }
                }
            }
        } catch (...) {
        }
        return s;
    }

    void logAddressReport()
    {
        std::scoped_lock l(lock());
        for (const auto& x : lookups()) {
            if (!x.flat) {
                spdlog::warn("FRIK-flat: unresolved VR {} 0x{:X} ({}) - feature disabled", x.isId ? "id" : "rva", x.vr, x.where ? x.where : "?");
            } else if (x.status == AddrStatus::kMappedLow) {
                spdlog::info("FRIK-flat: low-confidence VR 0x{:X} -> flat 0x{:X} ({})", x.vr, x.flat, x.where ? x.where : "?");
            }
        }
    }
}

namespace F4SE::log
{
    std::optional<std::filesystem::path> log_directory()
    {
        wchar_t* docs = nullptr;
        std::optional<std::filesystem::path> out;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
            out = std::filesystem::path(docs) / "My Games" / "Fallout4" / "F4SE";
        }
        ::CoTaskMemFree(docs);
        return out;
    }
}

std::uintptr_t REL::Module::base() const noexcept
{
    return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
}

REL::Version REL::Module::version() const noexcept
{
    return F4SE::RUNTIME_LATEST; // the plugin only loads on the runtime the flat CommonLibF4 targets (1.11.240)
}

void vrshim::niAVObjectUpdate(RE::NiAVObject* a_object, RE::NiUpdateData& a_data)
{
    using fn_t = void (*)(RE::NiAVObject*, RE::NiUpdateData&);
    static const auto fn = reinterpret_cast<fn_t>(vrshim::flatAddress(0x1C22FB0, false, "NiAVObject::Update"));
    if (fn && a_object) {
        fn(a_object, a_data);
    }
}

std::int32_t vrshim::actorRequestDetectionLevel(RE::Actor* a_observer, RE::Actor* a_target, const std::int32_t a_priority)
{
    using fn_t = std::int32_t (*)(RE::Actor*, RE::Actor*, std::int32_t);
    static const auto fn = reinterpret_cast<fn_t>(vrshim::flatAddress(943772, true, "Actor::RequestDetectionLevel"));
    return fn && a_observer && a_target ? fn(a_observer, a_target, a_priority) : -1;
}
