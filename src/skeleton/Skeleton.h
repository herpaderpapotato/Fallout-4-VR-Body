#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "CullGeometryHandler.h"
#include "HandPose.h"
#include "SelfieHandler.h"
#include "WeaponHandRecoil.h"
#include "f4vr/PlayerNodes.h"

namespace frik
{
    struct ArmNodes
    {
        RE::NiAVObject* shoulder = nullptr;
        RE::NiAVObject* upper = nullptr;
        RE::NiAVObject* upperT1 = nullptr;
        RE::NiAVObject* forearm1 = nullptr;
        RE::NiAVObject* forearm2 = nullptr;
        RE::NiAVObject* forearm3 = nullptr;
        RE::NiAVObject* hand = nullptr;
    };

    class Skeleton
    {
    public:
        /**
         * Create a fully initialized skeleton, or nullptr if the game nodes required by the skeleton
         * are not available yet. Guarantees that an existing Skeleton instance is always usable.
         */
        static std::unique_ptr<Skeleton> create(RE::NiNode* rootNode, bool inPowerArmor);

        ArmNodes getLeftArm() const
        {
            return _leftArm;
        }

        ArmNodes getRightArm() const
        {
            return _rightArm;
        }

        ArmNodes getArm(const bool isLeft) const
        {
            return isLeft ? _leftArm : _rightArm;
        }

        RE::NiNode* getWandNode(const bool isLeft) const
        {
            return isLeft ? f4vr::getLeftHandNode() : f4vr::getRightHandNode();
        }

        /**
         * The weapon offset node FRIK dampens and drives the first-person arm from for this hand.
         */
        RE::NiNode* getWeaponOffsetNode(const bool isLeft) const
        {
            return (RE::VRSettings::IsLeftHandedMode() ^ isLeft) ? _playerNodes->secondaryMeleeWeaponOffsetNode : _playerNodes->primaryWeaponOffsetNode;
        }

        /**
         * The first-person hand node FRIK solves the body arm to (before any external hand transform).
         */
        RE::NiNode* getFirstPersonHandNode(const bool isLeft) const
        {
            return isLeft ? _leftHand : _rightHand;
        }

        /**
         * Re-pair one of the engine's per-frame first-person arm placements during an external left carry. The engine moves whichever arm
         * the weapon node really hangs under, and handleLeftHandedWeaponNodesSwitch hung the nodes under the other hands. Returns true when
         * the placement was done here (with the other hand's offset and FRIK's glue) instead of by the engine's call.
         */
        bool repairEngineArmPlacementForCarry(RE::NiNode* weapon) const;

        bool getBoneWorldTransform(std::string_view boneName, RE::NiTransform& outTransform) const;

        enum class HandSolveState : std::uint8_t
        {
            // no external hand transform was set; solved to the tracked hand
            NoClaim,
            // solved to the external hand transform
            Consumed,
            // the external target was out of reach; solved to the tracked hand instead
            Unreachable,
        };

        /**
         * How this hand was solved this frame and the wrist world transform rendered for it (latched after world final).
         */
        void getHandSolveResult(const bool isLeft, HandSolveState& outState, RE::NiTransform& outWrist) const
        {
            outState = _handSolveState[isLeft ? 0 : 1];
            outWrist = _renderedWrist[isLeft ? 0 : 1];
        }

        // Called once the frame's world transforms are final
        void latchRenderedWrists();

        static float getAdjustedPlayerHMDOffset();

        // The face and skin geometries to hide are found again on the next frame, for a change of what to hide
        void resetHideGeometry()
        {
            _cullGeometry.resetHideGeometryIndexes();
        }

#ifdef FRIK_FLAT // C35
        // the view is not at the player's head any more (camera state other than first person / iron sights): the head gear,
        // face and skin geometry this skeleton's cull hid are shown again (FRIK culls them only for the first-person view)
        void restoreCulledGeometry()
        {
            _cullGeometry.restoreAll();
        }
        const CullGeometryHandler* cullGeometry() const { return &_cullGeometry; }  // C45 log: is a culled head in C39's set
#endif

