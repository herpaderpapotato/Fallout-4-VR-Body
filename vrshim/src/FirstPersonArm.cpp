// FRIK-flat C14: flat substitute for FO4VR's engine Update1StPersonArm (VR EF6280, no flat counterpart).
//
// VR: Update1StPersonArm(pc, &WEAPON, &offsetNode) rotates the hand's parent so WEAPON matches the offset node's rotation,
// translates the elbow chain by the remaining error and runs NiAVObject::Update(firstPerson3D). FRIK calls it every frame
// for both hands (Skeleton::updateHandTarget -> placeFirstPersonArm), with its glue presented on WEAPON, and then solves the
// third-person arms to the first-person hands.
// Flat: the same end state by a rigid move of the arm's upper-arm bone (WEAPON lands exactly on the offset node), then the
// engine's own first-person update - the call and addresses fo4flatvr's hands code verified in game (Hands.cpp W1):
//   D9F7D0 (float time, NiAVObject*, NiCamera*, bool) -> NiAVObject::Update, 21FA470 Q1stPersonCamera, 30E8940 frame time.
#include "vrshim/Bridge.h"
#include "vrshim/FlatCompat.h"
#include "vrshim/VrNodes.h"
#include "vrshim/XfMath.h"

#include <Windows.h>

#include <cmath>
#include <cstring>

namespace vrshim
{
    namespace
    {
        constexpr std::uintptr_t kUpdateNode = 0xD9F7D0;
        constexpr std::uintptr_t kQ1stPersonCamera = 0x21FA470;
        constexpr std::uintptr_t kFrameTime = 0x30E8940;
        using UpdateNode_t = void (*)(float, RE::NiAVObject*, void*, bool);
        using QCam_t = void* (*)();

        std::uintptr_t Base()
        {
            return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        }

        bool PrologueOk()
        {
            static const bool ok = [] {
                static constexpr std::uint8_t kUpd[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 };
                static constexpr std::uint8_t kQCam[] = { 0x48, 0x8B, 0x05, 0x59, 0x7E, 0xC6, 0x01, 0xC3 };
                const bool a = std::memcmp(reinterpret_cast<const void*>(Base() + kUpdateNode), kUpd, sizeof(kUpd)) == 0;
                const bool b = std::memcmp(reinterpret_cast<const void*>(Base() + kQ1stPersonCamera), kQCam, sizeof(kQCam)) == 0;
                if (!a || !b) {
                    spdlog::error("FRIK-flat: first-person update D9F7D0 / Q1stPersonCamera 21FA470 bytes differ ({} / {}): arm placement off", a, b);
                }
                return a && b;
            }();
            return ok;
        }

        std::uint64_t g_gripFrames = 0, g_gripFromGun = 0;
        vrshim::xf::Xf g_inHand;
        bool g_inHandReal = false;

        RE::BSFlattenedBoneTree* FindTree(RE::NiAVObject* a_obj, int a_depth)
        {
            if (!a_obj)
                return nullptr;
            if (const auto* rtti = a_obj->GetRTTI(); rtti && rtti->GetName() && std::strcmp(rtti->GetName(), "BSFlattenedBoneTree") == 0) {
                return static_cast<RE::BSFlattenedBoneTree*>(a_obj);
            }
            if (a_depth <= 0)
                return nullptr;
            if (auto* n = a_obj->IsNode()) {
                for (auto& c : n->children) {
                    if (auto* t = FindTree(c.get(), a_depth - 1))
                        return t;
                }
            }
            return nullptr;
        }

        // The floating gun's WEAPON (first-person rig, player-relative at this point of the frame) in absolute world.
        // Only while a weapon is drawn and not in power armour (the gun's own gates; elsewhere it does not place WEAPON).
        bool GunWeaponWorld(const RE::PlayerCharacter* a_pc, vrshim::xf::Xf& a_out)
        {
            if (!a_pc)
                return false;
            const auto bits = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(a_pc) + 0x134);
            if (((bits >> 1) & 7) != 3)
                return false;
            auto* tree = FindTree(a_pc->firstPerson3D.get(), 3);
            if (!tree || !tree->bone)
                return false;
            static RE::BSFlattenedBoneTree* s_tree = nullptr;
            static int s_idx = -1, s_count = -1;
            if (tree != s_tree || tree->boneCount != s_count) {
                s_tree = tree, s_count = tree->boneCount, s_idx = -1;
                for (int i = 0; i < tree->boneCount; ++i) {
                    const char* nm = tree->bone[i].name.c_str();
                    if (nm && _stricmp(nm, "WEAPON") == 0) {
                        s_idx = i;
                        break;
                    }
                }
            }
            if (s_idx < 0 || !tree->bone[s_idx].node)
                return false;
            a_out = vrshim::xf::FromNi(tree->bone[s_idx].node->world);
            a_out.t = a_out.t + vrshim::xf::V{ a_pc->data.location.x, a_pc->data.location.y, a_pc->data.location.z };
            return true;
        }

