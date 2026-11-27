#pragma once
// Whole-head hide, one implementation for fo4flatvr and FRIK-flat (19 Nov). Header-only: compiled into both DLLs.
//   - fo4flatvr camdir::PollHeadHide: the AAF POV / free-camera scene view at an actor's head (analysis/third-person-vr.md 7).
//   - FRIK-flat C39 (CullGeometryHandler): the player's head in first person instead of FRIK's head push-back, so the
//     shadow is headless and nothing of the head is in view (user 19 Nov: "A headless shadow would be better").
// Whole head or nothing: the actor's BSFaceGenNiNodeSkinned node (every head part at once: face, hair, eyes, brows, teeth,
// beard), the ARMO part clones in FRIK's head biped slots, and head gear found by FRIK's face-list words inside the actor's
// other ARMO pieces (a hat or hood on slot 54/55). No FaceGen node = nothing hidden. Only nodes that were visible are culled
// (app-cull bit 0), and Restore() shows exactly those again; a node another mod or FRIK's own cull hid is never ours.
// Include after the CommonLibF4 headers (RE::Actor, RE::NiAVObject, RE::BipedAnim).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fo4flatvr::headhide
{
    inline constexpr const char* kFaceNode = "BSFaceGenNiNodeSkinned";

    // FRIK's Mesh_Hide\face.ini list (FRIK-flat\FRIK_Config): head gear names
    inline constexpr const char* kHeadTokens[] = { "bandage", "beard", "cap", "cowl", "crown", "eyepiece", "eyes", "fedora", "glasses",
        "goggles", "hair", "head", "helm", "helmet", "hood", "mask", "mouth", "neckgore", "optics prototype", "shades", "visor",
        "wighairtop" };

    // head biped slots FRIK hides (CullGeometryHandler::restoreEquipment): hair top/long, FaceGen head, headband, eyes, beard,
    // mouth, neck, scalp
    inline constexpr int kHeadSlots[] = { 0, 1, 2, 16, 17, 18, 19, 20, 22 };

    // 22 Nov (VATS head agent): culls go through the engine's NiAVObject::SetAppCulled (vtable 0x2D, flat 16C86C0). Besides bit 0
    // it sets 0x20000000 on the node and on every parent up to one that already has it - the engine's "changed below" mark.
    // Flipping bit 0 directly (as this file did) left that mark unset: after a first-person hide, the head shown again for a
    // VATS playback was un-culled but not drawn (measured: bone matrices not built for the whole playback; with the hide off it
    // drew).
    // The cull call each DLL installs: FRIK-flat vrshim::cull::Set, fo4flatvr enginecull::Set (both: bit 0 at once + the engine's
    // SetAppCulled on the main thread, [FRIK] bEngineCullFlag). Not installed: the direct bit.
    using CullFn = void (*)(RE::NiAVObject*, bool);
    inline CullFn& CullHook()
    {
        static CullFn s_fn = nullptr;
        return s_fn;
    }

    inline void SetCulled(RE::NiAVObject* a_o, bool a_culled)
    {
        if (!a_o) return;
        if (auto fn = CullHook()) {
            fn(a_o, a_culled);
        } else if (a_culled) {
            a_o->flags.flags |= 1;
        } else {
            a_o->flags.flags &= ~static_cast<std::uint64_t>(1);
        }
    }

    inline bool HeadName(const char* a_name)
    {
        if (!a_name || !*a_name) {
            return false;
        }
        std::string n = a_name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* t : kHeadTokens) {
            if (n.find(t) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    class Set
    {
    public:
        // Hide a_actor's head under a_root (its third-person 3D). Adds what is newly hidden; false = no FaceGen node (nothing).
        bool Scan(RE::Actor* a_actor, RE::NiAVObject* a_root)
        {
            auto* face = a_root ? a_root->GetObjectByName(RE::BSFixedString(kFaceNode)) : nullptr;
            if (!a_actor || !face) {
                return false;
            }
            Cull(face);
            if (auto* biped = a_actor->biped.get()) {
                for (int slot = 0; slot < 32; ++slot) {
                    const auto& b = biped->object[slot];
                    if (!b.parent.object || !b.partClone || b.parent.object->GetFormType() != RE::ENUM_FORM_ID::kARMO) {
                        continue;
                    }
                    if (std::find(std::begin(kHeadSlots), std::end(kHeadSlots), slot) != std::end(kHeadSlots)) {
                        Cull(b.partClone.get());  // head slot: the whole piece
                    } else {
                        Collect(b.partClone.get(), 0);  // other slots: only its head-gear geometry
                    }
                }
            }
            return true;
        }

        // keep hidden what this set hid (a re-equip or another mod may clear the flag)
        void Keep()
        {
            for (auto& o : _culled) {
                if (o && !(o->flags.flags & 1)) SetCulled(o.get(), true);
            }
        }

        // show exactly what this set hid; returns how many nodes
        std::size_t Restore()
        {
            for (auto& o : _culled) {
                if (o) SetCulled(o.get(), false);
            }
            const auto n = _culled.size();
            _culled.clear();
            return n;
        }

        std::size_t Size() const { return _culled.size(); }

        // 22 Nov: what the set holds, for the logs ("'name'@ptr, ...", first a_max)
        std::string Names(std::size_t a_max = 6) const
        {
            std::string s;
            for (std::size_t i = 0; i < _culled.size() && i < a_max; ++i) {
                const auto& o = _culled[i];
                char buf[32];
                std::snprintf(buf, sizeof(buf), "@%p", static_cast<const void*>(o.get()));
                s += (i ? ", '" : "'") + std::string(o && o->name.c_str() ? o->name.c_str() : "") + "'" + buf;
            }
            if (_culled.size() > a_max) s += ", ...";
            return s;
        }

        bool Contains(const RE::NiAVObject* a_o) const
        {
            return std::any_of(_culled.begin(), _culled.end(), [&](const auto& o) { return o.get() == a_o; });
        }
        bool        Empty() const { return _culled.empty(); }

    private:
        void Cull(RE::NiAVObject* o)
        {
            if (!o || (o->flags.flags & 1)) {
                return;  // already culled (by the game, FRIK or another mod): never ours to show again
            }
            SetCulled(o, true);
            _culled.emplace_back(o);
        }

        void Collect(RE::NiAVObject* o, int a_depth)
        {
            if (!o || a_depth > 40) {
                return;
            }
            if (o->IsGeometry()) {
                if (HeadName(o->name.c_str())) Cull(o);
                return;
            }
            if (auto* n = o->IsNode()) {
                for (auto& c : n->children) {
                    if (c) Collect(c.get(), a_depth + 1);
                }
            }
        }

        std::vector<RE::NiPointer<RE::NiAVObject>> _culled;
    };
}