        void onFrameUpdate();

#ifdef FRIK_FLAT // C50
        // bridge v12: the floating gun's kick for this frame, before it places the weapon (WeaponHandRecoil::preResolveFlatGunKick)
        bool filterFlatGunKick(const bool physicalPrimaryIsLeft, const RE::NiTransform& gunKick, RE::NiTransform& outGunKick)
        {
            return _weaponHandRecoil.preResolveFlatGunKick(_playerNodes, physicalPrimaryIsLeft, gunKick, outGunKick);
        }
#endif

#ifdef FRIK_FLAT // C36
        // the body height FRIK fits the skeleton to (PlayerHeight): FRIK.toml's, or with [FRIK] bAutoBodyHeight=1 the camera's own
        // standing height (see Skeleton.cpp updateFlatBodyHeight)
        float playerHeightFlat() const;
#endif

    private:
        Skeleton(RE::NiNode* rootNode, const bool inPowerArmor)
            : _root(rootNode),
              _inPowerArmor(inPowerArmor),
              _handPose(inPowerArmor)
        {
            _curentPosition = RE::NiPoint3(0, 0, 0);
            _walkingState = 0;
        }

        // initialization
        bool initializeNodes();
        void initArmsNodes();
        void initSkeletonNodesDefaults();
        void setBodyLen();

        // on frame update - skeleton update
        void setTime();
        void restoreNodesToDefault();
        void setupHead(float neckYaw, float neckPitch) const;
        void setBodyUnderHMD(float neckYaw, float neckPitch);
        void setBodyPosture(float neckPitch);
        void setKneePos();
        void walk();
        void setSingleLeg(bool isLeft) const;
        void handleLeftHandedWeaponNodesSwitch();
        void updateHandTarget(bool isLeft);
        void placeFirstPersonArm(RE::NiNode* weaponNode, RE::NiNode* offsetNode, bool isLeft, bool handleOffhand) const;
        void solveArm(bool isLeft);
        void restoreArmNodesToDefault(bool isLeft);
        void applyUpperTwist();
        bool solveArmToHandWorldTarget(bool isLeft, const RE::NiTransform& handWorldTarget);
        void dampenHand(RE::NiNode* node, bool isLeft);
        void hide3rdPersonWeapon() const;
        void hideFistHelpers() const;
        void showHidePAHud() const;
        void fixArmor() const;

        // Utils - Body Positioning
        float getNeckYaw() const;
        float getNeckPitch() const;
        float getBodyPitch(float neckPitch) const;
        void rotateLeg(uint32_t pos, float angle) const;

        // root node and is in power armor define the Skeleton instance
        RE::NiNode* _root;
        bool _inPowerArmor;

        // ???
        LARGE_INTEGER _freqCounter;
        LARGE_INTEGER _timer;
        LARGE_INTEGER _prevTime;
        float _frameTime;

        // handle switch of hands for left-handed mode
        bool _lastLeftHandedModeSwitch = false;

        // Camera positions
        RE::NiPoint3 _curentPosition;
        RE::NiPoint3 _lastPosition;
        inline static float _comfortSneakCameraOffsetAdjustment = -1.0f;

        // ???
        RE::NiPoint3 _forwardDir;
        RE::NiPoint3 _sidewaysRDir;
        RE::NiPoint3 _upDir;

