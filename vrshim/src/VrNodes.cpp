// FRIK-flat: the VR player node table (flat stand-in for PlayerCharacter::vrNodes, VR +0x6E0).
//
// FO4VR builds these nodes in the PlayerCharacter VR setup (VR 0xEF21DD) and poses PlayerWorldNode/RoomNode/HMDNode/
// wands from OpenVR every frame. Here they are plain NiNodes with the same names and the same parenting, created once
// the game is loaded (createVrNodes) and posed by vrshim::updateVrNodes from fo4flatvr's OpenXR poses
// (analysis/frik-flat-port.md section 2.1). Only the subset FRIK and the framework read is given a real node; the rest
// stay null, which every FRIK user of them already checks (see the change log for the ones that do not).
#include "vrshim/VrNodes.h"
#include "vrshim/Bridge.h"

#include "fo4flatvr/FrikBridgeApi.h"

#include <Windows.h>

namespace vrshim
{
    namespace
    {
        RE::VRPlayerNodes g_nodes{};
        bool              g_created = false;

        // C33 (user 15 Nov: hand/wrist orientation follows the player's facing): the flat CommonLibF4 NiAVObject constructor
        // sets local/world/previousWorld with NiTransform::MakeIdentity, which copies NiMatrix3::IDENTITY - defined there as
        // { IDENTITY0, IDENTITY0, IDENTITY0 }: every row (1,0,0), a rank-1 matrix (extern/commonlibf4 NiMatrix3.h). A node made
        // here kept that rotation unless the bridge overwrote it, so PrimaryWeaponToWand/Kickback/PrimaryWeaponOffset (and the
        // SecondaryMeleeWeaponOffset copy) collapsed every axis of FRIK's hand target onto the wand's X axis; FRIK's quaternion
        // damping then turned that into a rotation depending on where that axis points (world heading, physical facing).
        // A real identity here (NiMatrix3::MakeIdentity is correct: IDENTITY0/1/2).
        void properIdentity(RE::NiAVObject* a_node)
        {
            // off switch: fo4flatvr.ini [FRIK] bFixNodeIdentity=0 (the library's rank-1 "identity" as before)
            static const bool s_on = [] {
                const auto* api = vrshim::bridge::api();
                const char* ini = api && api->IniPath ? api->IniPath() : nullptr;
                const bool  on = !ini || ::GetPrivateProfileIntA("FRIK", "bFixNodeIdentity", 1, ini) != 0;
                logger::info("FRIK-flat C33: VR stand-in nodes get a proper identity rotation: {} ([FRIK] bFixNodeIdentity)", on ? "on" : "OFF");
                return on;
            }();
            if (!s_on) return;
            for (auto* t : { &a_node->local, &a_node->world, &a_node->previousWorld }) {
                t->rotate.MakeIdentity();
                t->translate = {};
                t->scale = 1.0f;
            }
        }

        RE::NiNode* makeNode(const char* a_name, RE::NiNode* a_parent, const RE::NiPoint3& a_translate = {})
        {
            auto* node = new RE::NiNode(0);
            properIdentity(node);
            node->name = a_name;
            node->local.translate = a_translate;
            if (a_parent) {
                a_parent->AttachChild(node, true);
            }
            return node;
        }
    }

    RE::VRPlayerNodes* getVRPlayerNodes() noexcept
    {
        return &g_nodes;
    }

    bool vrNodesCreated() noexcept
    {
        return g_created;
    }

