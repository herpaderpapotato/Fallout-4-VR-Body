#pragma once
// FRIK-flat: VR player node table (see vrshim/src/VrNodes.cpp). RE::VRPlayerNodes is declared in FlatCompat.h.
#include "RE/Fallout.h"

namespace vrshim
{
    // One frame of poses, already converted to game space (Z up, game units) by the caller (fo4flatvr exports).
    struct VrFramePoses
    {
        RE::NiTransform playerWorld; // PlayerWorldNode local = world (player location, body yaw)
        RE::NiTransform roomLocal; // RoomNode in PlayerWorldNode (roomscale origin)
        bool hmdValid{ false };
        RE::NiTransform hmdInRoom;
        RE::NiTransform hmdUprightInRoom; // HMD with yaw only
        bool primaryValid{ false };
        RE::NiTransform primaryInRoom; // primary (weapon) hand grip
        bool secondaryValid{ false };
        RE::NiTransform secondaryInRoom;
        RE::NiNode* firstPersonWeaponLeft{ nullptr };
    };

    bool vrNodesCreated() noexcept;
    void createVrNodes();
    void updateVrNodes(const VrFramePoses& a_poses);
}

namespace vrshim
{
    // C19: the first-person hand/weapon nodes FRIK places its hand targets with (RArm_Hand, LArm_Hand, Weapon, WeaponLeft).
    // FO4VR's first-person skeleton always has them; flat's only while a weapon is drawn. selectFpHands() (once per frame,
    // from bridge::frameUpdate, before FRIK's update) picks the real nodes when all four exist, else detached stand-ins
    // (stand-in hand with a stand-in weapon child, placed by update1stPersonArm: hand = offset node . weapon local^-1).
    // The generation changes whenever the selection changes; FRIK re-initialises its skeleton then (it caches the hands).
    struct FpHands
    {
        RE::NiNode* rHand{ nullptr };
        RE::NiNode* lHand{ nullptr };
        RE::NiNode* weapon{ nullptr };
        RE::NiNode* weaponLeft{ nullptr };
        bool standIn{ true };
    };

    void selectFpHands();
    const FpHands& fpHands() noexcept;
    std::uint64_t fpHandsGeneration() noexcept;
    bool isStandInWeapon(const RE::NiNode* a_node) noexcept;
}

namespace vrshim
{
    // C27: counters of the primary stand-in hand placement (update1stPersonArm): frames placed from the floating gun's WEAPON.
    std::uint64_t gripFromGunFrames() noexcept;
    std::uint64_t gripFrames() noexcept;
}

namespace vrshim
{
    // C29: FRIK's hand-to-weapon local for the drawn weapon (its weapon adjuster's per-weapon offset, read before FRIK writes
    // its glue for the arm placement) and whether a real weapon (not fists) is drawn. The primary hand then holds the floating
    // gun's WEAPON with exactly that offset: hand = WEAPON(gun) . local^-1.
    void setPrimaryWeaponInHand(const RE::NiTransform& a_local, bool a_realWeapon) noexcept;
}