        bool EndsWith(const char* a_s, const char* a_suffix)
        {
            const auto n = std::strlen(a_s), m = std::strlen(a_suffix);
            return n >= m && _stricmp(a_s + n - m, a_suffix) == 0;
        }
    }

    namespace
    {
        void* place(const RE::PlayerCharacter* a_pc, RE::NiNode* weapon, RE::NiNode* offset);
    }

    void* update1stPersonArm(const RE::PlayerCharacter* a_pc, RE::NiNode** a_weapon, RE::NiNode** a_offset)
    {
        auto* weapon = a_weapon ? *a_weapon : nullptr;
        auto* r = place(a_pc, weapon, a_offset ? *a_offset : nullptr);
        if (weapon) {
            weapon->DecRefCount(); // VR EF6280 releases the reference its caller took (Skeleton::updateHandTarget IncRefs)
        }
        return r;
    }

    namespace
    {
        void* place(const RE::PlayerCharacter* a_pc, RE::NiNode* weapon, RE::NiNode* offset)
        {
            // C19: stand-in hand (no drawn weapon on flat): the hand itself goes where WEAPON lands on the offset node
            if (isStandInWeapon(weapon) && offset && weapon->parent) {
                using namespace vrshim::xf;
                auto* hand = weapon->parent;
                Xf target = FromNi(offset->world);
                // C27 (user headset 15 Nov: revolver rotated off the grip): the drawn weapon is placed by fo4flatvr's floating gun
                // (per-weapon JSON + its own calibration), so the primary hand takes the gun's actual WEAPON as its target and
                // keeps FRIK's hand-to-weapon local: hand = WEAPON(gun) . weaponLocal^-1. Needs the gun's write of this frame:
                // fo4flatvr runs FRIK's frame after its gun (FrikBridge.cpp HkNotify). Falls back to FRIK's offset node.
                // 16 Nov: not in a pause-time frame - the floating gun is not placed while the game is paused (its W1 hook is in the
                // player update), so its WEAPON is frozen; the hand follows the controller instead (user: hands frozen at a terminal)
                if (weapon == fpHands().weapon && g_inHandReal && !vrshim::bridge::pausedFrame()) {
                    ++g_gripFrames;
                    Xf gun;
                    if (GunWeaponWorld(a_pc, gun) &&
                        std::sqrt((gun.t - target.t).x * (gun.t - target.t).x + (gun.t - target.t).y * (gun.t - target.t).y + (gun.t - target.t).z * (gun.t - target.t).z) < 40.0) {
                        gun.s = FromNi(offset->world).s;
                        // C29: hand = WEAPON(gun) . (FRIK's weapon-in-hand local)^-1 (C27 used the arm-placement glue here, which put the
                        // hand off the grip by the per-weapon offset - user 15 Nov 15:33)
                        const Xf handWorld = Mul(gun, Inv(g_inHand));
                        hand->local = ToNi(handWorld);
                        hand->world = hand->local;
                        weapon->world = ToNi(Mul(handWorld, FromNi(weapon->local)));
                        ++g_gripFromGun;
                        return nullptr;
                    }
                }
                const Xf handWorld = Mul(target, Inv(FromNi(weapon->local)));
                hand->local = ToNi(handWorld);
                hand->world = hand->local;
                weapon->world = ToNi(Mul(handWorld, FromNi(weapon->local)));
                return nullptr;
            }
            auto* fp = a_pc ? a_pc->firstPerson3D.get() : nullptr;
            if (!weapon || !offset || !fp || !PrologueOk()) {
                return nullptr;
            }
            RE::NiNode* upper = nullptr;
            for (auto* p = weapon->parent; p; p = p->parent) {
                if (EndsWith(p->name.c_str(), "Arm_UpperArm")) {
                    upper = p;
                    break;
                }
            }
            if (!upper || !upper->parent) {
                logger::sample(10000, "FRIK-flat: no *Arm_UpperArm above '{}': first-person arm not placed", weapon->name.c_str());
                return nullptr;
            }
            // world delta D = target . WEAPON^-1, applied to the upper arm: upper.world' = D . upper.world
            using namespace vrshim::xf;
            const Xf D = Mul(FromNi(offset->world), Inv(FromNi(weapon->world)));
            upper->local = ToNi(Mul(Inv(FromNi(upper->parent->world)), Mul(D, FromNi(upper->world))));
            const float ft = *reinterpret_cast<const float*>(Base() + kFrameTime);
            reinterpret_cast<UpdateNode_t>(Base() + kUpdateNode)(ft, fp, reinterpret_cast<QCam_t>(Base() + kQ1stPersonCamera)(), false);
            return nullptr;
        }
    }
}

namespace vrshim
{
    std::uint64_t gripFromGunFrames() noexcept
    {
        return g_gripFromGun;
    }

    std::uint64_t gripFrames() noexcept
    {
        return g_gripFrames;
    }
}

namespace vrshim
{
    void setPrimaryWeaponInHand(const RE::NiTransform& a_local, bool a_realWeapon) noexcept
    {
        g_inHand = xf::FromNi(a_local);
        g_inHandReal = a_realWeapon;
    }
}