        // skeleton nodes
        RE::VRPlayerNodes* _playerNodes;
        RE::NiNode* _rightHand;
        RE::NiNode* _leftHand;
        RE::NiNode* _head;
        RE::NiNode* _spine;
        RE::NiNode* _chest;
        float _torsoLen;
        float _legLen;
#ifdef FRIK_FLAT // C36
        void updateFlatBodyHeight();
        float _flatHeight = 0.0f;        // low-passed standing camera height (FRIK units), 0 = not measured yet / off
        float _flatHeightLogged = 0.0f;
        float _flatHeightPrevH = -1.0f;  // last frame's camera height (any state), < 0 = none: a one-frame step re-seeds (recentre)
        bool  _flatHeightReseed = false;
        // C38: the 3D root's world z as the engine left it (FRIK adds/subtracts offsets to it every frame; see setBodyUnderHMD)
        float _flatBodyEngineZ = 0.0f;
        float _flatBodyWrittenZ = 0.0f;
        bool  _flatBodyZValid = false;
        void  flatBodyEngineZ(RE::NiNode* body);
        void  flatBodyWrittenZ();
        // C39: FRIK's leg IK with skeleton-mod offset bones between thigh/calf/foot (setSingleLeg)
        void setSingleLegChain(bool isLeft) const;
        // C42: VRIK-style legs (Skeleton.cpp "C42"): the engine's locomotion animation drives the feet while moving, world-planted
        // feet with short procedural steps while standing; FRIK's IK solves the legs to them. [FRIK] bAnimatedLegs (live)
        static bool flatAnimatedLegsOn();
        bool        flatCaptureAnimLegs();
        void        flatAnimatedLegs();
        void        flatAnimLegsDiag();
        void        flatStartStep(int a_foot, const std::array<RE::NiPoint3, 2>& a_tgt, float a_scale);
        bool                           _flatAnimLegs = false;  // this frame's legs come from flatAnimatedLegs
        bool                           _flatAnimLegsPrev = false;  // ... and the previous frame's (a switch starts fresh)
        const void*                    _flatLegIdxTree = nullptr;
        std::array<int, 4>             _flatLegIdx = { -1, -1, -1, -1 };  // tree index: L thigh, L foot, R thigh, R foot
        std::array<RE::NiTransform, 4> _flatAnimRel{};                     // their animated pose in the actor (tree node) frame
        RE::NiTransform                _flatPrevEng[3]{};                  // diag: last frame's engine local of L foot, L thigh, COM
        std::array<RE::NiPoint3, 2>    _flatPole{};                        // knee pole (animated thigh axis), world
        std::array<RE::NiMatrix3, 2>   _flatFootRot{};                     // foot world rotation to keep after the solve
        bool                           _flatFootRotValid = false;
        // standing: world-planted feet and the step in progress
        bool                         _flatPlantedValid = false;
        std::array<RE::NiPoint3, 2>  _flatPlanted{};
        std::array<RE::NiMatrix3, 2> _flatPlantedRot{};
        int                          _flatStepFoot = -1;
        float                        _flatStepT = 0.0f, _flatStepDur = 0.3f, _flatStepLift = 1.0f;
        RE::NiPoint3                 _flatStepStart{};
        RE::NiMatrix3                _flatStepStartRot{};
        float                        _flatLegYaw = 0.0f;  // heading of the step targets (dead zone around the body heading)
        bool                         _flatLegYawValid = false, _flatLegYawTurning = false;
        RE::NiPoint3                 _flatAnchor{};       // standing: root x/y the step targets hang from (lean dead band)
        bool                         _flatAnchorRecentring = false;
        bool                         _flatMoving = false;
        float                        _flatMoveHold = 0.0f;   // seconds the animated feet stay after the movement stops
        float                        _flatAnimBlend = 1.0f;  // planted -> animated blend at a movement start
        float                        _flatStrideYaw = 0.0f;  // C43: holstered strafe / back - stride turned to the move direction (rad)
        std::array<RE::NiPoint3, 2>  _flatOut{};             // this frame's foot targets (world)
        std::array<RE::NiMatrix3, 2> _flatOutRot{};
        // C42 diag window (FRIK.log `FRIK-flat C42 legs:` every 2 s)
        struct FlatLegDiag
        {
            int   frames = 0, moving = 0, steps = 0, plantedFrames = 0;
            float footErrMax[2] = { 0, 0 };     // |solved foot - target| after the IK
            float plantSlideMax = 0.0f;         // planted position change per frame (not stepping)
            float plantFootMoveMax = 0.0f;      // solved planted foot world move per frame (not stepping)
            float animMin[2][2] = { { 1e9f, 1e9f }, { 1e9f, 1e9f } }, animMax[2][2] = { { -1e9f, -1e9f }, { -1e9f, -1e9f } };  // standing: animated foot x/y range (actor frame)
            float capErr = -1.0f;               // composed engine foot vs the frame-start tree world (tree-relative)
            float mvMin[3] = { 1e9f, 1e9f, 1e9f }, mvMax[3] = { -1e9f, -1e9f, -1e9f };  // moving: animated L foot x/y/z range (actor frame)
            int   engChanged = 0, engFrames = 0;  // frames where the engine's L foot / L thigh / COM local changed since the previous frame
            int   thighChanged = 0, comChanged = 0;
            float yawErrMax = 0.0f;
            float strideYawMax = 0.0f;  // C43: largest holstered stride turn this window (rad)
            std::uint32_t moveBits = 0, moveBitsOr = 0;
        } _flatLegDiag;
        std::array<RE::NiPoint3, 2> _flatPrevSolvedFoot{};
        std::array<bool, 2>         _flatPrevPlanted = { false, false };
#endif
        ArmNodes _rightArm;
        ArmNodes _leftArm;
        // flattened bone tree index by bone name, for API bone reads
        std::unordered_map<std::string, int> _boneIndexByName;