    void createVrNodes()
    {
        if (g_created) {
            return;
        }
        auto& n = g_nodes;
        // Hierarchy as in FO4VR (VR node setup 0xEF21DD); translations are the VR binds FRIK depends on.
        n.playerWorldNode = makeNode("PlayerWorldNode", nullptr);
        n.roomNode = makeNode("RoomNode", n.playerWorldNode);
        n.hmdNode = makeNode("HMDNode", n.roomNode);
        n.offscreenHmdNode = makeNode("OffscreenHmdNode", n.roomNode);
        n.uprightHmdNode = makeNode("UprightHmdNode", n.roomNode);
        n.uprightHmdLagNode = makeNode("UprightHmdLagNode", n.roomNode);
        n.headLightParentNode = makeNode("HeadLightParent", n.hmdNode);

        n.primaryWandNode = makeNode("PrimaryWandNode", n.roomNode);
        n.primaryUIAttachNode = makeNode("PrimaryUIAttachNode", n.primaryWandNode);
        // FRIK-flat: FO4VR loads the wand mesh "world_primaryWand.nif" under PrimaryUIAttachNode; FRIK places its back-of-hand
        // UI through that node unchecked (f4vr::getPrimaryWandNode). Flat has no wand mesh: an empty node with the name stands in.
        makeNode("world_primaryWand.nif", n.primaryUIAttachNode);
        n.primaryWeaponToWandNode = makeNode("PrimaryWeaponToWandNode", n.primaryWandNode, { 0.0f, -4.0f, 1.0f });
        n.primaryWeaponKickbackRecoilNode = makeNode("PrimaryWeaponKickbackRecoilNode", n.primaryWeaponToWandNode);
        n.primaryWeaponOffsetNode = makeNode("PrimaryWeaponOffsetNode", n.primaryWeaponKickbackRecoilNode);
        n.primaryMeleeWeaponOffsetNode = makeNode("PrimaryMeleeWeaponOffsetNode", n.primaryWandNode);
        n.primaryUnarmedPowerArmorWeaponOffsetNode = makeNode("PrimaryUnarmedPowerArmorWeaponOffsetNode", n.primaryWandNode);
        n.scopeParentNode = makeNode("ScopeParent", n.primaryWeaponOffsetNode);
        // FRIK-flat: FRIK's weapon adjuster moves the VR scope camera unchecked; on flat nothing renders through it, so an
        // NiNode stands in (FRIK touches only NiAVObject members of it: local, world, parent)
        n.primaryWeaponScopeCamera = reinterpret_cast<RE::NiCamera*>(makeNode("PrimaryWeaponScopeCamera", n.scopeParentNode));

        n.secondaryWandNode = makeNode("SecondaryWandNode", n.roomNode);
        n.secondaryUIOffsetNode = makeNode("SecondaryUIOffsetNode", n.secondaryWandNode);
        n.secondaryMeleeWeaponOffsetNode = makeNode("SecondaryMeleeWeaponOffsetNode", n.secondaryWandNode);
        n.secondaryUnarmedPowerArmorWeaponOffsetNode = makeNode("SecondaryUnarmedPowerArmorWeaponOffsetNode", n.secondaryWandNode);
        n.secondaryAimNode = makeNode("SecondaryAimNode", n.secondaryWandNode);
        n.pipboyParentNode = makeNode("PipboyParent", n.secondaryWandNode);
        n.pipboyRootNIFOnlyNode = makeNode("PipboyRoot_NIF_ONLY", n.pipboyParentNode);
        n.screenNode = makeNode("Screen", n.pipboyRootNIFOnlyNode);
        n.pipboyLightParentNode = makeNode("PipboyLightParent", n.pipboyParentNode);
        // weaponLeftNode is the flat first-person skeleton's own "WeaponLeft" (under LArm_Hand): set by updateVrNodes.
        // primaryWeaponScopeCamera, workshop, compass, lock-pick, laser pointer: null (no flat counterpart yet).
        n.primaryMeleeWeaponOffsetNodeInUse = n.primaryMeleeWeaponOffsetNode;
        n.secondaryMeleeWeaponOffsetNodeInUse = n.secondaryMeleeWeaponOffsetNode;
        g_created = true;
    }

    void updateVrNodes(const VrFramePoses& a_poses)
    {
        if (!g_created) {
            return;
        }
        auto& n = g_nodes;
        n.playerWorldNode->local = a_poses.playerWorld;
        n.roomNode->local = a_poses.roomLocal;
        if (a_poses.hmdValid) {
            n.hmdNode->local = a_poses.hmdInRoom;
            n.offscreenHmdNode->local = a_poses.hmdInRoom;
            // upright: the HMD with its yaw only
            n.uprightHmdNode->local = a_poses.hmdUprightInRoom;
            n.uprightHmdLagNode->local = a_poses.hmdUprightInRoom;
        }
        if (a_poses.primaryValid) {
            n.primaryWandNode->local = a_poses.primaryInRoom;
        }
        if (a_poses.secondaryValid) {
            n.secondaryWandNode->local = a_poses.secondaryInRoom;
        }
        n.weaponLeftNode = a_poses.firstPersonWeaponLeft;
        RE::NiUpdateData ud{};
        niAVObjectUpdate(n.playerWorldNode, ud);
    }
}

