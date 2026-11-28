#pragma once
// FRIK-flat compatibility layer, included from frik-src/PCH.h and fw-src/PCH.h right after the CommonLib headers.
// Maps the CommonLibF4VR API surface FRIK and F4VR-CommonFramework were written against onto the flat
// CommonLibF4 (libxse, Fallout 4 1.11.240). Every category is logged in analysis/frik-flat-port.md (change log).
//
// Rules: only additions here (aliases, shims, helpers). Anything that changes behaviour is an explicit
// `#ifdef FRIK_FLAT` edit at the call site, so it can be found with a grep.

#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

// ---- C1: {fmt} -> std::format. CommonLibF4VR's spdlog bundles {fmt}; the flat CommonLib's spdlog is built with
// std_format, so `fmt::` is std's.
namespace fmt
{
    using std::format;
    using std::format_to;
    using std::format_to_n;
    using std::formatted_size;
    using std::make_format_args;
    using std::vformat;
    using std::format_string;
    template <class CharT>
    using basic_string_view = std::basic_string_view<CharT>;
    using string_view = std::string_view;
    template <class T, class CharT = char>
    using formatter = std::formatter<T, CharT>;
}

// ---- C2: runtime identity. The flat build is never VR; FRIK's VR-only gates become flat gates through these.
namespace f4cf::flat
{
    inline constexpr bool IS_VR = false;
    inline bool isVR() noexcept { return false; }
    inline bool isF4() noexcept { return true; }
}

// ---- C3: F4SE::log (CommonLibF4VR) -> spdlog / F4SE::GetSaveFolderName (flat)
namespace F4SE::log
{
    std::optional<std::filesystem::path> log_directory();  // Documents/My Games/Fallout4/F4SE (vrshim/src/FlatCompat.cpp)
    using spdlog::critical;
    using spdlog::debug;
    using spdlog::error;
    using spdlog::info;
    using spdlog::trace;
    using spdlog::warn;
}

// ---- C4: address bridging. Every raw Fallout4VR 1.2.72 address (REL::Offset / REL::ID with a VR address-library id)
// in the copied sources is rewritten to one of these. They look the VR address up in the generated table
// (generated/FrikFlatAddresses.inl, from analysis/db/frik_flat_symbols.csv) and give the 1.11.240 address, or 0 when
// the address is VR-only / not mapped (callers must check before use). Every lookup is registered, so the plugin
// can log "N/M addresses resolved" and list the misses.
namespace vrshim
{
    enum class AddrStatus : std::uint8_t
    {
        kUnresolved,
        kMapped,      // function / data, high-medium-anchor match or shared id
        kMappedSite,  // call or patch site re-found inside the mapped flat function
        kMappedLow,   // only a low-confidence function match
        kManual,      // set by hand (analysis/frik-flat-port.md section 1)
    };

    struct AddrEntry
    {
        std::uint64_t vr;        // Fallout4VR 1.2.72 RVA (or VR address-library id when isId)
        std::uint64_t flat;      // Fallout 4 1.11.240 RVA, 0 = none
        AddrStatus    status;
        bool          isId;
        const char*   name;
        bool          isCode;  // VR address in .text: unresolved -> return-0 stub; data -> zero block
    };

    // Unresolved addresses never come back as 0 from flatAddress / FlatOffset / FlatId (that would be a null call or read):
    // code gets a stub that returns 0 (xor eax,eax; xorps xmm0,xmm0; ret), data a zeroed writable 64 KiB block. Both lie
    // outside the game image, so every hook / patch site check (inGameImage) still refuses them.
    std::uintptr_t unresolvedCodeStub() noexcept;
    std::uintptr_t unresolvedDataBlock() noexcept;

    // True if a_address is inside the game's image (a resolved FlatOffset / FlatId).
    bool inGameImage(std::uintptr_t a_address) noexcept;