        // Default transform are used to reset the skeleton before each frame update to start from scratch
        std::vector<std::pair<RE::NiAVObject*, const RE::NiTransform>> _skeletonNodesToDefaultTransforms;
        static std::unordered_map<std::string, RE::NiTransform> getSkeletonNodesDefaultTransforms();
        static std::unordered_map<std::string, RE::NiTransform> getSkeletonNodesDefaultTransformsInPA();
        inline static const std::unordered_map<std::string, RE::NiTransform> _skeletonNodesDefaultTransform = getSkeletonNodesDefaultTransforms();
        inline static const std::unordered_map<std::string, RE::NiTransform> _skeletonNodesDefaultTransformInPA = getSkeletonNodesDefaultTransformsInPA();

        // legs walking stuff
        int _walkingState;
        float _currentStepTime;
        RE::NiPoint3 _leftFootPos;
        RE::NiPoint3 _rightFootPos;
        RE::NiPoint3 _rightFootTarget;
        RE::NiPoint3 _leftFootTarget;
        RE::NiPoint3 _rightFootStart;
        RE::NiPoint3 _leftFootStart;
        RE::NiPoint3 _leftKneePosture;
        RE::NiPoint3 _rightKneePosture;
        RE::NiPoint3 _leftKneePos;
        RE::NiPoint3 _rightKneePos;
        int _footStepping;
        RE::NiPoint3 _stepDir;
        float _prevSpeed;
        float _stepTimeinStep;
        int _delayFrame;

        RE::NiTransform _rightHandPrevFrame;
        RE::NiTransform _leftHandPrevFrame;
        // the damping anchors only mean something once they hold a damped node's own world; until then the first frame snaps
        bool _rightHandDampenSeeded = false;
        bool _leftHandDampenSeeded = false;

        // last frame's looking-through state, to log the damping pair only when it switches
        bool _dampenHandsInScopePrevFrame = false;

        // elbow twist smoothing: committed once per frame so a re-solve in the same frame gives the same answer
        std::array<float, 2> _twistAnglePrevFrame = { 0, 0 };
        std::array<float, 2> _twistAngleThisFrame = { 0, 0 };
        std::array<HandSolveState, 2> _handSolveState = { HandSolveState::NoClaim, HandSolveState::NoClaim };
        // upper-arm twist bones live only in the flattened bone tree (no scene-graph node); index, rest local rotation, this frame's roll (left first, like the twist angles)
        std::array<int, 2> _upperTwistTreeIdx = { -1, -1 };
        std::array<RE::NiMatrix3, 2> _upperTwistRestRotate{};
        std::array<float, 2> _upperTwistRoll = { 0, 0 };
        std::array<RE::NiTransform, 2> _renderedWrist = {};

        WeaponHandRecoil _weaponHandRecoil;

        HandPose _handPose;

        // cull (hide) parts of the skeleton (head, equipment)
        CullGeometryHandler _cullGeometry;

        SelfieHandler _selfieHandler;
    };
}