namespace vrshim
{
    namespace
    {
        FpHands       g_fp{};
        std::uint64_t g_fpGen = 0;
        RE::NiNode*   g_standR = nullptr;
        RE::NiNode*   g_standL = nullptr;
        RE::NiNode*   g_standW = nullptr;
        RE::NiNode*   g_standWL = nullptr;

        RE::NiNode* find(RE::NiAVObject* a_obj, const char* a_name, int a_depth = 64)
        {
            if (!a_obj) return nullptr;
            auto* node = a_obj->IsNode();
            if (!node) return nullptr;
            if (_stricmp(a_name, node->name.c_str()) == 0) return node;
            if (a_depth < 1) return nullptr;
            for (auto& child : node->children) {
                if (auto* r = child ? find(child.get(), a_name, a_depth - 1) : nullptr) return r;
            }
            return nullptr;
        }

        RE::NiNode* standIn(const char* a_name, RE::NiNode* a_parent)
        {
            auto* node = new RE::NiNode(0);
            properIdentity(node);  // C33
            node->name = a_name;
            if (a_parent) {
                a_parent->AttachChild(node, true);
            } else {
                node->IncRefCount();  // detached root: kept alive for the whole session
            }
            return node;
        }
    }

    void selectFpHands()
    {
        auto*   pc = RE::PlayerCharacter::GetSingleton();
        auto*   fp = pc ? pc->firstPerson3D.get() : nullptr;
        FpHands real{};
        real.rHand = find(fp, "RArm_Hand");
        real.lHand = find(fp, "LArm_Hand");
        real.weapon = find(fp, "Weapon");
        real.weaponLeft = find(fp, "WeaponLeft");
        real.standIn = false;
        if (!g_standR) {
            g_standR = standIn("RArm_Hand", nullptr);
            g_standL = standIn("LArm_Hand", nullptr);
            g_standW = standIn("Weapon", g_standR);
            g_standWL = standIn("WeaponLeft", g_standL);
        }
        // per side: the real hand + its weapon node when both exist (the weapon node must hang under that hand), else the stand-in pair
        // C21 (15 Nov 14:14, measured): flat's first-person rig is a BSFlattenedBoneTree whose bones are not reachable as an
        // NiNode chain (rHand/Weapon not found by name even with the 10mm drawn) and lives in player-relative space at this
        // point of the frame, so FO4VR's arm placement (VR EF6280) has nothing to act on. The stand-ins are used always: FRIK's
        // hand targets = its PrimaryWeaponOffsetNode . glue^-1 in absolute world, and the drawn weapon stays with fo4flatvr's
        // floating gun (same FRIK glue + per-weapon JSONs). kUseRealFpNodes keeps the real-node path for a future rig port.
        constexpr bool kUseRealFpNodes = false;
        FpHands    next{};
        const bool realR = kUseRealFpNodes && real.rHand && real.weapon;
        const bool realL = kUseRealFpNodes && real.lHand && real.weaponLeft;
        next.rHand = realR ? real.rHand : g_standR;
        next.weapon = realR ? real.weapon : g_standW;
        next.lHand = realL ? real.lHand : g_standL;
        next.weaponLeft = realL ? real.weaponLeft : g_standWL;
        next.standIn = !realR || !realL;
        if (next.rHand != g_fp.rHand || next.lHand != g_fp.lHand || next.weapon != g_fp.weapon || next.weaponLeft != g_fp.weaponLeft) {
            ++g_fpGen;
            logger::info("FRIK-flat: first-person hands -> right {}, left {} (generation {}; real: rHand={} lHand={} Weapon={} WeaponLeft={})",
                next.rHand == g_standR ? "stand-in" : "real", next.lHand == g_standL ? "stand-in" : "real", g_fpGen, real.rHand != nullptr,
                real.lHand != nullptr, real.weapon != nullptr, real.weaponLeft != nullptr);
        }
        g_fp = next;
    }

    const FpHands& fpHands() noexcept { return g_fp; }
    std::uint64_t  fpHandsGeneration() noexcept { return g_fpGen; }
    bool           isStandInWeapon(const RE::NiNode* a_node) noexcept { return a_node && (a_node == g_standW || a_node == g_standWL); }
}