    // Look up (and register) a VR RVA / VR address-library id. Returns the flat RVA or 0.
    std::uint64_t lookupFlatRva(std::uint64_t a_vr, bool a_isId, const char* a_where) noexcept;
    // Absolute flat address (module base + RVA) or 0.
    std::uintptr_t flatAddress(std::uint64_t a_vr, bool a_isId, const char* a_where) noexcept;

    // Registered lookups so far: total and resolved, plus a log of the misses.
    struct AddrStats
    {
        std::size_t total{ 0 };
        std::size_t resolved{ 0 };
        std::size_t low{ 0 };
    };
    AddrStats addressStats() noexcept;
    void      logAddressReport();

    // Drop-in for REL::Offset(0xVR) in the copied sources: .address() / .offset() / conversion give the FLAT
    // address, or 0 when there is none (REL::Offset would give module base + VR RVA, i.e. garbage on flat).
    class FlatOffset
    {
    public:
        explicit FlatOffset(std::uint64_t a_vrRva, const std::source_location a_loc = std::source_location::current()) noexcept :
            _address(flatAddress(a_vrRva, false, a_loc.file_name()))
        {}
        [[nodiscard]] std::uintptr_t address() const noexcept { return _address; }
        [[nodiscard]] std::size_t    offset() const noexcept;
        [[nodiscard]] bool           resolved() const noexcept { return inGameImage(_address); }
        operator std::uintptr_t() const noexcept { return _address; }

    private:
        std::uintptr_t _address;
    };

    // Drop-in for REL::ID(<VR address-library id>) in the copied sources (CommonLibF4VR resolves those through the VR
    // address library; the flat ids are a different series).
    class FlatId
    {
    public:
        explicit FlatId(std::uint64_t a_vrId, const std::source_location a_loc = std::source_location::current()) noexcept :
            _address(flatAddress(a_vrId, true, a_loc.file_name()))
        {}
        [[nodiscard]] std::uintptr_t address() const noexcept { return _address; }
        [[nodiscard]] bool           resolved() const noexcept { return inGameImage(_address); }
        operator std::uintptr_t() const noexcept { return _address; }

    private:
        std::uintptr_t _address;
    };

}

// ---- C5: CommonLibF4VR names that live elsewhere in the flat CommonLibF4
namespace RE
{
    namespace Scaleform = ::Scaleform;  // CommonLibF4VR nests Scaleform in RE
}

namespace REL
{
    // CommonLibF4VR's REL::Module (the flat lib has REX::FModule): base() and version() are all FRIK uses.
    class Module
    {
    public:
        static Module& get() noexcept
        {
            static Module m;
            return m;
        }
        [[nodiscard]] std::uintptr_t base() const noexcept;
        [[nodiscard]] REL::Version   version() const noexcept;
    };
}

// ---- C6: VR player nodes. Flat PlayerCharacter has no VR node table (VR +0x6E0); fo4flatvr-flat keeps its own table of
// NiNodes with the same names and roles, posed from OpenXR every frame (vrshim/VrNodes.cpp). Same member names as
// CommonLibF4VR's RE::VRPlayerNodes, so FRIK's `nodes->primaryWandNode` code compiles unchanged.
namespace RE
{
    class NiNode;
    class NiCamera;
    class BSTriShape;

