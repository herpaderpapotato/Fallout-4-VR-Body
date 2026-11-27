#pragma once
// ---- C46 (27 Nov 2026, upstream sync to FRIK 3d79219e / F4VR-CommonFramework 14a76b94): CommonLibF4VR 0.6 names.
// Upstream moved the framework's game RE (f4vr::findNode, isLeftHandedMode, ...) into CommonLibF4VR as RE::NiNodeUtils /
// RE::VRSettings. The flat port keeps its own versions in fw-src/f4vr (F4VRUtils, F4VROffsets: flat addresses, engine
// cull via vrshim::cull, flat substitutes for VR settings), so these names forward to them. Behaviour stays the port's:
// e.g. SetVisible goes through the engine's SetAppCulled (C45 / 22 Nov), not CommonLibF4VR's raw bit flip.
// Included at the end of FlatCompat.h (both PCHs).

#include <cstdint>
#include <string>
#include <string_view>

namespace RE
{
    class NiAVObject;
    class NiNode;
}

namespace f4cf::f4vr
{
    // fw-src/f4vr/F4VRUtils.h (declared again here: FlatCompat.h is included before the framework headers)
    enum class PipboyPlacement : std::uint8_t;
    bool isLeftHandedMode();
    bool isPipboyOnWrist();
    PipboyPlacement getPipboyPlacement();
    void setPipboyPlacement(PipboyPlacement placement);
    bool isComfortSneakMode();
    bool useWandDirectionalMovement();

    RE::NiAVObject* getFirstChild(RE::NiAVObject* avObject);
    RE::NiAVObject* findAVObject(RE::NiAVObject* node, const std::string& name, int maxDepth);
    RE::NiAVObject* findAVObjectStartsWith(RE::NiAVObject* node, const char* name, int maxDepth);
    RE::NiNode* findNode(RE::NiAVObject* node, const char* name, int maxDepth);
    RE::NiNode* findNodeStartsWith(RE::NiAVObject* node, const char* name, int maxDepth);
    bool isNodeVisible(const RE::NiAVObject* node);
    void setNodeVisibility(RE::NiAVObject* node, bool show);
    void setNodeVisibilityDeep(RE::NiAVObject* node, bool show, bool updateSelf);
    void updateDown(RE::NiAVObject* node, bool updateSelf, const char* ignoreNode);
    void updateDownTo(RE::NiNode* toNode, RE::NiNode* fromNode, bool updateSelf);
    void updateUpTo(RE::NiNode* toNode, RE::NiNode* fromNode, bool updateSelf);
    void updateTransforms(RE::NiAVObject* node);
    void updateTransformsDown(RE::NiAVObject* node, bool updateSelf, const char* ignoreNode);
}

namespace RE::NiNodeUtils
{
    namespace detail
    {
        // the framework's find helpers recurse with a depth budget; CommonLibF4VR's default is "no limit"
        inline constexpr int NO_LIMIT = 1 << 30;
        inline std::string str(const std::string_view a_sv) { return std::string(a_sv); }
    }

    [[nodiscard]] inline NiAVObject* GetFirstChild(NiAVObject* a_object) { return f4cf::f4vr::getFirstChild(a_object); }

    [[nodiscard]] inline NiAVObject* FindObject(NiAVObject* a_root, const std::string_view a_name, const int a_maxDepth = detail::NO_LIMIT)
    {
        return f4cf::f4vr::findAVObject(a_root, detail::str(a_name), a_maxDepth);
    }

    [[nodiscard]] inline NiAVObject* FindObjectStartsWith(NiAVObject* a_root, const std::string_view a_prefix, const int a_maxDepth = detail::NO_LIMIT)
    {
        return f4cf::f4vr::findAVObjectStartsWith(a_root, detail::str(a_prefix).c_str(), a_maxDepth);
    }

    [[nodiscard]] inline NiNode* FindNode(NiAVObject* a_root, const std::string_view a_name, const int a_maxDepth = detail::NO_LIMIT)
    {
        return f4cf::f4vr::findNode(a_root, detail::str(a_name).c_str(), a_maxDepth);
    }

    [[nodiscard]] inline NiNode* FindNodeStartsWith(NiAVObject* a_root, const std::string_view a_prefix, const int a_maxDepth = detail::NO_LIMIT)
    {
        return f4cf::f4vr::findNodeStartsWith(a_root, detail::str(a_prefix).c_str(), a_maxDepth);
    }

    [[nodiscard]] inline bool IsVisible(const NiAVObject* a_object) { return f4cf::f4vr::isNodeVisible(a_object); }
    inline void SetVisible(NiAVObject* a_object, const bool a_visible) { f4cf::f4vr::setNodeVisibility(a_object, a_visible); }
    inline void SetVisibleDeep(NiAVObject* a_root, const bool a_visible, const bool a_includeRoot = true)
    {
        f4cf::f4vr::setNodeVisibilityDeep(a_root, a_visible, a_includeRoot);
    }

    inline void UpdateWorldDataDown(NiAVObject* a_root, const bool a_includeRoot = true, const std::string_view a_skipName = {})
    {
        const auto skip = detail::str(a_skipName);
        f4cf::f4vr::updateDown(a_root, a_includeRoot, skip.empty() ? nullptr : skip.c_str());
    }

    inline void UpdateWorldDataDownTo(NiNode* a_to, NiNode* a_from, const bool a_includeFrom = true) { f4cf::f4vr::updateDownTo(a_to, a_from, a_includeFrom); }
    inline void UpdateWorldDataUpTo(NiNode* a_to, NiNode* a_from, const bool a_includeTo = true) { f4cf::f4vr::updateUpTo(a_to, a_from, a_includeTo); }

    inline void UpdateTransformFromParent(NiAVObject* a_object)
    {
        if (a_object) {
            f4cf::f4vr::updateTransforms(a_object);
        }
    }

    inline void UpdateTransformsDown(NiAVObject* a_root, const bool a_includeRoot = true, const std::string_view a_skipName = {})
    {
        const auto skip = detail::str(a_skipName);
        f4cf::f4vr::updateTransformsDown(a_root, a_includeRoot, skip.empty() ? nullptr : skip.c_str());
    }
}

namespace RE::VRSettings
{
    // CommonLibF4VR's enumerators; same values as f4vr::PipboyPlacement (the order of the game's settings menu)
    enum class PipboyPlacement : std::uint8_t
    {
        kWrist = 0,
        kInFront = 1,
        kProjected = 2,
    };

    [[nodiscard]] inline bool IsLeftHandedMode() { return f4cf::f4vr::isLeftHandedMode(); }
    [[nodiscard]] inline bool IsComfortSneak() { return f4cf::f4vr::isComfortSneakMode(); }
    [[nodiscard]] inline bool IsWandDirectionalMovement() { return f4cf::f4vr::useWandDirectionalMovement(); }
    [[nodiscard]] inline bool IsPipboyOnWrist() { return f4cf::f4vr::isPipboyOnWrist(); }
    [[nodiscard]] inline PipboyPlacement GetPipboyPlacement() { return static_cast<PipboyPlacement>(f4cf::f4vr::getPipboyPlacement()); }
    inline void SetPipboyPlacement(const PipboyPlacement a_placement)
    {
        f4cf::f4vr::setPipboyPlacement(static_cast<f4cf::f4vr::PipboyPlacement>(a_placement));
    }
}
