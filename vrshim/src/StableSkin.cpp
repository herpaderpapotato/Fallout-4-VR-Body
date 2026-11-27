// FRIK-flat C23: stable skinning for the player's third-person body.
//
// Flat 1.11.240 renders frame N on the main thread (Main::Swap -> DrawWorld) while the job threads already run the update of
// frame N+1 (analysis/vr-hands-weapon.md 1.3, measured 14 Nov), and BSGraphics::Utility::CalculateBoneMatrices (flat 182E190)
// builds the skinning matrices at render time from BSSkin::Instance::worldTransforms - pointers to the live bone worlds.
// The next frame's animation update rewrites those bones from the animation pose (measured 15 Nov 14:29: 101 of 126 body bones
// > 2 u away from FRIK's pose between two FRIK frames, up to 51 u), so the render skins a mix of FRIK's pose and the
// animation pose: the outfit stretched into ribbons. The engine's own actors never show it because consecutive animation
// poses are nearly equal.
// Fix: each skin instance under the player's third-person root gets its own copy of its bone worlds, refreshed at the end of
// FRIK's frame, and its worldTransforms entries point at that copy. The next frame's animation writes the bones, not the
// copy, so the render always skins a FRIK pose (this frame's or, while FRIK's next frame writes the copy, the next one).
// BSSkin::Instance layout (flat, from CalculateBoneMatrices): +0x28 NiTransform** worldTransforms data, +0x38 uint32 count.
#include "vrshim/StableSkin.h"
#include "vrshim/Bridge.h"  // C23b switch

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vrshim::skin
{
    namespace
    {
        constexpr std::size_t kWtData = 0x28;
        constexpr std::size_t kWtCount = 0x38;
        constexpr std::size_t kBones = 0x10;       // C23b: BSTArray<NiAVObject*> bones (data), count at +0x20
        constexpr std::size_t kBonesCount = 0x20;

        // the engine's own pointer for entry i: its bone's world (nullptr = unknown layout / no bone)
        // [FRIK] bSkinRestoreLiveBones (fo4flatvr.ini, live): 0 = the 15 Nov remembered-pointer behaviour only
        bool LiveBonesOn() { return vrshim::bridge::iniIntLive("bSkinRestoreLiveBones", 1) != 0; }

        RE::NiTransform* LiveWorld(void* a_inst, std::uint32_t a_n, std::uint32_t a_i)
        {
            auto* const* bones = *reinterpret_cast<RE::NiAVObject* const* const*>(static_cast<std::uint8_t*>(a_inst) + kBones);
            const std::uint32_t nb = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(a_inst) + kBonesCount);
            return bones && nb == a_n && a_i < nb && bones[a_i] ? &bones[a_i]->world : nullptr;
        }

        struct Shadow
        {
            std::vector<RE::NiTransform*> orig;    // the engine's pointers (bone worlds)
            std::vector<RE::NiTransform>  copy;    // what the render reads instead
        };
        std::unordered_map<void*, Shadow> g_shadows;
        Stats                             g_stats;

        RE::NiTransform** Data(void* a_inst) { return *reinterpret_cast<RE::NiTransform***>(static_cast<std::uint8_t*>(a_inst) + kWtData); }
        std::uint32_t     Count(void* a_inst) { return *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(a_inst) + kWtCount); }

        void Collect(RE::NiAVObject* a_obj, std::vector<void*>& a_out, int a_depth)
        {
            if (!a_obj || a_depth < 0) return;
            if (auto* geom = a_obj->IsGeometry()) {
                if (geom->skinInstance) a_out.push_back(geom->skinInstance.get());
                return;
            }
            if (auto* node = a_obj->IsNode()) {
                for (auto& c : node->children) {
                    if (c) Collect(c.get(), a_out, a_depth - 1);
                }
            }
        }
    }

    void refresh(RE::NiAVObject* a_root)
    {
        std::vector<void*> insts;
        Collect(a_root, insts, 24);
        std::unordered_set<void*> seen;
        g_stats.instances = 0, g_stats.bones = 0;
        for (void* inst : insts) {
            if (!seen.insert(inst).second) continue;
            const std::uint32_t n = Count(inst);
            RE::NiTransform**   wt = Data(inst);
            if (!wt || n == 0 || n > 1024) continue;
            auto& sh = g_shadows[inst];
            bool  rebuild = sh.copy.size() != n;
            if (!rebuild) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    if (wt[i] != &sh.copy[i]) {  // set up again by the engine (attach / re-equip): take its pointers anew
                        rebuild = true;
                        break;
                    }
                }
            }
            if (rebuild) {
                std::vector<RE::NiTransform*> orig(wt, wt + n);
                const bool liveBones = LiveBonesOn();
                for (std::uint32_t i = 0; i < n; ++i) {
                    // C23b: the engine's pointer is its bone's world; take that, never a pointer into a copy (ours, or one
                    // already freed when a shadow was dropped while the instance still pointed into it)
                    if (auto* lw = liveBones ? LiveWorld(inst, n, i) : nullptr) {
                        orig[i] = lw;
                        continue;
                    }
                    // an entry still pointing into our previous copy keeps its previous engine pointer (never our own memory)
                    if (!sh.copy.empty() && wt[i] >= sh.copy.data() && wt[i] < sh.copy.data() + sh.copy.size()) {
                        orig[i] = i < sh.orig.size() ? sh.orig[i] : nullptr;
                    }
                }
                sh.orig = std::move(orig);
                sh.copy.assign(n, RE::NiTransform{});
                ++g_stats.rebuilds;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                if (sh.orig[i]) sh.copy[i] = *sh.orig[i];
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                if (sh.orig[i]) wt[i] = &sh.copy[i];
            }
            ++g_stats.instances;
            g_stats.bones += n;
        }
        // forget instances that are gone (never touched again: they may be freed)
        for (auto it = g_shadows.begin(); it != g_shadows.end();) {
            it = seen.contains(it->first) ? std::next(it) : g_shadows.erase(it);
        }
    }

    void restore(RE::NiAVObject* a_root)
    {
        std::vector<void*> insts;
        if (a_root) Collect(a_root, insts, 24);
        std::unordered_set<void*> live(insts.begin(), insts.end());
        for (auto& [inst, sh] : g_shadows) {
            if (!live.contains(inst)) continue;  // not under the root any more: may be freed, leave it
            const std::uint32_t n = Count(inst);
            RE::NiTransform**   wt = Data(inst);
            if (!wt || n != sh.orig.size()) continue;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (sh.orig[i] && wt[i] == &sh.copy[i]) wt[i] = sh.orig[i];
            }
        }
        // C23b (22 Nov, VATS head agent): every entry that still points into one of our copies goes back to its live bone's
        // world (BSSkin::Instance +0x10 bones; the engine's own entry is &bones[i]->world, NiAVObject +0x70: measured on all 20
        // player skin instances during a VATS playback). The remembered-pointer pass above misses an entry whose shadow was
        // dropped or rebuilt while it pointed into a copy; that entry would read freed memory once the copies are cleared below.
        std::uint32_t missed = 0;
        for (void* inst : LiveBonesOn() ? live : std::unordered_set<void*>{}) {
            const std::uint32_t n = Count(inst);
            RE::NiTransform**   wt = Data(inst);
            if (!wt || n == 0 || n > 1024) continue;
            for (std::uint32_t i = 0; i < n; ++i) {
                auto* liveWorld = LiveWorld(inst, n, i);
                if (liveWorld && wt[i] != liveWorld) {
                    wt[i] = liveWorld;
                    ++missed;
                }
            }
        }
        g_stats.lastRestoreMissed = missed;
        g_stats.lastRestoreInstances = static_cast<std::uint32_t>(live.size());
        g_stats.restoreMissedTotal += missed;
        g_shadows.clear();
        g_stats.instances = 0, g_stats.bones = 0;
    }

    const Stats& stats() noexcept { return g_stats; }
}