    struct VRPlayerNodes
    {
        NiNode*     playerWorldNode;
        NiNode*     roomNode;
        NiNode*     primaryWandNode;
        NiNode*     primaryWandTouchpad;
        NiNode*     primaryUIAttachNode;
        NiNode*     primaryWeaponToWandNode;
        NiNode*     primaryWeaponKickbackRecoilNode;
        NiNode*     primaryWeaponOffsetNode;
        NiCamera*   primaryWeaponScopeCamera;
        NiNode*     primaryVertibirdMinigunOffsetNode;
        NiNode*     primaryMeleeWeaponOffsetNode;
        NiNode*     primaryUnarmedPowerArmorWeaponOffsetNode;
        BSTriShape* primaryWandLaserPointer;
        NiNode*     primaryWandLaserPointerAdjuster;
        NiNode*     equippedWeaponNode;
        NiNode*     primaryMeleeWeaponOffsetNodeInUse;
        NiNode*     secondaryMeleeWeaponOffsetNodeInUse;
        NiNode*     secondaryWandNode;
        NiNode*     point002Node;
        NiNode*     workshopPalletNode;
        NiNode*     workshopPalletSlider;
        NiNode*     secondaryUIOffsetNode;
        NiNode*     secondaryMeleeWeaponOffsetNode;
        NiNode*     secondaryUnarmedPowerArmorWeaponOffsetNode;
        NiNode*     secondaryAimNode;
        NiNode*     pipboyParentNode;
        NiNode*     pipboyRootNIFOnlyNode;
        NiNode*     screenNode;
        NiNode*     pipboyLightParentNode;
        NiNode*     unk7C8;
        NiNode*     scopeParentNode;
        NiNode*     compassDialsNode;
        NiNode*     hmdNode;
        NiNode*     offscreenHmdNode;
        NiNode*     uprightHmdNode;
        NiNode*     uprightHmdLagNode;
        NiNode*     blackSphereNode;
        NiNode*     headLightParentNode;
        NiNode*     unk810;
        NiNode*     weaponLeftNode;
    };
}

namespace vrshim
{
    // The flat stand-in for PlayerCharacter::vrNodes (never null; members null until VrNodes::create ran).
    RE::VRPlayerNodes* getVRPlayerNodes() noexcept;
    bool               vrNodesCreated() noexcept;  // true once vrshim::bridge::frameUpdate posed them
}

namespace RE
{
    class NiAVObject;
    class NiUpdateData;
}

namespace vrshim
{
    // NiAVObject::Update(NiUpdateData&) - a member in CommonLibF4VR, absent from the flat CommonLibF4.
    // Flat 1.11.240 16C8570 (VR 1C22FB0, 1.10.163 1BA3BE0), through the address table.
    void niAVObjectUpdate(RE::NiAVObject* a_object, RE::NiUpdateData& a_data);
}

namespace RE
{
    class PlayerCharacter;
}

namespace vrshim
{
    // C16: first-chance crash logger for bring-up (vrshim/src/CrashLog.cpp)
    void installCrashLog();

    // C14: flat substitute for FO4VR's Update1StPersonArm (vrshim/src/FirstPersonArm.cpp)
    void* update1stPersonArm(const RE::PlayerCharacter* a_pc, RE::NiNode** a_weapon, RE::NiNode** a_offset);
    // FRIK-flat C47 (28 Nov): flat's first-person rig (BSFlattenedBoneTree under firstPerson3D) lives in player-relative
    // space; absolute world = rig world + this offset (translation only, the player's location). The one helper every
    // rig -> world conversion uses (GunWeaponWorld; exported to ROCK-flat as FRIKFLAT_GetFirstPersonRigOffset).
    bool firstPersonRigToWorldOffset(const RE::PlayerCharacter* a_pc, float a_out[3]);
}

// ---- C8: REL write helpers CommonLibF4VR has and the flat lib names differently
namespace REL
{
    template <class T>
    void safe_write(const std::uintptr_t a_dst, const T& a_data)
    {
        REL::WriteSafe(reinterpret_cast<void*>(a_dst), std::addressof(a_data), sizeof(T));
    }
}

#include "vrshim/FlatTypes.h"  // C11: types the flat lib lacks

// ---- C9: CommonLibF4VR singletons spelled differently in the flat lib
namespace RE::BSGraphics
{
    struct RendererDataCompat
    {
        static RendererData* GetSingleton() { return GetRendererData(); }
    };
}

#include "vrshim/ClibVrCompat.h"  // C46: CommonLibF4VR 0.6 names (NiNodeUtils, VRSettings) -> fw-src/f4vr
