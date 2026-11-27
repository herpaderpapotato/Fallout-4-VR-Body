#include "Skeleton.h"
#include "vrshim/EngineCull.h"  // FRIK-flat 22 Nov: engine cull

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "Config.h"
#include "ExternalAuthority.h"
#include "FRIK.h"
#include "api/ApiCore.h"
#include "common/MatrixUtils.h"
#include "common/Quaternion.h"
#include "f4vr/F4VRSkelly.h"
#include "f4vr/F4VRUtils.h"
#include "perf/Perf.h"
#include "skeleton/QuaternionSmoothing.h"
#include "utils.h"
#ifdef FRIK_FLAT
#include "vrshim/Bridge.h" // C36
#include "BodyGraph.h"      // C43
#include "vrshim/XfMath.h" // C42
#endif

using namespace common;
using namespace f4vr;
using namespace vrcf;

namespace
{
    /**
     * Hack to handle comfort sneak affecting the height of the player without real-world body change.
     * By setting static body pitch the body position doesn't change, making it easier to handle skeleton
     * related things like Virtual Holsters.
     */
    bool isComfortSneakHackEnabled()
    {
        return frik::g_config.comfortSneakHackStaticBodyPitchAngle > 0 && RE::VRSettings::IsComfortSneak() && isPlayerSneaking();
    }
}

namespace frik
{
    /**
     * Create a fully initialized skeleton, or nullptr if the game nodes required by the skeleton
     * are not available yet. Guarantees that an existing Skeleton instance is always usable.
     */
    std::unique_ptr<Skeleton> Skeleton::create(RE::NiNode* rootNode, const bool inPowerArmor)
    {
        // not using make_unique as the constructor is private
        std::unique_ptr<Skeleton> skeleton(new Skeleton(rootNode, inPowerArmor));
        return skeleton->initializeNodes() ? std::move(skeleton) : nullptr;
    }

    /**
     * Get the player camera height offset adjusted for power armor, sneaking, and dynamic height from external API.
     * The height needs to be adjusted for comfort sneaking because the player physical height doesn't change but
     * the player avatar does. So the camera offset has to be reduced by the same amount as the game changes the height
     * which is 70% of the normal height.
     */
    float Skeleton::getAdjustedPlayerHMDOffset()
    {
        auto offset = g_config.getPlayerHMDOffsetUp() + g_frik.getDynamicCameraHeight();
        if (RE::VRSettings::IsComfortSneak() && isPlayerSneaking()) {
            offset *= _comfortSneakCameraOffsetAdjustment;
        }
        return offset;
    }

    /**
     * Initialize all the skeleton nodes for quick access during frame update.
     * Setup known defaults where relevant.
     */
    bool Skeleton::initializeNodes()
    {
        QueryPerformanceFrequency(&_freqCounter);
        QueryPerformanceCounter(&_timer);

        _prevSpeed = 0.0;

        _playerNodes = getVRPlayerNodes();
        if (!_playerNodes || !_root) {
            logger::warn("Skeleton initialization failed: missing player nodes or root. playerNodes={} root={}",
                static_cast<const void*>(_playerNodes),
                static_cast<const void*>(_root));
            return false;
        }

        const auto fpSkeleton = getFirstPersonSkeleton();
        if (!fpSkeleton) {
            logger::warn("Skeleton initialization failed: missing first-person skeleton.");
            return false;
        }

#ifdef FRIK_FLAT // C19: real first-person hands or stand-ins (vrshim::selectFpHands)
        _rightHand = vrshim::fpHands().rHand;
        _leftHand = vrshim::fpHands().lHand;
#else
        _rightHand = RE::NiNodeUtils::FindNode(fpSkeleton, "RArm_Hand");
        _leftHand = RE::NiNodeUtils::FindNode(fpSkeleton, "LArm_Hand");
#endif
        if (!_rightHand || !_leftHand) {
            logger::warn("Skeleton initialization failed: missing first-person hand nodes. right={} left={}",
                static_cast<const void*>(_rightHand),
                static_cast<const void*>(_leftHand));
            return false;
        }
        // NOT an anchor for hand damping: at build the first-person hands still sit in their unplaced player-relative pose (the
        // engine's arm placement has not run for this skeleton yet), so damping from here would drag the hand targets in from
        // ~100k units over tens of frames. dampenHand snaps on its first frame instead.
        _rightHandPrevFrame = _rightHand->world;
        _leftHandPrevFrame = _leftHand->world;
        _rightHandDampenSeeded = false;
        _leftHandDampenSeeded = false;

        _head = RE::NiNodeUtils::FindNode(_root, "Head");
        _spine = RE::NiNodeUtils::FindNode(_root, "SPINE2");
        _chest = RE::NiNodeUtils::FindNode(_root, "Chest");
        if (!_head || !_spine || !_chest) {
            logger::warn("Skeleton initialization failed: missing body nodes. head={} spine={} chest={}",
                static_cast<const void*>(_head),
                static_cast<const void*>(_spine),
                static_cast<const void*>(_chest));
            return false;
        }

        // Setup Arms
        initArmsNodes();

        initSkeletonNodesDefaults();

        Skelly::initBoneTreeMap();

        _boneIndexByName.clear();
        if (const auto* tree = f4vr::getFlattenedBoneTree()) {
            for (int i = 0; i < tree->numTransforms; ++i) {
                _boneIndexByName.emplace(tree->transforms[i].name.c_str(), i);
            }
            // the upper-arm twist bones have no scene-graph node; they are driven through the tree from their rest local
            for (const bool isLeft : { true, false }) {
                const auto it = _boneIndexByName.find(isLeft ? "LArm_UpperTwist1" : "RArm_UpperTwist1");
                const auto side = isLeft ? 0 : 1;
                _upperTwistTreeIdx[side] = it != _boneIndexByName.end() ? it->second : -1;
                if (_upperTwistTreeIdx[side] >= 0) {
                    // the entry's local at skeleton build is the rest the split rotates from; refuse a degenerate one (a zero or
                    // non-orthonormal matrix would put a broken rotation on the twist chain every frame)
                    const auto& rest = tree->transforms[_upperTwistTreeIdx[side]].local.rotate;
                    const RE::NiPoint3 c0(rest.entry[0][0], rest.entry[1][0], rest.entry[2][0]);
                    const RE::NiPoint3 c1(rest.entry[0][1], rest.entry[1][1], rest.entry[2][1]);
                    const RE::NiPoint3 c2(rest.entry[0][2], rest.entry[1][2], rest.entry[2][2]);
                    const float det = MatrixUtils::vec3Dot(c0, MatrixUtils::vec3Cross(c1, c2));
                    const bool valid = std::isfinite(det) && std::abs(det - 1.0f) < 0.05f;
                    _upperTwistRestRotate[side] = valid ? rest : MatrixUtils::getIdentityMatrix();
                    logger::info("UpperTwist1 {} tree entry {} rest det {:.3f} ({})",
                        isLeft ? "L" : "R",
                        _upperTwistTreeIdx[side],
                        det,
                        valid ? "used" : "degenerate, identity used");
                    if (!valid) {
                        _upperTwistTreeIdx[side] = -1;
                    }
                }
            }
        }

        setBodyLen();
#ifdef FRIK_FLAT // 20 Nov no-scaling switch: the engine's own root scale, before FRIK writes it this session (log once per session)
        {
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                logger::info("FRIK-flat: skeleton root '{}' local scale {:.3f} before FRIK's first write (engine value); no-scaling {} ([General] bNoScaling)",
                    _root->name.c_str() ? _root->name.c_str() : "", _root->local.scale, vrshim::bridge::noScaling() ? "ON (FRIK writes 1.000)" : "off");
            }
        }
#endif

        _comfortSneakCameraOffsetAdjustment = getIniSetting("fComfortSneakHeight:VR")->GetFloat();
        return true;
    }

    void Skeleton::initArmsNodes()
    {
        const std::vector<std::pair<RE::BSFixedString, RE::NiAVObject**>> armNodes = { { "RArm_Collarbone", &_rightArm.shoulder },
            { "RArm_UpperArm", &_rightArm.upper },
            { "RArm_UpperTwist1", &_rightArm.upperT1 },
            { "RArm_ForeArm1", &_rightArm.forearm1 },
            { "RArm_ForeArm2", &_rightArm.forearm2 },
            { "RArm_ForeArm3", &_rightArm.forearm3 },
            { "RArm_Hand", &_rightArm.hand },
            { "LArm_Collarbone", &_leftArm.shoulder },
            { "LArm_UpperArm", &_leftArm.upper },
            { "LArm_UpperTwist1", &_leftArm.upperT1 },
            { "LArm_ForeArm1", &_leftArm.forearm1 },
            { "LArm_ForeArm2", &_leftArm.forearm2 },
            { "LArm_ForeArm3", &_leftArm.forearm3 },
            { "LArm_Hand", &_leftArm.hand } };
        const auto commonNode = getCommonNode();
        for (const auto& [name, node] : armNodes) {
            *node = RE::NiNodeUtils::FindObject(commonNode, name.c_str());
        }
    }

    /**
     * Setup default skeleton nodes collection for quick reset on every frame
     * instead of looking up the skeleton nodes every time.
     */
    void Skeleton::initSkeletonNodesDefaults()
    {
        const auto defaultBonesMap = _inPowerArmor ? _skeletonNodesDefaultTransformInPA : _skeletonNodesDefaultTransform;
        for (const auto& [boneName, defaultTransform] : defaultBonesMap) {
            if (auto node = RE::NiNodeUtils::FindObject(_root, boneName)) {
                auto transform = node->local; // use node transform to keep scale
                transform.translate = defaultTransform.translate;
                transform.rotate = defaultTransform.rotate;
                _skeletonNodesToDefaultTransforms.emplace_back(node, transform);
            } else {
                logger::warn("Skeleton bone node not found for '{}'", boneName.c_str());
            }
        }

        // The upper-arm twist bones have no authored entry; hold them at the local they were built with, so a solver
        // that writes them starts from the same pose every frame instead of accumulating
        for (auto* twist : { _rightArm.upperT1, _leftArm.upperT1 }) {
            if (twist) {
                _skeletonNodesToDefaultTransforms.emplace_back(twist, twist->local);
            }
        }
    }

    void Skeleton::setBodyLen()
    {
        _torsoLen = MatrixUtils::vec3Len(RE::NiNodeUtils::FindNode(_root, "Camera")->world.translate - RE::NiNodeUtils::FindNode(_root, "COM")->world.translate);
#ifdef FRIK_FLAT // C36
        _torsoLen *= playerHeightFlat() / DEFAULT_CAMERA_HEIGHT;
#else
        _torsoLen *= g_config.playerHeight / DEFAULT_CAMERA_HEIGHT;
#endif

        _legLen = MatrixUtils::vec3Len(RE::NiNodeUtils::FindNode(_root, "LLeg_Thigh")->world.translate - RE::NiNodeUtils::FindNode(_root, "Pelvis")->world.translate);
        _legLen += MatrixUtils::vec3Len(RE::NiNodeUtils::FindNode(_root, "LLeg_Calf")->world.translate - RE::NiNodeUtils::FindNode(_root, "LLeg_Thigh")->world.translate);
        _legLen += MatrixUtils::vec3Len(RE::NiNodeUtils::FindNode(_root, "LLeg_Foot")->world.translate - RE::NiNodeUtils::FindNode(_root, "LLeg_Calf")->world.translate);
#ifdef FRIK_FLAT // C36
        _legLen *= playerHeightFlat() / DEFAULT_CAMERA_HEIGHT;
#else
        _legLen *= g_config.playerHeight / DEFAULT_CAMERA_HEIGHT;
#endif
    }

#ifdef FRIK_FLAT // C36
    /**
     * Flat (user 16 Nov: "frik's leg movements are really bad ... They move and stretch around"): FRIK fits a body of PlayerHeight
     * (FRIK.toml, default 120.48 = the flat game's standing eye height) under the camera and expects, standing upright,
     * HMD height above the room floor + HMD offset (= uprightHmdNode local z + getAdjustedPlayerHMDOffset, the quantity
     * getBodyPitch compares) to equal it. In FO4VR that is the user's real eye height, which the user calibrates PlayerHeight
     * to. On flat the camera height is the game's eye height plus fo4flatvr's head offset (fHeightOffsetCm + the head's height in
     * the reference space): the user's 16 Nov 22:07 session stood at 140-147 u (FRIK.log `FRIK-flat body:` eye - pwnZ + 8.6),
     * 20-27 u above PlayerHeight. FRIK then (getBodyPitch: |PlayerHeight - h| / PlayerHeight, absolute) pitched the torso
     * forward as if crouching and lifted the hip to 77-80 u above the root (default standing pose 68.9), so the feet could not
     * reach their floor targets: setSingleLeg's out-of-reach branch, legs locked straight and swinging after the walk targets.
     * Here PlayerHeight follows the camera's standing height (low-passed 4 s, sampled only standing on the ground: not sneaking,
     * not in the air, not in a paused frame), i.e. what calibrating PlayerHeight does in FO4VR; sneaking and physical crouching
     * still lower h against it and crouch the body as in FO4VR. Power armour keeps FRIK.toml's height (own offsets there).
     * Clamped to 0.7 .. 1.45 x FRIK.toml PlayerHeight. Off: fo4flatvr.ini [FRIK] bAutoBodyHeight=0.
     */
    float Skeleton::playerHeightFlat() const
    {
        // 20 Nov (user: "no option to disable any scaling from us, and to just use the game's default scale"): fo4flatvr.ini
        // [General] bNoScaling=1 (bridge v6, live) = the skeleton at the game's own size: root scale 1 (FRIK's height
        // DEFAULT_CAMERA_HEIGHT), no FRIK.toml PlayerHeight, no C36 calibrated height; torso / leg lengths unscaled.
        if (vrshim::bridge::noScaling()) {
            return DEFAULT_CAMERA_HEIGHT;
        }
        const float cfg = g_config.playerHeight;
        return _flatHeight > 0.0f ? std::clamp(_flatHeight, 0.7f * cfg, 1.45f * cfg) : cfg;
    }

    void Skeleton::updateFlatBodyHeight()
    {
        // 20 Nov (live settings): re-read when fo4flatvr's settings change (bridge v7; logged by iniIntLive)
        const bool s_on = vrshim::bridge::iniIntLive("bAutoBodyHeight", 1) != 0;
        if (!s_on || _inPowerArmor || !_playerNodes || !_playerNodes->uprightHmdNode) {
            _flatHeight = 0.0f;
            return;
        }
        const float h = _playerNodes->uprightHmdNode->local.translate.z + getAdjustedPlayerHMDOffset();
        // 20 Nov (user: "Ensure the auto scale only occurs on calibrate"): with fo4flatvr's recentre counter (bridge v5) the body
        // height is measured only after a recentre - F8, the recentre gesture, the pipe, or fo4flatvr's automatic first-pose
        // anchor / placeholder re-anchor - at the first standing frame 0.3 s after it (not sneaking, not in the air, not
        // paused), and then stays fixed until the next recentre: no following the camera while playing. Session-wide, so a
        // new skeleton (load, power armour exit) keeps it. The first skeleton of the session takes one sample the same way
        // (the start-up anchor happened before it existed). [FRIK] bBodyHeightOnRecentre=0 = the 16/19 Nov behaviour below
        // (continuous 4 s low-pass + re-seed on a camera step); also used with an older fo4flatvr.dll (bridge < v5).
        const bool s_onRecentre = vrshim::bridge::iniIntLive("bBodyHeightOnRecentre", 1) != 0;  // live (bridge v7)
        if (std::uint32_t seq = 0; s_onRecentre && vrshim::bridge::recentreCount(seq)) {
            static float                                 s_calHeight = 0.0f;  // calibrated standing h, 0 = none yet
            static std::uint32_t                         s_calSeq = 0;
            static bool                                  s_calSeqKnown = false, s_calPending = false;
            static std::chrono::steady_clock::time_point s_calNotBefore{};
            const auto                                   now = std::chrono::steady_clock::now();
            if (!s_calSeqKnown || seq != s_calSeq) {
                if (s_calSeqKnown) {
                    logger::info("FRIK-flat C36: recentre #{}: body height is measured at the next standing frame", seq);
                }
                s_calSeqKnown = true;
                s_calSeq = seq;
                s_calPending = true;
                s_calNotBefore = now + std::chrono::milliseconds(300);  // the recentred head pose reaches the camera first
            }
            if (s_calPending && now >= s_calNotBefore && !isPlayerSneaking() && !isJumpingOrInAir() && !vrshim::bridge::pausedFrame() &&
                std::isfinite(h) && h >= 50.0f && h <= 260.0f) {
                s_calPending = false;
                s_calHeight = h;
                logger::info("FRIK-flat C36: calibrated at recentre #{}: camera standing h {:.1f} u -> body height {:.1f} u (FRIK.toml PlayerHeight {:.1f}), "
                             "skeleton scale {:.3f}; fixed until the next recentre",
                    seq, h, std::clamp(h, 0.7f * g_config.playerHeight, 1.45f * g_config.playerHeight), g_config.playerHeight,
                    std::clamp(h, 0.7f * g_config.playerHeight, 1.45f * g_config.playerHeight) / DEFAULT_CAMERA_HEIGHT);
            }
            _flatHeight = s_calHeight;  // 0 until the first calibration: FRIK.toml's PlayerHeight
            _flatHeightPrevH = -1.0f;
            _flatHeightReseed = false;
            return;
        }
        // 19 Nov (user: "the initial height is wrong ... recalibrating with a gesture brings you to the right size"): a recentre
        // moves the camera height in one frame (user log 22:28:01: 204 -> 111 u), which the 4 s low-pass then took ~10 s to
        // follow. A step of more than 25 u between two consecutive frames (no head moves 36 cm in one frame; the game's own eye
        // height change is low-passed 0.6 s by fo4flatvr) re-seeds instead of ramping.
        if (_flatHeightPrevH >= 0.0f && std::isfinite(h) && std::abs(h - _flatHeightPrevH) > 25.0f) {
            _flatHeightReseed = true;  // taken by the next standing sample
        }
        _flatHeightPrevH = std::isfinite(h) ? h : -1.0f;
        if (isPlayerSneaking() || isJumpingOrInAir() || vrshim::bridge::pausedFrame() || !std::isfinite(h) || h < 50.0f || h > 260.0f) {
            return;
        }
        if (_flatHeight <= 0.0f || _flatHeightReseed) {
            if (_flatHeightReseed) {
                logger::info("FRIK-flat C36: camera height stepped to {:.1f} u in one frame (recentre): body height re-seeded", h);
            }
            _flatHeightReseed = false;
            _flatHeight = h;  // first standing sample after skeleton init: no ramp from the default
        } else {
            const float dt = std::clamp(_frameTime, 0.0f, 0.25f);
            _flatHeight += (h - _flatHeight) * (1.0f - std::exp(-dt / 4.0f));
        }
        const float eff = playerHeightFlat();
        if (std::abs(eff - _flatHeightLogged) > 2.0f) {
            _flatHeightLogged = eff;
            logger::info("FRIK-flat C36: body height {:.1f} u (camera standing h {:.1f}; FRIK.toml PlayerHeight {:.1f}) -> skeleton scale {:.3f}", eff, h,
                g_config.playerHeight, eff / DEFAULT_CAMERA_HEIGHT);
        }
    }
#endif

    /**
     * Runs on every game frame to calculate and update the skeleton transform.
     */
    void Skeleton::onFrameUpdate()
    {
        F4CF_PERF_FUNCTION();

        setTime();
#ifdef FRIK_FLAT // C36
        updateFlatBodyHeight();
#endif

        // save last position at this time for anyone doing speed calculations
        _lastPosition = _curentPosition;
        _curentPosition = getCameraPosition();

        logger::trace("Hide Wands...");
        setWandsVisibility(false, true);
        setWandsVisibility(false, false);

        // Each step is a perf site, so a sitting can read where the body pass spends its time (devbench perf action)
        float neckYaw, neckPitch;
        {
            F4CF_PERF_SCOPE("resetAndFlatten");
            logger::trace("Restore locals of skeleton");
            _twistAnglePrevFrame = _twistAngleThisFrame;
#ifdef FRIK_FLAT // C42: the engine's leg pose of this frame (before FRIK's rest pose replaces it)
            _flatAnimLegs = flatAnimatedLegsOn() && flatCaptureAnimLegs();
#endif
            restoreNodesToDefault();
            updateDownFromRoot();

            neckYaw = getNeckYaw();
            neckPitch = getNeckPitch();
        }

        {
            F4CF_PERF_SCOPE("bodyUnderHMD");
            if (!g_config.hideHead || (g_frik.isSelfieModeOn() && g_config.selfieIgnoreHideFlags)) {
                logger::trace("Setup Head");
                setupHead(neckYaw, neckPitch);
            }

            logger::trace("Set body under HMD");
            setBodyUnderHMD(neckYaw, neckPitch);
            updateDownFromRoot(); // Do world update now so that IK calculations have proper world reference
        }

        {
            F4CF_PERF_SCOPE("posture");
            // Now Set up body Posture and hook up the legs
            logger::trace("Set body posture...");
            setBodyPosture(neckPitch);
            updateDownFromRoot(); // Do world update now so that IK calculations have proper world reference
            api::core::invokeFramePhase(FramePhase::BodyPlaced);
        }

        {
            F4CF_PERF_SCOPE("legs");
#ifdef FRIK_FLAT // C42: VRIK-style legs replace FRIK's knee placement and invented walk cycle
            if (_flatAnimLegs) {
                flatAnimatedLegs();
            } else
#endif
            {
                logger::trace("Set knee posture...");
                setKneePos();

                logger::trace("Set walk...");
                walk();
            }
#ifdef FRIK_FLAT
            _flatAnimLegsPrev = _flatAnimLegs;
#endif

            logger::trace("Set legs...");
            setSingleLeg(false);
            setSingleLeg(true);

            // Do another update before setting arms
            updateDownFromRoot(); // Do world update now so that IK calculations have proper world reference
#ifdef FRIK_FLAT // C42 diag
            if (_flatAnimLegs) {
                flatAnimLegsDiag();
            }
#endif
#ifdef FRIK_FLAT // C39 diag: where the solved feet landed vs their targets (every 5 s)
            {
                static auto s_t = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - s_t > std::chrono::seconds(5)) {
                    s_t = std::chrono::steady_clock::now();
                    const auto lf = RE::NiNodeUtils::FindNode(_root, "LLeg_Foot");
                    const auto rf = RE::NiNodeUtils::FindNode(_root, "RLeg_Foot");
                    const auto lc = RE::NiNodeUtils::FindNode(_root, "LLeg_Calf");
                    const auto lt = RE::NiNodeUtils::FindNode(_root, "LLeg_Thigh");
                    if (lf && rf && lc && lt) {
                        logger::info("FRIK-flat C39 legs: foot - target L {:.1f} R {:.1f} u | L hip-knee {:.1f} knee-foot {:.1f} u | chain {} | walk state {} | root z {:.1f} footTgt z {:.1f}",
                            MatrixUtils::vec3Len(lf->world.translate - _leftFootPos), MatrixUtils::vec3Len(rf->world.translate - _rightFootPos),
                            MatrixUtils::vec3Len(lc->world.translate - lt->world.translate), MatrixUtils::vec3Len(lf->world.translate - lc->world.translate),
                            lc->parent != lt || lf->parent != lc ? "offset bones" : "direct", _walkingState, _root->world.translate.z, _leftFootPos.z);
                    }
                }
            }
#endif
            api::core::invokeFramePhase(FramePhase::LegsSolved);
        }

        {
            F4CF_PERF_SCOPE("arms");
            // do arm IK - Right then Left
            logger::trace("Set Arms...");
            {
                F4CF_PERF_SCOPE("handTargets");
                handleLeftHandedWeaponNodesSwitch();
                _weaponHandRecoil.onFrameUpdate(_playerNodes, g_frik.isWeaponInLeftHand());
                updateHandTarget(false);
                updateHandTarget(true);
            }
            {
                // Tracked hands are current here; hand transforms published in this phase are solved below, in the same frame
                F4CF_PERF_SCOPE("phaseBeforeArmSolve");
                api::core::invokeFramePhase(FramePhase::BeforeArmSolve);
            }
            {
                F4CF_PERF_SCOPE("solveArms");
                solveArm(false);
                solveArm(true);
            }
            {
                F4CF_PERF_SCOPE("flatten");
                updateDownFromRoot(); // Do world update now so that IK calculations have proper world reference
                applyUpperTwist();
            }

            // A claim published or cleared inside AfterArmSolve (a mod that needs the solved arm first) is re-solved right here,
            // before hand pose and weapon position run, so the rest of the frame still sees one consistent arm
            const std::array<std::uint64_t, 2> claimRevisionBefore{ g_externalAuthority.getHandClaimRevision(false), g_externalAuthority.getHandClaimRevision(true) };
            {
                F4CF_PERF_SCOPE("phaseAfterArmSolve");
                api::core::invokeFramePhase(FramePhase::AfterArmSolve);
            }
            bool resolved = false;
            for (const bool isLeft : { false, true }) {
                if (g_externalAuthority.getHandClaimRevision(isLeft) != claimRevisionBefore[isLeft ? 1 : 0]) {
                    restoreArmNodesToDefault(isLeft);
                    solveArm(isLeft);
                    resolved = true;
                }
            }
            if (resolved) {
                updateDownFromRoot();
            }
        }

        {
            F4CF_PERF_SCOPE("hideCullSelfie");
            // Misc stuff to show/hide things
            logger::trace("Pipboy and Weapons...");
            hide3rdPersonWeapon();
            hideFistHelpers();
            showHidePAHud();

            logger::trace("Cull geometry...");
            _cullGeometry.cullPlayerGeometry(g_frik.shouldHideBodyInScope());

            // project body out in front of the camera for debug purposes
            logger::trace("Selfie Time");
            _selfieHandler.onFrameUpdate();
        }

        {
            F4CF_PERF_SCOPE("handPose");
            logger::trace("Operate hands...");
            _handPose.onFrameUpdate(_root, _frameTime);
            api::core::invokeFramePhase(FramePhase::AfterHandPose);
        }

        if (_inPowerArmor) {
            fixArmor();
        }
#ifdef FRIK_FLAT // C38
        flatBodyWrittenZ();
#endif
    }

#ifdef FRIK_FLAT // C38
    /**
     * Flat (user 19 Nov: "pip boy or pause menu and possible others, the player's head and body float up and away"): FRIK
     * moves the 3D root (the skeleton root's parent) relative to its world z every frame: setBodyUnderHMD adds the
     * PlayerWorldNode z, setBodyPosture subtracts PlayerBodyOffsetUp + the HMD offset. In a running frame the engine has
     * rebuilt that world from the actor first. In a paused frame (C33E62, fo4flatvr [FRIK] bPausedFrame) it has not, so
     * each paused frame stacked the net offset on the last one: user FRIK.log 22:28:14-22:28:23 (PauseMenu), bodyZ
     * 7777 -> 10020 u, +240 u a second, camera and feet unchanged. Now the engine's value is kept (taken when the engine
     * rewrote it, i.e. the world z differs from what FRIK left) and put back first in a frame where it was not rewritten
     * (always in a paused frame), so every frame applies FRIK's offsets once to the engine's z. No switch: it only
     * removes the stacking (without a paused frame the engine value is always fresh).
     */
    void Skeleton::flatBodyEngineZ(RE::NiNode* body)
    {
        float& z = body->world.translate.z;
        const bool stale = _flatBodyZValid && (vrshim::bridge::pausedFrame() || z == _flatBodyWrittenZ);
        if (stale) {
            z = _flatBodyEngineZ;
        } else {
            _flatBodyEngineZ = z;
            _flatBodyZValid = true;
        }
    }

    void Skeleton::flatBodyWrittenZ()
    {
        if (_root && _root->parent) {
            _flatBodyWrittenZ = _root->parent->world.translate.z;
        }
    }
#endif

    void Skeleton::setTime()
    {
        _prevTime = _timer;
        QueryPerformanceFrequency(&_freqCounter);
        QueryPerformanceCounter(&_timer);
        _frameTime = static_cast<float>(_timer.QuadPart - _prevTime.QuadPart) / _freqCounter.QuadPart;
    }

    /**
     * Restore the skeleton main 25 nodes to their default transforms.
     * To wipe out any local transform changes the game might have made since last update
     */
    void Skeleton::restoreNodesToDefault()
    {
        for (const auto& [boneNode, resetTransform] : _skeletonNodesToDefaultTransforms) {
            boneNode->local = resetTransform;
        }
    }

    /**
     * Moves head up and back out of the player view and handle head movement.
     * It's still not good enough to prevent seeing hats and stuff, but it's a step forward in case
     * someone wants to tackle it further.
     */
    void Skeleton::setupHead(const float neckYaw, const float neckPitch) const
    {
#ifdef FRIK_FLAT // C39: the head is hidden as a whole in first person (CullGeometryHandler::flatHeadless): no push-back, so the
        // body and its shadow keep their natural head position (selfie shows the head; FRIK's own selfie rule below applies)
        const bool flatHeadless = CullGeometryHandler::flatHeadlessOn() && !g_frik.isSelfieModeOn();
#else
        constexpr bool flatHeadless = false;
#endif
        const float headBackAdj = (g_frik.isSelfieModeOn() && g_config.selfieIgnoreHideFlags) || flatHeadless
            ? 0 : g_config.headBackPositionOffset + (neckPitch > 0 ? 2 * neckPitch : 0);
        _head->local.translate -= RE::NiPoint3(headBackAdj, 2 * headBackAdj, 0);
        _head->local.rotate = _head->local.rotate * MatrixUtils::getMatrixFromEulerAngles(neckYaw, 0, neckPitch);
        RE::NiUpdateData* ud = nullptr;
        _head->UpdateWorldData(ud);
    }

    // below takes the two vectors from hmd to each hand and sums them to determine a center axis in which to see how much the hmd has rotated
    // A secondary angle is also calculated which is 90 degrees on the z axis up to handle when the hands are approaching the z plane of the hmd
    // this helps keep the body stable through a wide range of hand poses
    // this still struggles with hands close to the face and with one hand low and one hand high.
    // Will need to take prog advice to add weights to these positions which I'll do at a later date.
    float Skeleton::getNeckYaw() const
    {
        if (!_playerNodes) {
            logger::info("player nodes not set in neck yaw");
            return 0.0;
        }

        const RE::NiPoint3 pos = _playerNodes->uprightHmdNode->world.translate;
        const RE::NiPoint3 hmdToLeft = _playerNodes->secondaryWandNode->world.translate - pos;
        const RE::NiPoint3 hmdToRight = _playerNodes->primaryWandNode->world.translate - pos;
        float weight = 1.0f;

        if (MatrixUtils::vec3Len(hmdToLeft) < 10.0f || MatrixUtils::vec3Len(hmdToRight) < 10.0f) {
            return 0.0;
        }

        // handle excessive angles when hand is above the head.
        if (hmdToLeft.z > 0) {
            weight = (std::max)(weight - 0.05f * hmdToLeft.z, 0.0f);
        }

        if (hmdToRight.z > 0) {
            weight = (std::max)(weight - 0.05f * hmdToRight.z, 0.0f);
        }

        // hands moving across the chest rotate too much.   try to handle with below
        // wp = parWp + parWr * lp =>   lp = (wp - parWp) * parWr'
        const RE::NiPoint3 locLeft = _playerNodes->hmdNode->world.rotate * (hmdToLeft);
        const RE::NiPoint3 locRight = _playerNodes->hmdNode->world.rotate * (hmdToRight);

        if (locLeft.x > locRight.x) {
            const float delta = locRight.x - locLeft.x;
            weight = (std::max)(weight + 0.02f * delta, 0.0f);
        }

        const RE::NiPoint3 sum = hmdToRight + hmdToLeft;

        const RE::NiPoint3 forwardDir = MatrixUtils::vec3Norm(_playerNodes->hmdNode->world.rotate * (MatrixUtils::vec3Norm(sum)));
        // rotate sum to local hmd space to get the proper angle
        const RE::NiPoint3 hmdForwardDir = MatrixUtils::vec3Norm(_playerNodes->hmdNode->world.rotate * (_playerNodes->hmdNode->local.translate));

        const float anglePrime = atan2f(forwardDir.x, forwardDir.y);
        const float angleSec = atan2f(forwardDir.x, forwardDir.z);

        const float pitchDiff = atan2f(hmdForwardDir.y, hmdForwardDir.z) - atan2f(forwardDir.z, forwardDir.y);

        const float angleFinal = fabs(pitchDiff) > MatrixUtils::degreesToRads(80.0f) ? angleSec : anglePrime;
        return std::clamp(-angleFinal * weight, MatrixUtils::degreesToRads(-50.0f), MatrixUtils::degreesToRads(50.0f));
    }

    float Skeleton::getNeckPitch() const
    {
        const RE::NiPoint3& lookDir = MatrixUtils::vec3Norm(_playerNodes->hmdNode->world.rotate * (_playerNodes->hmdNode->local.translate));
        return atan2f(lookDir.y, lookDir.z);
    }

    float Skeleton::getBodyPitch(const float neckPitch) const
    {
        if (isComfortSneakHackEnabled()) {
            return MatrixUtils::degreesToRads(g_config.comfortSneakHackStaticBodyPitchAngle);
        }

        constexpr float basePitch = 105.3f;
        constexpr float weight = 0.1f;

#ifdef FRIK_FLAT // C36
        const float curHeight = playerHeightFlat();
#else
        const float curHeight = g_config.playerHeight;
#endif
        const float heightCalc = std::abs((curHeight - (_playerNodes->uprightHmdNode->local.translate.z + getAdjustedPlayerHMDOffset())) / curHeight);
        const float angle = heightCalc * (basePitch + weight * MatrixUtils::radsToDegrees(neckPitch));
        return MatrixUtils::degreesToRads(angle);
    }

    /**
     * set up the body underneath the headset in a proper scale and orientation
     */
    void Skeleton::setBodyUnderHMD(const float neckYaw, const float neckPitch)
    {
        if (g_config.disableSmoothMovement) {
            _playerNodes->playerWorldNode->local.translate.z = getAdjustedPlayerHMDOffset();
            RE::NiNodeUtils::UpdateWorldDataDown(_playerNodes->playerWorldNode, true);
        }

        //		float y    = (*g_playerCamera)->cameraNode->world.rotate.data[1][1];  // Middle column is y vector.   Grab just x and y portions and make a unit vector.    This can be used to rotate body to always be orientated with the hmd.
        //	float x    = (*g_playerCamera)->cameraNode->world.rotate.data[1][0];  //  Later will use this vector as the basis for the rest of the IK
        const float z = _root->local.translate.z;
        //	float z = groundHeight;

        Quaternion qa;
        qa.setAngleAxis(-neckPitch, RE::NiPoint3(-1, 0, 0));

        const RE::NiMatrix3 newRot = qa.getMatrix() * _playerNodes->hmdNode->local.rotate;

        _forwardDir = MatrixUtils::rotateXY(RE::NiPoint3(newRot.entry[1][0], newRot.entry[1][1], 0), neckYaw * 0.7f);
        _sidewaysRDir = RE::NiPoint3(_forwardDir.y, -_forwardDir.x, 0);

        // The body node has two writers: the player-update hook (GameHooks.cpp hookMainUpdatePlayer) keeps it under the camera
        // between FRIK frames, and this pass sets it for the frame. Both write the same value, the camera's X/Y, to local and
        // world, so whichever ran last the node reads the same; the local used to be zeroed here, which the hook then undid.
        RE::NiNode* body = _root->parent;
#ifdef FRIK_FLAT // C38
        flatBodyEngineZ(body);
#endif
        body->local.translate.x = _curentPosition.x;
        body->local.translate.y = _curentPosition.y;
        body->world.translate.x = _curentPosition.x;
        body->world.translate.y = _curentPosition.y;
        body->world.translate.z += _playerNodes->playerWorldNode->local.translate.z;

        const RE::NiPoint3 back = MatrixUtils::vec3Norm(RE::NiPoint3(_forwardDir.x, _forwardDir.y, 0));
        const auto bodyDir = RE::NiPoint3(0, 1, 0);

        _root->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(back, bodyDir) * body->world.rotate.Transpose();
        _root->local.translate = body->world.translate - _curentPosition;
        _root->local.translate.z = z;
        //_root->local.translate *= 0.0f;
        //_root->local.translate.y = g_config.playerBodyOffsetForwardStanding - 6.0f;
#ifdef FRIK_FLAT // C36
        _root->local.scale = playerHeightFlat() / DEFAULT_CAMERA_HEIGHT;
#else
        _root->local.scale = g_config.playerHeight / DEFAULT_CAMERA_HEIGHT; // set scale based off specified user height
#endif
    }

    void Skeleton::setBodyPosture(const float neckPitch)
    {
        const float bodyPitch = _inPowerArmor ? getBodyPitch(neckPitch) : getBodyPitch(neckPitch) / 1.2f;

        RE::NiNode* com = RE::NiNodeUtils::FindNode(_root, "COM");
        const RE::NiNode* neck = RE::NiNodeUtils::FindNode(_root, "Neck");
        RE::NiNode* spine = RE::NiNodeUtils::FindNode(_root, "SPINE1");

        _leftKneePos = RE::NiNodeUtils::FindNode(com, "LLeg_Calf")->world.translate;
        _rightKneePos = RE::NiNodeUtils::FindNode(com, "RLeg_Calf")->world.translate;

        com->local.translate.x = 0.0;
        com->local.translate.y = 0.0;

        // comfort sneak changes the height of the avatar without the player changing height in the real world, need to adjust for it
        const float comfortSneakAdjustZ = RE::VRSettings::IsComfortSneak() && isPlayerSneaking() ? _comfortSneakCameraOffsetAdjustment * _comfortSneakCameraOffsetAdjustment : 1.0f;

        // small offset to (1) not change player height when looking up/down and (2) move the body back, especially when looking down
        const float xOffsetByNeckPitch = fmaxf(0, (isComfortSneakHackEnabled() ? 2.0f : 5.0f) * fabs(neckPitch) * _root->local.scale);
        const float zOffsetByNeckPitch = 6.0f * neckPitch * _root->local.scale;

        const float playerAdjustZ =
            (4 * g_config.getPlayerBodyOffsetUp() - g_config.getPlayerHMDOffsetUp() + g_config.getPlayerLegSlackAdjustOffset()) * comfortSneakAdjustZ + zOffsetByNeckPitch;

        const auto neckPos = _curentPosition + RE::NiPoint3(-_forwardDir.x * (g_config.getPlayerBodyOffsetForward() / 2 - xOffsetByNeckPitch),
                                                   -_forwardDir.y * (g_config.getPlayerBodyOffsetForward() / 2 - xOffsetByNeckPitch),
                                                   -playerAdjustZ);

        _torsoLen = MatrixUtils::vec3Len(neck->world.translate - com->world.translate);

        const RE::NiPoint3 hmdToHip = neckPos - com->world.translate;
        const auto dir = RE::NiPoint3(-_forwardDir.x, -_forwardDir.y, 0);

        const float dist = tanf(bodyPitch) * MatrixUtils::vec3Len(hmdToHip);
        RE::NiPoint3 tmpHipPos = com->world.translate + dir * (dist / MatrixUtils::vec3Len(dir));
        tmpHipPos.z = com->world.translate.z;

        const RE::NiPoint3 hmdToNewHip = tmpHipPos - neckPos;
        const RE::NiPoint3 newHipPos = neckPos + hmdToNewHip * (_torsoLen / MatrixUtils::vec3Len(hmdToNewHip));

        const RE::NiPoint3 newPos = com->local.translate + _root->world.rotate * (newHipPos - com->world.translate);
        com->local.translate.y += newPos.y + g_config.getPlayerBodyOffsetForward() - 2 * xOffsetByNeckPitch;
        com->local.translate.z = _inPowerArmor ? newPos.z / 1.7f : newPos.z / 1.5f;

        // ???
        _root->parent->world.translate.z -= g_config.getPlayerBodyOffsetUp() + getAdjustedPlayerHMDOffset();

        const RE::NiMatrix3 mat = MatrixUtils::getMatrixFromRotateVectorVec(neckPos - tmpHipPos, hmdToHip) * spine->parent->world.rotate.Transpose();
        spine->local.rotate = spine->world.rotate * mat;
    }

    void Skeleton::setKneePos()
    {
        const auto lKnee = RE::NiNodeUtils::FindNode(_root, "LLeg_Calf");
        const auto rKnee = RE::NiNodeUtils::FindNode(_root, "RLeg_Calf");

        if (!lKnee || !rKnee) {
            return;
        }

        lKnee->world.translate.z = _leftKneePos.z;
        rKnee->world.translate.z = _rightKneePos.z;

        _leftKneePos = lKnee->world.translate;
        _rightKneePos = rKnee->world.translate;

        RE::NiNodeUtils::UpdateWorldDataDown(lKnee, false);
        RE::NiNodeUtils::UpdateWorldDataDown(rKnee, false);
    }

    // TODO: does it do anything? check if it works at all
    void Skeleton::fixArmor() const
    {
        auto lPauldron = RE::NiNodeUtils::FindNode(_root, "L_Pauldron");
        auto rPauldron = RE::NiNodeUtils::FindNode(_root, "R_Pauldron");

        if (!lPauldron || !rPauldron) {
            return;
        }

        //float delta = RE::NiNodeUtils::FindNode("LArm_Collarbone", _root)->world.translate.z - _root->world.translate.z;
        const float delta = RE::NiNodeUtils::FindNode(_root, "LArm_UpperArm")->world.translate.z - _root->world.translate.z;
        if (lPauldron) {
            lPauldron->local.translate.z = delta - 15.0f;
        }
        if (rPauldron) {
            rPauldron->local.translate.z = delta - 15.0f;
        }
    }

    void Skeleton::walk()
    {
        const auto lHip = RE::NiNodeUtils::FindNode(_root, "LLeg_Thigh");
        const auto rHip = RE::NiNodeUtils::FindNode(_root, "RLeg_Thigh");

        if (!lHip || !rHip) {
            return;
        }

        const auto lKnee = RE::NiNodeUtils::FindNode(lHip, "LLeg_Calf");
        const auto rKnee = RE::NiNodeUtils::FindNode(rHip, "RLeg_Calf");
        const auto lFoot = RE::NiNodeUtils::FindNode(lHip, "LLeg_Foot");
        const auto rFoot = RE::NiNodeUtils::FindNode(rHip, "RLeg_Foot");

        if (!lKnee || !rKnee || !lFoot || !rFoot) {
            return;
        }

        // move feet closer together
        const RE::NiPoint3 leftToRight = _inPowerArmor ? (rFoot->world.translate - lFoot->world.translate) * -0.15f : (rFoot->world.translate - lFoot->world.translate) * 0.3f;
        lFoot->world.translate += leftToRight;
        rFoot->world.translate -= leftToRight;

        // want to calculate direction vector first.     Will only concern with x-y vector to start.
        RE::NiPoint3 lastPos = _lastPosition;
        RE::NiPoint3 curPos = _curentPosition;
        curPos.z = 0;
        lastPos.z = 0;

        RE::NiPoint3 dir = curPos - lastPos;

        float curSpeed = std::clamp(abs(MatrixUtils::vec3Len(dir)) / _frameTime, 0.0f, 350.0f);
        if (_prevSpeed > 20.0f) {
            curSpeed = (curSpeed + _prevSpeed) / 2.0f;
        }

        const float stepTime = std::clamp(cos(curSpeed / 140.0f), 0.28f, 0.50f);
        dir = MatrixUtils::vec3Norm(dir);

        // if decelerating reset target
        if (curSpeed - _prevSpeed < -20.0f) {
            _walkingState = 3;
        }

        _prevSpeed = curSpeed;

        static float spineAngle = 0.0;

        // setup current walking state based on velocity and previous state
        if (!isJumpingOrInAir()) {
            switch (_walkingState) {
            case 0: {
                if (curSpeed >= 35.0) {
                    _walkingState = 1; // start walking
                    _footStepping = std::rand() % 2 + 1; // pick a random foot to take a step  // NOLINT(concurrency-mt-unsafe)
                    _stepDir = dir;
                    _stepTimeinStep = stepTime;
                    _delayFrame = 2;

                    if (_footStepping == 1) {
                        _rightFootTarget = rFoot->world.translate + _stepDir * (curSpeed * stepTime * 1.5f);
                        _rightFootStart = rFoot->world.translate;
                        _leftFootTarget = lFoot->world.translate;
                        _leftFootStart = lFoot->world.translate;
                        _leftFootPos = _leftFootStart;
                        _rightFootPos = _rightFootStart;
                    } else {
                        _rightFootTarget = rFoot->world.translate;
                        _rightFootStart = rFoot->world.translate;
                        _leftFootTarget = lFoot->world.translate + _stepDir * (curSpeed * stepTime * 1.5f);
                        _leftFootStart = lFoot->world.translate;
                        _leftFootPos = _leftFootStart;
                        _rightFootPos = _rightFootStart;
                    }
                    _currentStepTime = stepTime / 2;
                    break;
                }
                _currentStepTime = 0.0;
                _footStepping = 0;
                spineAngle = 0.0;
                break;
            }
            case 1: {
                if (curSpeed < 20.0) {
                    _walkingState = 2; // begin process to stop walking
                    _currentStepTime = 0.0;
                }
                break;
            }
            case 2: {
                if (curSpeed >= 20.0) {
                    _walkingState = 1; // resume walking
                    _currentStepTime = 0.0;
                }
                break;
            }
            case 3: {
                _stepDir = dir;
                if (_footStepping == 1) {
                    _rightFootTarget = rFoot->world.translate + _stepDir * (curSpeed * stepTime * 0.1f);
                } else {
                    _leftFootTarget = lFoot->world.translate + _stepDir * (curSpeed * stepTime * 0.1f);
                }
                _walkingState = 1;
                break;
            }
            default: {
                _walkingState = 0;
                break;
            }
            }
        } else {
            _walkingState = 0;
        }

        if (_walkingState == 0) {
            // we're standing still so just set foot positions accordingly.
            _leftFootPos = lFoot->world.translate;
            _rightFootPos = rFoot->world.translate;
            _leftFootPos.z = _root->world.translate.z;
            _rightFootPos.z = _root->world.translate.z;

            return;
        }
        if (_walkingState == 1) {
            RE::NiPoint3 dirOffset = dir - _stepDir;
            const float dot = MatrixUtils::vec3Dot(dir, _stepDir);
            const float scale = (std::min)(curSpeed * stepTime * 1.5f, 140.0f);
            dirOffset = dirOffset * scale;

            float sign = 1.0f;

            _currentStepTime += _frameTime;

            const float frameStep = _frameTime / _stepTimeinStep;
            const float interp = std::clamp(frameStep * (_currentStepTime / _frameTime), 0.0f, 1.0f);

            if (_footStepping == 1) {
                sign = -1.0f;
                if (dot < 0.9) {
                    if (!_delayFrame) {
                        _rightFootTarget += dirOffset;
                        _stepDir = dir;
                        _delayFrame = 2;
                    } else {
                        _delayFrame--;
                    }
                } else {
                    _delayFrame = _delayFrame == 2 ? _delayFrame : _delayFrame + 1;
                }
                _rightFootTarget.z = _root->world.translate.z;
                _rightFootStart.z = _root->world.translate.z;
                _rightFootPos = _rightFootStart + (_rightFootTarget - _rightFootStart) * interp;
                const float stepAmount = std::clamp(MatrixUtils::vec3Len(_rightFootTarget - _rightFootStart) / 150.0f, 0.0f, 1.0f);
                const float stepHeight = (std::max)(stepAmount * 9.0f, 1.0f);
                const float up = sinf(interp * std::numbers::pi_v<float>) * stepHeight;
                _rightFootPos.z += up;
            } else {
                if (dot < 0.9f) {
                    if (!_delayFrame) {
                        _leftFootTarget += dirOffset;
                        _stepDir = dir;
                        _delayFrame = 2;
                    } else {
                        _delayFrame--;
                    }
                } else {
                    _delayFrame = _delayFrame == 2 ? _delayFrame : _delayFrame + 1;
                }
                _leftFootTarget.z = _root->world.translate.z;
                _leftFootStart.z = _root->world.translate.z;
                _leftFootPos = _leftFootStart + (_leftFootTarget - _leftFootStart) * interp;
                const float stepAmount = std::clamp(MatrixUtils::vec3Len(_leftFootTarget - _leftFootStart) / 150.0f, 0.0f, 1.0f);
                const float stepHeight = (std::max)(stepAmount * 9.0f, 1.0f);
                const float up = sinf(interp * std::numbers::pi_v<float>) * stepHeight;
                _leftFootPos.z += up;
            }

            spineAngle = sign * sinf(interp * std::numbers::pi_v<float>) * 3.0f;

            _spine->local.rotate = MatrixUtils::getMatrixFromEulerAngles(MatrixUtils::degreesToRads(spineAngle), 0.0f, 0.0f) * _spine->local.rotate;

            if (_currentStepTime > stepTime) {
                _currentStepTime = 0.0;
                _stepDir = dir;
                _stepTimeinStep = stepTime;
                //logger::info("%2f %2f", curSpeed, stepTime);

                if (_footStepping == 1) {
                    _footStepping = 2;
                    _leftFootTarget = lFoot->world.translate + _stepDir * scale;
                    _leftFootStart = _leftFootPos;
                } else {
                    _footStepping = 1;
                    _rightFootTarget = rFoot->world.translate + _stepDir * scale;
                    _rightFootStart = _rightFootPos;
                }
            }
            return;
        }
        if (_walkingState == 2) {
            _leftFootPos = lFoot->world.translate;
            _rightFootPos = rFoot->world.translate;
            _walkingState = 0;
        }
    }

    // adapted solver from VRIK.  Thanks prog!
    void Skeleton::setSingleLeg(const bool isLeft) const
    {
        const auto footNode = isLeft ? RE::NiNodeUtils::FindNode(_root, "LLeg_Foot") : RE::NiNodeUtils::FindNode(_root, "RLeg_Foot");
        const auto kneeNode = isLeft ? RE::NiNodeUtils::FindNode(_root, "LLeg_Calf") : RE::NiNodeUtils::FindNode(_root, "RLeg_Calf");
        const auto hipNode = isLeft ? RE::NiNodeUtils::FindNode(_root, "LLeg_Thigh") : RE::NiNodeUtils::FindNode(_root, "RLeg_Thigh");
#ifdef FRIK_FLAT // C39: a skeleton mod's offset bones between thigh/calf/foot -> the chain-aware solve
        if (hipNode && kneeNode && footNode && (kneeNode->parent != hipNode || footNode->parent != kneeNode)) {
            const bool s_chain = vrshim::bridge::iniIntLive("bLegChainIK", 1) != 0;  // 20 Nov: live (bridge v7)
            if (s_chain) {
                setSingleLegChain(isLeft);
                return;
            }
        }
#endif

        const RE::NiPoint3 footPos = isLeft ? _leftFootPos : _rightFootPos;
        const RE::NiPoint3 hipPos = hipNode->world.translate;

        const RE::NiPoint3 footToHip = hipNode->world.translate - footPos;

        auto rotV = RE::NiPoint3(0, 1, 0);
        if (_inPowerArmor) {
            rotV.y = 0;
            rotV.z = isLeft ? 1.0f : -1.0f;
        }
#ifdef FRIK_FLAT // C42: knee pole = the animated thigh's axis (VRIK), not the rest pose's
        const RE::NiPoint3 hipDir = _flatAnimLegs ? _flatPole[isLeft ? 0 : 1] : hipNode->world.rotate.Transpose() * (rotV);
#else
        const RE::NiPoint3 hipDir = hipNode->world.rotate.Transpose() * (rotV);
#endif
        const RE::NiPoint3 xDir = MatrixUtils::vec3Norm(footToHip);
        const RE::NiPoint3 yDir = MatrixUtils::vec3Norm(hipDir - xDir * MatrixUtils::vec3Dot(hipDir, xDir));

        const float thighLenOrig = MatrixUtils::vec3Len(kneeNode->local.translate);
        const float calfLenOrig = MatrixUtils::vec3Len(footNode->local.translate);
        float thighLen = thighLenOrig;
        float calfLen = calfLenOrig;

        const float ftLen = (std::max)(MatrixUtils::vec3Len(footToHip), 0.1f);

        if (ftLen > thighLen + calfLen) {
            const float diff = ftLen - thighLen - calfLen;
            const float ratio = calfLen / (calfLen + thighLen);
            calfLen += ratio * diff + 0.1f;
            thighLen += (1.0f - ratio) * diff + 0.1f;
        }
        // Use the law of cosines to calculate the angle the calf must bend to reach the knee position
        // In cases where this is impossible (foot too close to thigh), then set calfLen = thighLen so
        // there is always a solution
        float footAngle = acosf((calfLen * calfLen + ftLen * ftLen - thighLen * thighLen) / (2 * calfLen * ftLen));
        if (std::isnan(footAngle) || std::isinf(footAngle)) {
            calfLen = thighLen = (thighLenOrig + calfLenOrig) / 2.0f;
            footAngle = acosf((calfLen * calfLen + ftLen * ftLen - thighLen * thighLen) / (2 * calfLen * ftLen));
        }

        BodyAdjustmentSubConfigMode::updateLegSlack((thighLenOrig + calfLenOrig) - ftLen);

        // Get the desired world coordinate of the knee
        const float xDist = cosf(footAngle) * calfLen;
        const float yDist = sinf(footAngle) * calfLen;
        const RE::NiPoint3 kneePos = footPos + xDir * xDist + yDir * yDist;

        const RE::NiPoint3 pos = kneePos - hipPos;
        RE::NiPoint3 uLocalDir = hipNode->world.rotate * (MatrixUtils::vec3Norm(pos) / hipNode->world.scale);
        hipNode->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(uLocalDir, kneeNode->local.translate) * hipNode->local.rotate;

        const RE::NiMatrix3 hipWR = hipNode->local.rotate * hipNode->parent->world.rotate;

        RE::NiMatrix3 calfWR = kneeNode->local.rotate * hipWR;

        uLocalDir = calfWR * (MatrixUtils::vec3Norm(footPos - kneePos) / kneeNode->world.scale);
        kneeNode->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(uLocalDir, footNode->local.translate) * kneeNode->local.rotate;

        calfWR = kneeNode->local.rotate * hipWR;

        // Calculate Clp:  Cwp = Twp + Twr * (Clp * Tws) = kneePos   ===>   Clp = Twr' * (kneePos - Twp) / Tws
        kneeNode->local.translate = hipWR * ((kneePos - hipPos) / hipNode->world.scale);
        if (MatrixUtils::vec3Len(kneeNode->local.translate) > thighLenOrig) {
            kneeNode->local.translate = MatrixUtils::vec3Norm(kneeNode->local.translate) * thighLenOrig;
        }

        // Calculate Flp:  Fwp = Cwp + Cwr * (Flp * Cws) = footPos   ===>   Flp = Cwr' * (footPos - Cwp) / Cws
        footNode->local.translate = calfWR * ((footPos - kneePos) / kneeNode->world.scale);
        if (MatrixUtils::vec3Len(footNode->local.translate) > calfLenOrig) {
            footNode->local.translate = MatrixUtils::vec3Norm(footNode->local.translate) * calfLenOrig;
        }
#ifdef FRIK_FLAT // C42: the foot keeps the animation's (or the planted) world rotation instead of following the shin (VRIK updateLeg)
        if (_flatAnimLegs && _flatFootRotValid) {
            footNode->local.rotate = _flatFootRot[isLeft ? 0 : 1] * calfWR.Transpose();
        }
#endif
    }

#ifdef FRIK_FLAT // C39
    /**
     * Flat (user 19 Nov, TestProfileVR: "running/walking/moving send the playes legs flying around at different angles,
     * potentialky different skeleton cause"): the user's female skeleton (Skeletal Adjustments for CBBE over Discrete Female
     * Skeleton, female/skeleton.nif) puts offset bones into the leg chain: Pelvis > LLeg_Thigh_FULL_OFFSET > LLeg_Thigh >
     * LLeg_Calf_FULL_OFFSET > LLeg_Calf > LLeg_Calf_OFFSET (t 0,2,0, 6.7 deg) > LLeg_Foot_OFFSET (t 0,-2,0, 2.5 deg) > LLeg_Foot.
     * setSingleLeg assumes calf = child of thigh and foot = child of calf: it takes the bone lengths from the calf/foot local
     * translations, aims and places them in the thigh/calf frames, so the foot landed in a frame turned ~9 deg and offset
     * 2 u from the one it was solved in. Same solve here, with the rest vectors and the parent frames composed through the
     * nodes in between (their locals are the skeleton's, untouched). Identical to setSingleLeg for a direct chain (not used
     * then). Off: fo4flatvr.ini [FRIK] bLegChainIK=0.
     */
    void Skeleton::setSingleLegChain(const bool isLeft) const
    {
        RE::NiNode* footNode = isLeft ? RE::NiNodeUtils::FindNode(_root, "LLeg_Foot") : RE::NiNodeUtils::FindNode(_root, "RLeg_Foot");
        RE::NiNode* kneeNode = isLeft ? RE::NiNodeUtils::FindNode(_root, "LLeg_Calf") : RE::NiNodeUtils::FindNode(_root, "RLeg_Calf");
        RE::NiNode* hipNode = isLeft ? RE::NiNodeUtils::FindNode(_root, "LLeg_Thigh") : RE::NiNodeUtils::FindNode(_root, "RLeg_Thigh");
        if (!footNode || !kneeNode || !hipNode || !hipNode->parent) {
            return;
        }
        // nodes strictly between top and node, top-down; false if top is not an ancestor within 8 levels
        constexpr int kMax = 8;
        auto between = [](const RE::NiNode* top, const RE::NiAVObject* node, std::array<RE::NiNode*, kMax>& out, int& n) {
            RE::NiNode* up[kMax];
            int         c = 0;
            RE::NiNode* p = node->parent;
            for (; p && p != top; p = p->parent) {
                if (c == kMax) {
                    return false;
                }
                up[c++] = p;
            }
            if (!p) {
                return false;
            }
            n = c;
            for (int i = 0; i < c; ++i) {
                out[i] = up[c - 1 - i];
            }
            return true;
        };
        std::array<RE::NiNode*, kMax> hk{}, kf{};
        int                           nhk = 0, nkf = 0;
        if (!between(hipNode, kneeNode, hk, nhk) || !between(kneeNode, footNode, kf, nkf)) {
            return;
        }
        // rest vector of the child's origin in the top bone's local frame (FRIK convention: parent-local = lR^T . child-local)
        auto restVec = [](const std::array<RE::NiNode*, kMax>& chain, const int n, const RE::NiAVObject* child) {
            RE::NiPoint3 p = child->local.translate;
            for (int i = n - 1; i >= 0; --i) {
                const auto* x = chain[i];
                p = x->local.translate + x->local.rotate.Transpose() * (p * x->local.scale);
            }
            return p;
        };
        const RE::NiPoint3 vHK = restVec(hk, nhk, kneeNode);
        const RE::NiPoint3 vKF = restVec(kf, nkf, footNode);

        const RE::NiPoint3 footPos = isLeft ? _leftFootPos : _rightFootPos;
        const RE::NiPoint3 hipPos = hipNode->world.translate;
        const float        hs = hipNode->world.scale;
        const RE::NiPoint3 footToHip = hipPos - footPos;

        auto rotV = RE::NiPoint3(0, 1, 0);
        if (_inPowerArmor) {
            rotV.y = 0;
            rotV.z = isLeft ? 1.0f : -1.0f;
        }
        // C42: knee pole = the animated thigh's axis (VRIK), not the rest pose's
        const RE::NiPoint3 hipDir = _flatAnimLegs ? _flatPole[isLeft ? 0 : 1] : hipNode->world.rotate.Transpose() * (rotV);
        const RE::NiPoint3 xDir = MatrixUtils::vec3Norm(footToHip);
        const RE::NiPoint3 yDir = MatrixUtils::vec3Norm(hipDir - xDir * MatrixUtils::vec3Dot(hipDir, xDir));

        const float thighLenOrig = MatrixUtils::vec3Len(vHK);
        const float calfLenOrig = MatrixUtils::vec3Len(vKF);
        if (thighLenOrig < 1.0f || calfLenOrig < 1.0f) {
            return;
        }
        float thighLen = thighLenOrig;
        float calfLen = calfLenOrig;
        const float ftLen = (std::max)(MatrixUtils::vec3Len(footToHip), 0.1f);
        if (ftLen > thighLen + calfLen) {
            const float diff = ftLen - thighLen - calfLen;
            const float ratio = calfLen / (calfLen + thighLen);
            calfLen += ratio * diff + 0.1f;
            thighLen += (1.0f - ratio) * diff + 0.1f;
        }
        float footAngle = acosf((calfLen * calfLen + ftLen * ftLen - thighLen * thighLen) / (2 * calfLen * ftLen));
        if (std::isnan(footAngle) || std::isinf(footAngle)) {
            calfLen = thighLen = (thighLenOrig + calfLenOrig) / 2.0f;
            footAngle = acosf((calfLen * calfLen + ftLen * ftLen - thighLen * thighLen) / (2 * calfLen * ftLen));
        }
        BodyAdjustmentSubConfigMode::updateLegSlack((thighLenOrig + calfLenOrig) - ftLen);

        const RE::NiPoint3 kneePos = footPos + xDir * (cosf(footAngle) * calfLen) + yDir * (sinf(footAngle) * calfLen);

        // thigh: turn so the rest knee vector points at the knee target
        RE::NiPoint3 uLocalDir = hipNode->world.rotate * (MatrixUtils::vec3Norm(kneePos - hipPos) / hs);
        hipNode->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(uLocalDir, vHK) * hipNode->local.rotate;
        const RE::NiMatrix3 hipWR = hipNode->local.rotate * hipNode->parent->world.rotate;

        // knee's parent frame through the nodes in between
        RE::NiMatrix3 kpWR = hipWR;
        RE::NiPoint3  kpWP = hipPos;
        float         kpWS = hs;
        for (int i = 0; i < nhk; ++i) {
            kpWP += kpWR.Transpose() * (hk[i]->local.translate * kpWS);
            kpWR = hk[i]->local.rotate * kpWR;
            kpWS *= hk[i]->local.scale;
        }
        const float ks = kpWS * kneeNode->local.scale;

        // calf: turn so the rest foot vector points at the foot target
        RE::NiMatrix3 calfWR = kneeNode->local.rotate * kpWR;
        uLocalDir = calfWR * (MatrixUtils::vec3Norm(footPos - kneePos) / ks);
        kneeNode->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(uLocalDir, vKF) * kneeNode->local.rotate;
        calfWR = kneeNode->local.rotate * kpWR;

        // knee at its target (thigh never longer than at rest), expressed in its real parent
        RE::NiPoint3 kneeT = kneePos;
        if (const float l = MatrixUtils::vec3Len(kneePos - hipPos); l > thighLenOrig * hs && l > 0.0f) {
            kneeT = hipPos + (kneePos - hipPos) * (thighLenOrig * hs / l);
        }
        kneeNode->local.translate = kpWR * ((kneeT - kpWP) / kpWS);

        // foot's parent frame from the placed knee
        RE::NiMatrix3 fpWR = calfWR;
        RE::NiPoint3  fpWP = kneeT;
        float         fpWS = ks;
        for (int i = 0; i < nkf; ++i) {
            fpWP += fpWR.Transpose() * (kf[i]->local.translate * fpWS);
            fpWR = kf[i]->local.rotate * fpWR;
            fpWS *= kf[i]->local.scale;
        }
        RE::NiPoint3 footT = footPos;
        if (const float l = MatrixUtils::vec3Len(footPos - kneeT); l > calfLenOrig * ks && l > 0.0f) {
            footT = kneeT + (footPos - kneeT) * (calfLenOrig * ks / l);
        }
        footNode->local.translate = fpWR * ((footT - fpWP) / fpWS);
        // C42: the foot keeps the animation's (or the planted) world rotation instead of following the shin (VRIK updateLeg)
        if (_flatAnimLegs && _flatFootRotValid) {
            footNode->local.rotate = _flatFootRot[isLeft ? 0 : 1] * fpWR.Transpose();
        }
    }
#endif

#ifdef FRIK_FLAT // C42
    /*
     * C42 VRIK-style legs (user 20 Nov: "the FRIK approach is perpetually jank" ... "start vrik style legs"; research
     * analysis/vrik-legs.md). FRIK replaced the engine's leg pose with its rest pose every frame and invented a walk cycle from
     * the camera's speed (walk(): steps from camera deltas, feet on a flat plane at the smoothed root z, standing feet glued to the
     * root so every head yaw swung them). VRIK (VrikBody::updateWalk / updateLeg, decompiled) instead:
     *  - moving: the feet are the game's own third-person locomotion animation, which flat plays on the body in first person
     *    (C37: the engine rewrites COM, thighs, calves, feet every frame); the legs are re-solved with the IK so each foot lands
     *    where the animation put it, keeping the animation's foot rotation, the knee pole from the animated thigh;
     *  - standing: feet planted in the world; short procedural steps (0.2-0.5 s, sine lift) when a foot is more than
     *    2 x body scale u from its target (the animated idle feet under the body), e.g. after turning past VRIK's 6 deg dead zone
     *    or a real room step; the other foot slides at 7.5 u/s meanwhile; sneak narrows the stance 12.5 %, a drawn weapon 20 %
     *    standing (VRIK). FRIK's body placement (COM under the head) stays: sneak's deep animated crouch is not used, the IK bends
     *    the knees down to the animated feet.
     * Flat specifics (research 3.2 risks): the animated foot offsets are mapped with the stride unscaled (relative to the animated
     * hip, so the C36 body scale does not make the feet skate) and the hip width / foot height scaled; foot height is against the
     * actor's own z (the real ground) minus FRIK's PlayerBodyOffsetUp, not the smooth-movement z; the animated pose is turned from
     * the actor's heading to the leg heading (FRIK's body heading with VRIK's dead zone while standing). No camera code from VRIK
     * (head bob / fCharacterHeightOffset): the camera is untouched. Off (live, 1 Hz): fo4flatvr.ini [FRIK] bAnimatedLegs=0 =
     * FRIK's walk().
     * ROLLED BACK to default OFF (20 Nov ~20:00; user: "no animation or movement seen"): measured (legs_session.py, no-XR,
     * TestProfileVR) while walking / strafing steadily the engine does NOT re-pose the third-person legs in first person on flat:
     * "engine changed L foot 0 thigh 0 COM 0 of 120 frames", animated foot range 0.0 u; only ~0.7 u of movement at a start.
     * The C37 "17 bones animated" finding was idle / transition writes, not a locomotion cycle. VRIK's input does not exist
     * here until the third-person graph is made to run its locomotion in first person (engine work, not done).
     * C43 (20 Nov): done - BodyGraph.cpp runs graph 0 in first person and skips the engine's first-person bone copy (D7E380,
     * the frozen pose above). Default on again, only on frames the body graph posed ([FRIK] bBodyGraph); else FRIK's walk.
     */
    namespace
    {
        using namespace vrshim::xf;
        constexpr float kPi = std::numbers::pi_v<float>;
        constexpr float kTurnDeadZone = 6.0f * kPi / 180.0f;  // VRIK bodyRotationAngleLimit (vrik.ini default)
        constexpr float kTurnRate = 5.0f;                     // VRIK: 5 x error per second
        constexpr float kStepTol = 2.0f;                      // VRIK: a foot steps when > 2 x bodySize u from its target
        constexpr float kSnapDist = 50.0f;                    // VRIK: > 50 x bodySize u = teleport, re-plant
        constexpr float kSlideSpeed = 7.5f;                   // VRIK: the other foot's slide during a step, u/s (x bodySize)
        constexpr float kStopHold = 0.15f;                    // VRIK: animated feet 0.15 s after the movement stops
        constexpr float kLeanBand = 8.0f;                     // flat: lean dead band of the standing step targets (u x scale)

        RE::NiPoint3 toP(const V& v) { return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) }; }
        float        wrapPi(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }
        RE::NiMatrix3 slerpRot(const RE::NiMatrix3& a, const RE::NiMatrix3& b, const float t)
        {
            Quaternion qa, qb;
            qa.fromMatrix(a);
            qb.fromMatrix(b);
            qa.slerp(std::clamp(t, 0.0f, 1.0f), qb);
            return qa.getMatrix();
        }
        float lenXY(const RE::NiPoint3& p) { return std::sqrt(p.x * p.x + p.y * p.y); }
    }

    bool Skeleton::flatAnimatedLegsOn()
    {
        static int  s_on = -1;
        static auto s_t = std::chrono::steady_clock::time_point{};
        const auto  now = std::chrono::steady_clock::now();
        if (s_on < 0 || now - s_t > std::chrono::seconds(1)) {  // live: the ini (incl. MCM / panel previews) re-read once a second
            s_t = now;
            const int v = vrshim::bridge::iniInt("bAnimatedLegs", 1) != 0 ? 1 : 0;  // C43: default on again (needs the body graph)
            if (v != s_on) {
                logger::info("FRIK-flat C42: VRIK-style legs (game locomotion animation + planted steps): {} ([FRIK] bAnimatedLegs)",
                    v ? "on (with the C43 body graph)" : "OFF (FRIK's walk)");
            }
            s_on = v;
        }
        // C43: only while the third-person body graph posed the legs this frame (otherwise the pose is the engine's frozen
        // first-person bone copy - the C42 rollback cause); else FRIK's walk for that frame
        const bool graph = bodygraph::posingLegs();
        static int s_graph = -1;
        if (s_on == 1 && (graph ? 1 : 0) != s_graph) {
            s_graph = graph ? 1 : 0;
            logger::info("FRIK-flat C42: animated legs {}", graph ? "follow the body graph" : "wait for the body graph (FRIK's walk meanwhile)");
        }
        return s_on == 1 && graph;
    }

    /**
     * The animated thigh / foot poses of this frame in the actor frame (the tree node: x right, y forward, z up, the actor's
     * feet at 0), composed from C37's per-bone engine locals (FRIK.cpp flatTrackFrameStart) through every bone in between,
     * so skeleton-mod offset bones (C39) are included.
     */
    bool Skeleton::flatCaptureAnimLegs()
    {
        auto* const tree = getFlattenedBoneTree();
        const auto& eng = g_frik.flatEngineLocal();
        if (!tree || !tree->transforms || tree->numTransforms <= 0 || static_cast<int>(eng.size()) != tree->numTransforms) {
            return false;
        }
        const int n = tree->numTransforms;
        if (_flatLegIdxTree != tree) {
            _flatLegIdxTree = tree;
            static constexpr const char* kNames[4] = { "LLeg_Thigh", "LLeg_Foot", "RLeg_Thigh", "RLeg_Foot" };
            for (int k = 0; k < 4; ++k) {
                _flatLegIdx[k] = -1;
                for (int i = 0; i < n; ++i) {
                    const char* nm = tree->transforms[i].name.c_str();
                    if (nm && _stricmp(nm, kNames[k]) == 0) {
                        _flatLegIdx[k] = i;
                        break;
                    }
                }
            }
            std::string chain;
            for (int i = _flatLegIdx[1], g = 0; i >= 0 && i < n && g < 64; i = tree->transforms[i].parPos, ++g) {
                const auto& b = tree->transforms[i];
                chain = std::format("{}{}{} < ", b.name.c_str() ? b.name.c_str() : "?", b.refNode == _root ? "(tree node)" : "", b.refNode ? "" : "*") + chain;
                if (b.parPos == i) {
                    break;
                }
            }
            logger::info("FRIK-flat C42: animated legs: tree {} (skeleton root {}), bones L thigh {} foot {} R thigh {} foot {}; LLeg_Foot chain: {}",
                static_cast<const void*>(tree), static_cast<const void*>(tree) == static_cast<const void*>(_root) ? "same" : "DIFFERENT", _flatLegIdx[0],
                _flatLegIdx[1], _flatLegIdx[2], _flatLegIdx[3], chain);
        }
        for (const int i : _flatLegIdx) {
            if (i < 0) {
                return false;
            }
        }
        {  // diag: does the engine re-pose the legs every frame?
            static int s_com = -2;
            if (s_com == -2) {
                s_com = -1;
                for (int i = 0; i < n; ++i) {
                    if (tree->transforms[i].name.c_str() && _stricmp(tree->transforms[i].name.c_str(), "COM") == 0) {
                        s_com = i;
                        break;
                    }
                }
            }
            const int  di[3] = { _flatLegIdx[1], _flatLegIdx[0], s_com };
            int*       cnt[3] = { &_flatLegDiag.engChanged, &_flatLegDiag.thighChanged, &_flatLegDiag.comChanged };
            for (int j = 0; j < 3; ++j) {
                if (di[j] < 0) {
                    continue;
                }
                const auto& e = eng[di[j]];
                if (std::memcmp(&e.rotate, &_flatPrevEng[j].rotate, sizeof(e.rotate)) != 0 || std::memcmp(&e.translate, &_flatPrevEng[j].translate, sizeof(e.translate)) != 0) {
                    ++*cnt[j];
                }
                _flatPrevEng[j] = e;
            }
            ++_flatLegDiag.engFrames;
        }
        const Xf treeInv = Inv(FromNi(_root->world));
        for (int k = 0; k < 4; ++k) {
            int idx[64];
            int c = 0;
            for (int i = _flatLegIdx[k]; i >= 0 && i < n && c < 64; i = tree->transforms[i].parPos) {
                if (tree->transforms[i].refNode == _root) {
                    break;  // the tree node itself: the frame we want
                }
                idx[c++] = i;
                if (tree->transforms[i].parPos == i) {
                    break;
                }
            }
            Xf rel;
            for (int j = c - 1; j >= 0; --j) {
                rel = Mul(rel, FromNi(eng[idx[j]]));
            }
            _flatAnimRel[k] = ToNi(rel);
            if (k == 1 || k == 3) {  // diag: composed vs the frame-start world the engine left (both relative to the tree node)
                const Xf    w = Mul(treeInv, FromNi(tree->transforms[_flatLegIdx[k]].world));
                const float e = MatrixUtils::vec3Len(toP(w.t) - toP(rel.t));
                _flatLegDiag.capErr = (std::max)(_flatLegDiag.capErr, e);
            }
        }
        return true;
    }

    void Skeleton::flatStartStep(const int a_foot, const std::array<RE::NiPoint3, 2>& a_tgt, const float a_scale)
    {
        const float dist = MatrixUtils::vec3Len(a_tgt[a_foot] - _flatPlanted[a_foot]);
        const float r = std::clamp(dist / (20.0f * a_scale), 0.0f, 1.0f);  // VRIK: step time 0.2 + 0.3 r s, lift max(1, 6 r) x bodySize
        _flatStepFoot = a_foot;
        _flatStepT = 0.0f;
        _flatStepDur = 0.2f + 0.3f * r;
        _flatStepLift = (std::max)(1.0f, 6.0f * r) * a_scale;
        _flatStepStart = _flatPlanted[a_foot];
        _flatStepStartRot = _flatPlantedRot[a_foot];
        ++_flatLegDiag.steps;
    }

    /**
     * Foot targets (_leftFootPos / _rightFootPos), foot world rotations and knee poles for this frame; FRIK's setSingleLeg /
     * setSingleLegChain solve to them. Runs after setBodyPosture (root and thighs placed).
     */
    void Skeleton::flatAnimatedLegs()
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const float dt = std::clamp(_frameTime, 0.0f, 0.1f);
        const bool  paused = vrshim::bridge::pausedFrame();
        const float s = _root->world.scale > 0.01f ? _root->world.scale : 1.0f;
        _walkingState = 0;  // FRIK's walk() starts fresh if the switch goes off
        if (!_flatAnimLegsPrev || _frameTime > 0.5f) {  // first frame, a switch, or a gap (other camera state, load): start fresh
            _flatPlantedValid = false;
            _flatLegYawValid = false;
            _flatStepFoot = -1;
            _flatMoving = false;
            _flatPrevPlanted = { false, false };
        }

        // movement state from the actor, not the camera (VRIK setupState): ActorState (+0x128) moveMode (+0x130, 14 bits):
        // bits 0-3 moving back / forward / right / left, bit 10 swimming (CommonLibF4 IsSwimming = 0x400). Roomscale and head
        // moves are not walking.
        const std::uint32_t moveMode = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(player) + 0x130) & 0x3FFF;
        const bool          inAir = isJumpingOrInAir();
        const bool          wantMove = !paused && ((moveMode & 0xF) != 0 || (moveMode & 0x400) != 0 || inAir);
        _flatLegDiag.moveBits = moveMode;
        _flatLegDiag.moveBitsOr |= moveMode;
        if (wantMove) {
            if (!_flatMoving) {
                _flatMoving = true;
                _flatAnimBlend = _flatPlantedValid ? 0.0f : 1.0f;  // planted -> animated over 0.15 s
            }
            _flatMoveHold = kStopHold;
        } else if (_flatMoving) {
            _flatMoveHold -= dt;
            if (_flatMoveHold <= 0.0f || paused) {  // stop: plant the feet where the animation left them
                _flatMoving = false;
                _flatPlanted = _flatOut;
                _flatPlantedRot = _flatOutRot;
                _flatPlantedValid = true;
                _flatStepFoot = -1;
                _flatLegYawValid = false;
                _flatPrevPlanted = { false, false };
            }
        }

        // leg heading: FRIK's body heading; standing, VRIK's dead zone (hold within 6 deg, then turn at 5 x error / s)
        const float bodyYaw = std::atan2(_forwardDir.x, _forwardDir.y);
        if (!_flatLegYawValid || _flatMoving) {
            _flatLegYaw = bodyYaw;
            _flatLegYawValid = true;
            _flatLegYawTurning = false;
        } else {
            const float err = wrapPi(bodyYaw - _flatLegYaw);
            if (!_flatLegYawTurning && std::abs(err) > kTurnDeadZone) {
                _flatLegYawTurning = true;
            }
            if (_flatLegYawTurning) {
                _flatLegYaw = wrapPi(_flatLegYaw + err * (std::min)(1.0f, kTurnRate * dt));
                if (std::abs(wrapPi(bodyYaw - _flatLegYaw)) < 1.0f * kPi / 180.0f) {
                    _flatLegYawTurning = false;
                }
            }
            _flatLegDiag.yawErrMax = (std::max)(_flatLegDiag.yawErrMax, std::abs(wrapPi(bodyYaw - _flatLegYaw)));
        }
        const RE::NiPoint3 fwd(std::sin(_flatLegYaw), std::cos(_flatLegYaw), 0.0f);
        const RE::NiPoint3 right(fwd.y, -fwd.x, 0.0f);
        const RE::NiPoint3 up(0.0f, 0.0f, 1.0f);
        M3                 yawR;  // actor frame -> world, turned to the leg heading
        yawR.m[0][0] = right.x, yawR.m[1][0] = right.y, yawR.m[2][0] = 0.0;
        yawR.m[0][1] = fwd.x, yawR.m[1][1] = fwd.y, yawR.m[2][1] = 0.0;
        yawR.m[0][2] = 0.0, yawR.m[1][2] = 0.0, yawR.m[2][2] = 1.0;

        // origin: the root's x/y (under the camera); standing, with a lean dead band (a seated lean does not step the feet; past
        // it the targets follow until re-centred). Height: the actor's z = the real ground (not the smooth-movement z).
        const RE::NiPoint3 rootW = _root->world.translate;
        if (_flatMoving || !_flatPlantedValid) {
            _flatAnchor = rootW;
            _flatAnchorRecentring = false;
        } else {
            RE::NiPoint3 d = rootW - _flatAnchor;
            d.z = 0.0f;
            const float dl = lenXY(d);
            if (dl > kLeanBand * s) {
                _flatAnchorRecentring = true;
            }
            if (_flatAnchorRecentring) {
                _flatAnchor += d * (std::min)(1.0f, kTurnRate * dt);
                if (dl < 0.5f) {
                    _flatAnchorRecentring = false;
                }
            }
        }
        const float groundZ = player->data.location.z - g_config.getPlayerBodyOffsetUp();

        // animated poses (actor frame), stance narrowed for sneak / weapon drawn standing (VRIK updateWalk)
        Xf          thigh[2] = { FromNi(_flatAnimRel[0]), FromNi(_flatAnimRel[2]) };
        Xf          foot[2] = { FromNi(_flatAnimRel[1]), FromNi(_flatAnimRel[3]) };
        const float narrow = isPlayerSneaking() ? 0.125f : (isWeaponDrawn() && !_flatMoving ? 0.2f : 0.0f);
        if (narrow > 0.0f) {
            const V lr = foot[1].t - foot[0].t;
            foot[0].t.x += lr.x * narrow, foot[0].t.y += lr.y * narrow;
            foot[1].t.x -= lr.x * narrow, foot[1].t.y -= lr.y * narrow;
        }
        // C43 holstered strafe / back: the third-person "relaxed" locomotion only has forward clips (a third-person player turns
        // to face the way it moves; both first-person body mods swap clips for this). In first person the actor keeps facing the
        // camera, so the stride (each foot relative to its own hip) is turned to the engine's move direction (graph "Direction",
        // a fraction of a turn clockwise: measured 0 forward, 0.25 strafe right, 0.5 back, 0.75 left; the moveMode low bits do
        // not separate back from right on flat: 0x5 for both); the hip width, the foot rotation and the knee pole stay with the
        // body. With a weapon drawn the graph's own directional (strafe / backpedal) clips play (measured: drawn strafe = 51 u
        // sideways foot range), so nothing is turned.
        {
            float target = 0.0f;
            float turns = 0.0f;
            if (_flatMoving && !isWeaponDrawn() && bodygraph::engineMoveDirection(turns)) {
                target = wrapPi(turns * 2.0f * kPi);
            }
            const float err = wrapPi(target - _flatStrideYaw);
            _flatStrideYaw = wrapPi(_flatStrideYaw + err * (std::min)(1.0f, dt / 0.2f));  // 0.2 s blend
            _flatLegDiag.strideYawMax = (std::max)(_flatLegDiag.strideYawMax, std::abs(_flatStrideYaw));
        }
        const double strideC = std::cos(_flatStrideYaw), strideS = std::sin(_flatStrideYaw);
        V                            rotV{ 0, 1, 0 };
        std::array<RE::NiPoint3, 2>  tgt;
        std::array<RE::NiMatrix3, 2> tgtRot;
        const RE::NiPoint3           origin(_flatMoving ? rootW.x : _flatAnchor.x, _flatMoving ? rootW.y : _flatAnchor.y, groundZ);
        for (int i = 0; i < 2; ++i) {
            const V& p = foot[i].t;
            const V& q = thigh[i].t;
            // stride (foot relative to the animated hip) at the animation's own scale: the ground passes at the actor's speed;
            // hip width and foot height with the body (C36 root scale)
            const double dx = p.x - q.x, dy = p.y - q.y;                           // stride relative to the hip
            const double sx = dx * strideC + dy * strideS, sy = -dx * strideS + dy * strideC;  // C43: turned to the move direction
            const double mx = s * q.x + sx, my = s * q.y + sy, mz = s * p.z;
            tgt[i] = origin + right * static_cast<float>(mx) + fwd * static_cast<float>(my) + up * static_cast<float>(mz);
            Xf fw;
            fw.R = Mul(yawR, foot[i].R);
            tgtRot[i] = ToNi(fw).rotate;
            if (_inPowerArmor) {
                rotV = V{ 0, 0, i == 0 ? 1.0 : -1.0 };
            }
            _flatPole[i] = toP(Mul(yawR, Mul(thigh[i].R, rotV)));
            if (_flatMoving && i == 0) {  // diag: does the animation move the feet while walking?
                const double c[3] = { p.x, p.y, p.z };
                for (int k = 0; k < 3; ++k) {
                    _flatLegDiag.mvMin[k] = (std::min)(_flatLegDiag.mvMin[k], static_cast<float>(c[k]));
                    _flatLegDiag.mvMax[k] = (std::max)(_flatLegDiag.mvMax[k], static_cast<float>(c[k]));
                }
            }
            if (!_flatMoving) {  // diag: does the animation move the idle feet (turn-in-place shuffle)?
                _flatLegDiag.animMin[i][0] = (std::min)(_flatLegDiag.animMin[i][0], static_cast<float>(p.x));
                _flatLegDiag.animMin[i][1] = (std::min)(_flatLegDiag.animMin[i][1], static_cast<float>(p.y));
                _flatLegDiag.animMax[i][0] = (std::max)(_flatLegDiag.animMax[i][0], static_cast<float>(p.x));
                _flatLegDiag.animMax[i][1] = (std::max)(_flatLegDiag.animMax[i][1], static_cast<float>(p.y));
            }
        }

        ++_flatLegDiag.frames;
        if (_flatMoving) {
            ++_flatLegDiag.moving;
            _flatStepFoot = -1;
            if (_flatAnimBlend < 1.0f) {
                _flatAnimBlend = (std::min)(1.0f, _flatAnimBlend + dt / kStopHold);
                for (int i = 0; i < 2; ++i) {
                    _flatOut[i] = _flatPlanted[i] + (tgt[i] - _flatPlanted[i]) * _flatAnimBlend;
                    _flatOutRot[i] = slerpRot(_flatPlantedRot[i], tgtRot[i], _flatAnimBlend);
                }
            } else {
                _flatOut = tgt;
                _flatOutRot = tgtRot;
            }
            _flatPrevPlanted = { false, false };
        } else {
            if (!_flatPlantedValid) {
                _flatPlanted = tgt;
                _flatPlantedRot = tgtRot;
                _flatPlantedValid = true;
                _flatStepFoot = -1;
            }
            const float tol = kStepTol * s;
            const float d0 = MatrixUtils::vec3Len(_flatPlanted[0] - tgt[0]);
            const float d1 = MatrixUtils::vec3Len(_flatPlanted[1] - tgt[1]);
            if (_flatStepFoot < 0) {
                if ((std::max)(d0, d1) > kSnapDist * s) {  // teleport / load: re-plant
                    _flatPlanted = tgt;
                    _flatPlantedRot = tgtRot;
                    _flatPrevPlanted = { false, false };
                } else if (d0 > tol || d1 > tol) {
                    flatStartStep(std::abs(d0 - d1) < 0.01f ? std::rand() % 2 : (d0 > d1 ? 0 : 1), tgt, s);  // NOLINT(concurrency-mt-unsafe)
                }
            }
            if (_flatStepFoot >= 0) {
                const int i = _flatStepFoot, o = 1 - i;
                _flatStepT = (std::min)(1.0f, _flatStepT + dt / _flatStepDur);
                RE::NiPoint3 pos = _flatStepStart + (tgt[i] - _flatStepStart) * _flatStepT;
                pos.z += std::sin(_flatStepT * kPi) * _flatStepLift;
                _flatOut[i] = pos;
                _flatOutRot[i] = slerpRot(_flatStepStartRot, tgtRot[i], _flatStepT);
                // the other foot slides toward its own target meanwhile (VRIK)
                const RE::NiPoint3 to = tgt[o] - _flatPlanted[o];
                const float        tl = MatrixUtils::vec3Len(to);
                if (tl > 0.001f) {
                    const float m = (std::min)(tl, kSlideSpeed * s * dt);
                    _flatPlanted[o] += to * (m / tl);
                    _flatLegDiag.plantSlideMax = (std::max)(_flatLegDiag.plantSlideMax, m);
                }
                if (_flatStepT >= 1.0f) {
                    _flatPlanted[i] = tgt[i];
                    _flatPlantedRot[i] = tgtRot[i];
                    _flatStepFoot = -1;
                    _flatOut[i] = _flatPlanted[i];
                    if (MatrixUtils::vec3Len(_flatPlanted[o] - tgt[o]) > tol) {
                        flatStartStep(o, tgt, s);
                    }
                }
            }
            for (int i = 0; i < 2; ++i) {
                if (i != _flatStepFoot) {
                    _flatOut[i] = _flatPlanted[i];
                    _flatOutRot[i] = _flatPlantedRot[i];
                }
            }
        }
        _leftFootPos = _flatOut[0];
        _rightFootPos = _flatOut[1];
        _flatFootRot = _flatOutRot;
        _flatFootRotValid = true;
    }

    /**
     * Functional numbers, FRIK.log every 2 s: solved foot vs its target, planted-foot drift, steps, the actor's move bits, the
     * idle animation's foot range while standing (turn-in-place shuffle), the capture check.
     */
    void Skeleton::flatAnimLegsDiag()
    {
        auto&             d = _flatLegDiag;
        const RE::NiNode* feet[2] = { RE::NiNodeUtils::FindNode(_root, "LLeg_Foot"), RE::NiNodeUtils::FindNode(_root, "RLeg_Foot") };
        for (int i = 0; i < 2; ++i) {
            if (!feet[i]) {
                continue;
            }
            const RE::NiPoint3 w = feet[i]->world.translate;
            d.footErrMax[i] = (std::max)(d.footErrMax[i], MatrixUtils::vec3Len(w - _flatOut[i]));
            const bool planted = !_flatMoving && i != _flatStepFoot;
            if (planted) {
                ++d.plantedFrames;
                if (_flatPrevPlanted[i]) {
                    d.plantFootMoveMax = (std::max)(d.plantFootMoveMax, MatrixUtils::vec3Len(w - _flatPrevSolvedFoot[i]));
                }
            }
            _flatPrevSolvedFoot[i] = w;
            _flatPrevPlanted[i] = planted;
        }
        static auto s_t = std::chrono::steady_clock::now();
        const auto  now = std::chrono::steady_clock::now();
        if (now - s_t < std::chrono::seconds(2)) {
            return;
        }
        s_t = now;
        const auto  player = RE::PlayerCharacter::GetSingleton();
        const float groundZ = player ? player->data.location.z - g_config.getPlayerBodyOffsetUp() : 0.0f;
        auto        range = [&](const int i) {
            return d.animMax[i][0] >= d.animMin[i][0] ? std::format("{:.1f}x{:.1f}", d.animMax[i][0] - d.animMin[i][0], d.animMax[i][1] - d.animMin[i][1]) :
                                                               std::string("-");
        };
        logger::info("FRIK-flat C42 legs: {} frames (moving {}) | foot - target max L {:.2f} R {:.2f} u | planted: solved foot move max {:.2f} u/frame over {} foot-frames, "
                     "slide max {:.2f} u/frame | steps {} | moveMode 0x{:X} (window or 0x{:X}) | standing anim foot range L {} R {} u | leg yaw lag max {:.1f} deg | "
                     "scale {:.3f} | root z - ground {:.1f} u | capture check {:.2f} u | moving anim L foot range {} | engine changed L foot {} thigh {} COM {} of {} frames | holstered stride turn max {:.0f} deg",
            d.frames, d.moving, d.footErrMax[0], d.footErrMax[1], d.plantFootMoveMax, d.plantedFrames, d.plantSlideMax, d.steps, d.moveBits, d.moveBitsOr,
            range(0), range(1), d.yawErrMax * 180.0f / kPi, _root->world.scale, _root->world.translate.z - groundZ, d.capErr,
            d.mvMax[0] >= d.mvMin[0] ? std::format("{:.1f}x{:.1f}x{:.1f}", d.mvMax[0] - d.mvMin[0], d.mvMax[1] - d.mvMin[1], d.mvMax[2] - d.mvMin[2]) : std::string("-"),
            d.engChanged, d.thighChanged, d.comChanged, d.engFrames, d.strideYawMax * 180.0f / kPi);
        d = FlatLegDiag{};
    }
#endif

    void Skeleton::rotateLeg(const uint32_t pos, const float angle) const
    {
        const auto rt = reinterpret_cast<f4vr::BSFlattenedBoneTree*>(_root);

        auto& transform = rt->transforms[pos];
        transform.local.rotate = MatrixUtils::getMatrixFromEulerAngles(MatrixUtils::degreesToRads(angle), 0, 0) * transform.local.rotate;

        const auto& parentTransform = rt->transforms[transform.parPos];
        const RE::NiPoint3 p = parentTransform.world.rotate * (transform.local.translate * parentTransform.world.scale);
        transform.world.translate = parentTransform.world.translate + p;

        transform.world.rotate = transform.local.rotate * parentTransform.world.rotate;
    }

    /**
     * Hide the 3rd-person weapon that comes with the skeleton as we are using the 1st-person weapon model.
     */
    void Skeleton::hide3rdPersonWeapon() const
    {
        if (RE::NiAVObject* weapon = RE::NiNodeUtils::FindNode(_rightArm.hand, "Weapon", 1)) {
            RE::NiNodeUtils::SetVisible(weapon, false);
        }
    }

    void Skeleton::hideFistHelpers() const
    {
        if (!RE::VRSettings::IsLeftHandedMode()) {
            RE::NiAVObject* node = RE::NiNodeUtils::FindNode(_playerNodes->primaryWandNode, "fist_M_Right_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true); // first bit sets the cull flag so it will be hidden;
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->primaryWandNode, "fist_F_Right_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->primaryWandNode, "PA_fist_R_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "fist_M_Left_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true); // first bit sets the cull flag so it will be hidden;
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "fist_F_Left_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "PA_fist_L_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }
        } else {
            RE::NiAVObject* node = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "fist_M_Right_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true); // first bit sets the cull flag so it will be hidden;
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "fist_F_Right_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "PA_fist_R_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->primaryWandNode, "fist_M_Left_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true); // first bit sets the cull flag so it will be hidden;
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->primaryWandNode, "fist_F_Left_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }

            node = RE::NiNodeUtils::FindNode(_playerNodes->primaryWandNode, "PA_fist_L_HELPER");
            if (node != nullptr) {
                vrshim::cull::Set(node, true);
            }
        }

        if (const auto uiNode = RE::NiNodeUtils::FindNode(_playerNodes->secondaryWandNode, "Point002")) {
            uiNode->local.scale = 0.0;
        }
    }

    void Skeleton::showHidePAHud() const
    {
        if (const auto hud = RE::NiNodeUtils::FindNode(_playerNodes->roomNode, "PowerArmorHelmetRoot")) {
            hud->local.scale = g_config.showPAHUD ? 1.0f : 0.0f;
        }
    }

    /**
     * Switch right and left weapon nodes if left-handed mode is enabled to correctly the hands.
     * Remember the setting to set back if settings change while game is running.
     */
    /**
     * Set a node's local so its world stays as given under its current parent (world = local * parent).
     */
    static void setLocalFromWorld(RE::NiAVObject* node, const RE::NiTransform& world)
    {
        if (!node || !node->parent) {
            return;
        }
        const auto& parent = node->parent->world;
        node->local.rotate = world.rotate * parent.rotate.Transpose();
        node->local.translate = parent.rotate * ((world.translate - parent.translate) / parent.scale);
        node->local.scale = world.scale / parent.scale;
        node->world = world;
    }

    void Skeleton::handleLeftHandedWeaponNodesSwitch()
    {
        const bool effectiveLeftHanded = g_frik.isWeaponInLeftHand();
        if (_lastLeftHandedModeSwitch == effectiveLeftHanded) {
            return;
        }

        RE::NiNode* rightWeapon = getWeaponNode();
        RE::NiNode* leftWeapon = _playerNodes->weaponLeftNode;
#ifdef FRIK_FLAT // C19
        const auto rHand = vrshim::fpHands().rHand;
        const auto lHand = vrshim::fpHands().lHand;
#else
        const auto rHand = RE::NiNodeUtils::FindNode(getFirstPersonSkeleton(), "RArm_Hand");
        const auto lHand = RE::NiNodeUtils::FindNode(getFirstPersonSkeleton(), "LArm_Hand");
#endif

        if (!rightWeapon || !rHand || !leftWeapon || !lHand) {
            logger::sample("Cannot set up weapon nodes for left-handed mode switch");
            return;
        }

        _lastLeftHandedModeSwitch = effectiveLeftHanded;
        logger::warn("Left-handed mode weapon nodes switch (EffectiveLeftHanded:{}, GameSetting:{})", effectiveLeftHanded, RE::VRSettings::IsLeftHandedMode());

        // keep each weapon node where it is in the world across the reparent, so an external owner's placement
        // (and the game's own) does not jump on the switch frame; FRIK's re-glue or the owner writes it afterwards
        const RE::NiTransform rightWeaponWorld = rightWeapon->world;
        const RE::NiTransform leftWeaponWorld = leftWeapon->world;

        rHand->DetachChild(rightWeapon);
        rHand->DetachChild(leftWeapon);
        lHand->DetachChild(rightWeapon);
        lHand->DetachChild(leftWeapon);

        if (effectiveLeftHanded) {
            rHand->AttachChild(leftWeapon, true);
            lHand->AttachChild(rightWeapon, true);
        } else {
            rHand->AttachChild(rightWeapon, true);
            lHand->AttachChild(leftWeapon, true);
        }
        setLocalFromWorld(rightWeapon, rightWeaponWorld);
        setLocalFromWorld(leftWeapon, leftWeaponWorld);
    }

    bool Skeleton::repairEngineArmPlacementForCarry(RE::NiNode* weapon) const
    {
        if (!weapon || !_playerNodes || g_frik.isWeaponInLeftHand() == RE::VRSettings::IsLeftHandedMode()) {
            return false;
        }
        // only while a carry needs the repair; a root of its own, since the engine places the arms after the mod's frame
        F4CF_PERF_FUNCTION();
        // the pairing updateHandTarget uses: the node hanging under the other hand goes to that hand's offset, with that hand's glue
        if (weapon == getWeaponNode()) {
            placeFirstPersonArm(weapon, _playerNodes->secondaryMeleeWeaponOffsetNode, !RE::VRSettings::IsLeftHandedMode(), true);
            return true;
        }
        if (weapon == _playerNodes->weaponLeftNode) {
            placeFirstPersonArm(weapon, _playerNodes->primaryWeaponOffsetNode, RE::VRSettings::IsLeftHandedMode(), false);
            return true;
        }
        return false;
    }

    // Bring this hand's weapon and offset nodes and the first-person hand up to date for the frame (the target solveArm uses).
    void Skeleton::updateHandTarget(bool isLeft)
    {
        // This first part is to handle the game calculating the first person hand based off two offset nodes
        // PrimaryWeaponOffset and PrimaryMeleeOffset
        // Unfortunately neither of these two nodes are that close to each other so when you equip a melee or ranged weapon
        // the hand will jump which completely messes up the solver and looks bad to boot.
        // So this code below does a similar operation as the in game function that solves the first person arm by forcing
        // everything to go to the PrimaryWeaponNode.  I have hardcoded a rotation below based off one of the guns that
        // matches my real life hand pose with an index controller very well.   I use this as the baseline for everything

        if (getFirstPersonSkeleton() == nullptr) {
            return;
        }

        RE::NiNode* rightWeapon = getWeaponNode();
        //RE::NiNode* rightWeapon = _playerNodes->primaryWandNode;
        RE::NiNode* leftWeapon = _playerNodes->weaponLeftNode; // "WeaponLeft" can return incorrect node for left-handed with throwable weapons

        // handle the NON-primary hand (i.e. the hand that is NOT holding the weapon)
        bool handleOffhand = RE::VRSettings::IsLeftHandedMode() ^ isLeft;

        RE::NiNode* weaponNode = handleOffhand ? leftWeapon : rightWeapon;
        RE::NiNode* offsetNode = handleOffhand ? _playerNodes->secondaryMeleeWeaponOffsetNode : _playerNodes->primaryWeaponOffsetNode;

        if (g_frik.isWeaponInLeftHand() != RE::VRSettings::IsLeftHandedMode()) {
            // the weapon node is parented under the other hand than the game setting says (external left-carry)
            weaponNode = handleOffhand ? rightWeapon : leftWeapon;
        }

        if (handleOffhand) {
            _playerNodes->secondaryMeleeWeaponOffsetNode->local = _playerNodes->primaryWeaponOffsetNode->local;
            _playerNodes->secondaryMeleeWeaponOffsetNode->local.rotate =
                _playerNodes->secondaryMeleeWeaponOffsetNode->local.rotate * MatrixUtils::getMatrixFromEulerAngles(0, MatrixUtils::degreesToRads(180.0f), 0);
            _playerNodes->secondaryMeleeWeaponOffsetNode->local.translate = RE::NiPoint3(-2, -9, 2);
            RE::NiNodeUtils::UpdateTransformFromParent(_playerNodes->secondaryMeleeWeaponOffsetNode);
        }

        const WeaponHandRecoil::ScopedNativeKickNeutralizer neutralizeNativeKick(_weaponHandRecoil);
        dampenHand(offsetNode, isLeft);
        weaponNode->IncRefCount();
        placeFirstPersonArm(weaponNode, offsetNode, isLeft, handleOffhand);
    }

    // Present FRIK's glue on the weapon node and run the engine's first-person arm placement, so the hand lands at the controller regardless
    // of the weapon's own offset. An external owner of the primary weapon node keeps its transform: the glue is presented for the placement
    // only and the owner's transform re-derived under the updated hand afterwards. The caller holds the reference the engine call releases.
    void Skeleton::placeFirstPersonArm(RE::NiNode* weaponNode, RE::NiNode* offsetNode, const bool isLeft, const bool handleOffhand) const
    {
        const bool ownedExternally = weaponNode == getWeaponNode() && g_externalAuthority.isPrimaryWeaponNodeOwnershipBlocked();
        const RE::NiTransform ownerLocal = weaponNode->local;
#ifdef FRIK_FLAT // C29: the weapon local FRIK's weapon adjuster left last frame (per-weapon JSON offset: weapon in the hand)
        if (weaponNode == getWeaponNode()) {
            vrshim::setPrimaryWeaponInHand(weaponNode->local, !isUnarmedWeaponDrawn());
        }
#endif

        weaponNode->local.rotate = !RE::VRSettings::IsLeftHandedMode() ? MatrixUtils::getMatrix(-0.122f, 0.987f, 0.100f, 0.990f, 0.114f, 0.081f, 0.069f, 0.109f, -0.992f)
                                                                       : MatrixUtils::getMatrix(-0.122f, 0.987f, 0.100f, -0.990f, -0.114f, -0.081f, -0.069f, -0.109f, 0.992f);

        if (handleOffhand) {
            weaponNode->local.rotate = weaponNode->local.rotate * MatrixUtils::getMatrixFromEulerAngles(0, MatrixUtils::degreesToRads(isLeft ? 45.0f : -45.0f), 0);
        }

        weaponNode->local.translate = RE::VRSettings::IsLeftHandedMode() ? (isLeft ? RE::NiPoint3(3.389f, -2.099f, 3.133f) : RE::NiPoint3(0, -4.8f, 0))
                                      : isLeft                           ? RE::NiPoint3(0, 0, 0)
                                                                         : RE::NiPoint3(4.389f, -1.899f, -3.133f);

#ifdef FRIK_FLAT // C14: VR engine function EF6280 has no flat counterpart
        vrshim::update1stPersonArm(RE::PlayerCharacter::GetSingleton(), &weaponNode, &offsetNode);
#else
        RE::PlayerCharacter::GetSingleton()->Update1stPersonArm(&weaponNode, &offsetNode);
#endif

        if (ownedExternally) {
            weaponNode->local = ownerLocal;
            RE::NiNodeUtils::UpdateTransformsDown(weaponNode, true);
        }
    }

    namespace
    {
        // Smoothers are tuned as a per-frame retention at 90 Hz; over a frame of dt the same time constant keeps r^(dt*90).
        // dt is clamped to three nominal frames so a hitch moves a smoother at most that far instead of flinging it.
        float smoothingMoveFraction(const float retentionPerFrameAt90Hz, const float dt, const bool frameRateIndependent)
        {
            const float retention = std::clamp(retentionPerFrameAt90Hz, 0.0f, 0.999f);
            if (!frameRateIndependent) {
                return 1.0f - retention;
            }
            constexpr float kMaxDt = 3.0f / 90.0f;
            return 1.0f - std::pow(retention, std::clamp(dt, 0.0f, kMaxDt) * 90.0f);
        }

        // Whether a transform was moved at all, translation or rotation. Used to tell a recoil kick that
        // actually displaced a hand target from one that had nothing to apply.
        bool transformsEqual(const RE::NiTransform& a, const RE::NiTransform& b)
        {
            if (!fEqual(a.translate.x, b.translate.x) || !fEqual(a.translate.y, b.translate.y) || !fEqual(a.translate.z, b.translate.z)) {
                return false;
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    if (!fEqual(a.rotate.entry[i][j], b.rotate.entry[i][j])) {
                        return false;
                    }
                }
            }
            return true;
        }

        // max(a, b) outside a band of width k around a == b, C1 across it (polynomial smooth maximum)
        float smoothMax(const float a, const float b, const float k)
        {
            const float h = (std::max)(k - std::abs(a - b), 0.0f) / k;
            return (std::max)(a, b) + h * h * k * 0.25f;
        }

        float smoothMin(const float a, const float b, const float k)
        {
            return -smoothMax(-a, -b, k);
        }

    }

    // This is the main arm IK solver function - Algo credit to prog from SkyrimVR VRIK mod - what a beast!
    void Skeleton::solveArm(bool isLeft)
    {
        if (getFirstPersonSkeleton() == nullptr) {
            return;
        }

        // An external mod can own the hand instead of the tracked controller, but only as the target
        // handed to the same solver, so everything downstream of the arm sees one consistent result.
        RE::NiTransform handWorldTarget;
        const bool hasTransformOverride = g_externalAuthority.getHandWorldTransform(isLeft, handWorldTarget);
        if (!hasTransformOverride) {
            handWorldTarget = isLeft ? _leftHand->world : _rightHand->world;
        }
        const RE::NiTransform targetWithoutRecoil = handWorldTarget;
        (void)_weaponHandRecoil.applyToHandWorldTarget(isLeft, handWorldTarget);
        // Its bool is "no error", returned both when it applied a kick and when it had nothing to apply for this
        // hand, so the only sound test of whether the kick moved THIS hand is the target it was asked to move.
        const bool recoilMovedTarget = !transformsEqual(targetWithoutRecoil, handWorldTarget);
        auto& solveState = _handSolveState[isLeft ? 0 : 1];
        solveState = hasTransformOverride ? HandSolveState::Consumed : HandSolveState::NoClaim;
        if (solveArmToHandWorldTarget(isLeft, handWorldTarget)) {
            return;
        }
        if (!hasTransformOverride) {
            // The recoil delta is applied before the reach gates, so a large kick can push an otherwise reachable target past them;
            // rather than leave the arm unsolved for that frame, solve to the target without the kick.
            if (recoilMovedTarget) {
                restoreArmNodesToDefault(isLeft);
                (void)solveArmToHandWorldTarget(isLeft, targetWithoutRecoil);
            }
            return;
        }

        // An unreachable target makes the solver bail after it has already rotated the collarbone. For the
        // tracked hand that only ever happens on a dropped frame of tracking and the next frame reset clears
        // it, but an override holds its target until the client clears it, so the arm would stay half solved
        // for as long as it is set. Solve to the tracked hand instead, as if no tag owned this hand.
        solveState = HandSolveState::Unreachable;
        restoreArmNodesToDefault(isLeft);
        RE::NiTransform trackedHandTarget = isLeft ? _leftHand->world : _rightHand->world;
        (void)_weaponHandRecoil.applyToHandWorldTarget(isLeft, trackedHandTarget);
        (void)solveArmToHandWorldTarget(isLeft, trackedHandTarget);
    }

    /**
     * Write this frame's upper-arm roll split onto the UpperTwist1 tree entries (rest local rotated about the bone axis) and refresh
     * their worlds from the upper arm, which is final here. Writing from the rest each frame means nothing accumulates.
     */
    void Skeleton::applyUpperTwist()
    {
        // Off by default: the tree is not touched at all unless the split is enabled (a launch on 53d9b9a coincided with ROCK reading the
        // flattened hand entries thousands of units off for a minute after load; until that is explained the write stays opt-in)
        if (!(g_config.armUpperTwistSplit > 0.0f)) {
            return;
        }
        auto* tree = f4vr::getFlattenedBoneTree();
        if (!tree) {
            return;
        }
        for (const bool isLeft : { true, false }) {
            const auto side = isLeft ? 0 : 1;
            const int idx = _upperTwistTreeIdx[side];
            if (idx < 0 || idx >= tree->numTransforms) {
                continue;
            }
            auto& bone = tree->transforms[idx];
            bone.local.rotate = MatrixUtils::getMatrixFromEulerAngles(_upperTwistRoll[side], 0, 0) * _upperTwistRestRotate[side];
            if (const auto* upper = getArm(isLeft).upper) {
                bone.world.translate = upper->world.translate + upper->world.rotate.Transpose() * (bone.local.translate * upper->world.scale);
                bone.world.rotate = bone.local.rotate * upper->world.rotate;
                bone.world.scale = bone.local.scale * upper->world.scale;
            } else if (bone.parPos >= 0 && bone.parPos < tree->numTransforms) {
                const auto& parentWorld = tree->transforms[bone.parPos].world;
                bone.world.translate = parentWorld.translate + parentWorld.rotate.Transpose() * (bone.local.translate * parentWorld.scale);
                bone.world.rotate = bone.local.rotate * parentWorld.rotate;
                bone.world.scale = bone.local.scale * parentWorld.scale;
            }
        }
    }

    void Skeleton::latchRenderedWrists()
    {
        for (const bool isLeft : { true, false }) {
            if (const auto* hand = getArm(isLeft).hand) {
                _renderedWrist[isLeft ? 0 : 1] = hand->world;
            }
        }
    }

    /**
     * World transform of a bone from the flattened bone tree, final after updateWorldFinal.
     */
    bool Skeleton::getBoneWorldTransform(const std::string_view boneName, RE::NiTransform& outTransform) const
    {
        const auto it = _boneIndexByName.find(std::string(boneName));
        const auto* tree = f4vr::getFlattenedBoneTree();
        if (it == _boneIndexByName.end() || !tree || it->second < 0 || it->second >= tree->numTransforms) {
            return false;
        }
        outTransform = tree->transforms[it->second].world;
        return true;
    }

    /**
     * Reset one arm chain to its default local transforms and refresh the world transforms below it,
     * undoing a partial solve so the arm can be solved again from a clean state.
     */
    void Skeleton::restoreArmNodesToDefault(const bool isLeft)
    {
        const ArmNodes arm = isLeft ? _leftArm : _rightArm;
        const std::array<RE::NiAVObject*, 7> armChain{ arm.shoulder, arm.upper, arm.upperT1, arm.forearm1, arm.forearm2, arm.forearm3, arm.hand };

        for (auto* armNode : armChain) {
            if (!armNode) {
                continue;
            }
            for (const auto& [boneNode, resetTransform] : _skeletonNodesToDefaultTransforms) {
                if (boneNode == armNode) {
                    armNode->local = resetTransform;
                    break;
                }
            }
        }
        RE::NiNodeUtils::UpdateWorldDataDown(arm.shoulder, true);
    }

    bool Skeleton::solveArmToHandWorldTarget(const bool isLeft, const RE::NiTransform& handWorldTarget)
    {
        if (!isFiniteTransform(handWorldTarget)) {
            return false;
        }

        RE::NiPoint3 handPos = handWorldTarget.translate;
        RE::NiMatrix3 handRot = handWorldTarget.rotate;

        const auto arm = isLeft ? _leftArm : _rightArm;
        if (!arm.shoulder || !arm.upper || !arm.forearm1 || !arm.hand || (!_inPowerArmor && (!arm.forearm2 || !arm.forearm3))) {
            return false;
        }

        // Detect if the 1st person hand position is invalid. This can happen when a controller loses tracking.
        // If it is, do not handle IK and let Fallout use its normal animations for that arm instead.
        if (isnan(handPos.x) || isnan(handPos.y) || isnan(handPos.z) || isinf(handPos.x) || isinf(handPos.y) || isinf(handPos.z) ||
            MatrixUtils::vec3Len(arm.upper->world.translate - handPos) > 200.0) {
            return false;
        }

#ifdef FRIK_FLAT // 20 Nov no-scaling switch: the arm bones keep the skeleton's own lengths (FRIK.toml armLength scales them)
        const float adjustedArmLength = vrshim::bridge::noScaling() ? 1.0f : g_config.armLength / 36.74f;
#else
        const float adjustedArmLength = g_config.armLength / 36.74f;
#endif
        const float armLength = g_config.armLength;

        // Bone lengths and the reach gate come first: a bail-out must leave the arm exactly as it was, not with a rotated collarbone
        const float originalUpperLen = MatrixUtils::vec3Len(arm.forearm1->local.translate);
        float originalForearmLen;

        if (_inPowerArmor) {
            originalForearmLen = MatrixUtils::vec3Len(arm.hand->local.translate);
        } else {
            originalForearmLen =
                MatrixUtils::vec3Len(arm.hand->local.translate) + MatrixUtils::vec3Len(arm.forearm2->local.translate) + MatrixUtils::vec3Len(arm.forearm3->local.translate);
        }
        float upperLen = originalUpperLen * adjustedArmLength;
        float forearmLen = originalForearmLen * adjustedArmLength;

        const RE::NiPoint3 shoulderToHand = handPos - arm.upper->world.translate;
        if (MatrixUtils::vec3Len(shoulderToHand) > (upperLen + forearmLen) * 2.25f) {
            return false;
        }

        // Shoulder IK is done in a very simple way

        float adjustAmount = (std::clamp)(MatrixUtils::vec3Len(shoulderToHand) - armLength * 0.5f, 0.0f, armLength * 0.85f) / (armLength * 0.85f);
        RE::NiPoint3 shoulderOffset = MatrixUtils::vec3Norm(shoulderToHand) * (adjustAmount * armLength * g_config.armShoulderReachFraction);
        if (shoulderOffset.z < 0.0f) {
            shoulderOffset.z *= g_config.armShoulderDownwardDamp;
        }

        RE::NiPoint3 clavicalToNewShoulder = arm.upper->world.translate + shoulderOffset - arm.shoulder->world.translate;

        RE::NiPoint3 sLocalDir = arm.shoulder->world.rotate * (clavicalToNewShoulder / arm.shoulder->world.scale);

        RE::NiMatrix3 result = MatrixUtils::getMatrixFromRotateVectorVec(sLocalDir, RE::NiPoint3(1, 0, 0)) * arm.shoulder->local.rotate;
        arm.shoulder->local.rotate = result;

        RE::NiNodeUtils::UpdateWorldDataDown(arm.shoulder, true);
        // the frame the upper-arm twist is measured against below; the worlds are not refreshed again until the solve is done
        const RE::NiMatrix3 preAimUpperWorldRot = arm.upper->world.rotate;

        // The bend of the arm depends on its distance to the body.  Its distance as well as the lengths of
        // the upper arm and forearm define the sides of a triangle:
        //                 ^
        //                /|\         Let a,b be the arm lengths, c be the distance from hand-to-shoulder
        //               /^| \        Let A be the total angle at which the wrist must bend
        //              / ||  \       Let x be the width of the right triangle
        //            a/  y|   \  b   Let y be the height of the right triangle
        //            /   ||    \
        //           /    v|<-x->\
        // Shoulder /______|_____A\ Hand
        //                c
        // Law of cosines: Wrist angle A = acos( (b^2 + c^2 - a^2) / (2*b*c) )
        // The wrist angle is used to calculate x and y, which are used to position the elbow

        float negLeft = isLeft ? -1.0f : 1.0f;

        RE::NiPoint3 Uwp = arm.upper->world.translate;
        RE::NiPoint3 handToShoulder = Uwp - handPos;
        float hsLen = (std::max)(MatrixUtils::vec3Len(handToShoulder), 0.1f);

        // Reach. With a flexion floor the bones stretch so the hand is reached with the elbow still bent by that much, on every
        // frame: a straight arm is where the elbow twist below loses its reference and spins. Without one, the old stretch with
        // its 0.1 slack applies (the slack was the only guard, and only on the stretch branch).
        const float minFlexion = MatrixUtils::degreesToRads(g_config.armElbowMinFlexionDeg);
        if (minFlexion > 0.0f) {
            const float reachAtMinFlexion = sqrtf(upperLen * upperLen + forearmLen * forearmLen + 2.0f * upperLen * forearmLen * cosf(minFlexion));
            if (hsLen > reachAtMinFlexion) {
                const float stretch = hsLen / reachAtMinFlexion;
                upperLen *= stretch;
                forearmLen *= stretch;
            }
        } else if (hsLen > upperLen + forearmLen) {
            float diff = hsLen - upperLen - forearmLen;
            float ratio = forearmLen / (forearmLen + upperLen);
            forearmLen += ratio * diff + 0.1f;
            upperLen += (1.0f - ratio) * diff + 0.1f;
        }

        // Near the shoulder the elbow bends at most the ceiling; the triangle is solved for that reach and the upper arm
        // shortens continuously (its length is realised from the elbow position below) instead of both bones flipping to their mean.
        float triangleLen = hsLen;
        const float maxFlexion = MatrixUtils::degreesToRads(g_config.armElbowMaxFlexionDeg);
        if (maxFlexion > 0.0f) {
            const float reachAtMaxFlexion = sqrtf((std::max)(upperLen * upperLen + forearmLen * forearmLen + 2.0f * upperLen * forearmLen * cosf(maxFlexion), 0.0f));
            triangleLen = (std::max)(hsLen, reachAtMaxFlexion);
        }

        RE::NiPoint3 forwardDir = MatrixUtils::vec3Norm(_forwardDir);
        RE::NiPoint3 sidewaysDir = MatrixUtils::vec3Norm(_sidewaysRDir * negLeft);

        // The primary twist angle comes from the direction the wrist is pointing into the forearm
        RE::NiPoint3 handBack = handRot.Transpose() * (RE::NiPoint3(-1, 0, 0));
        float twistAngle = asinf((std::clamp)(handBack.z, -0.999f, 0.999f));

        // The second twist angle comes from a side vector pointing "outward" from the side of the wrist
        RE::NiPoint3 handSide = handRot.Transpose() * (RE::NiPoint3(0, -1, 0));
        RE::NiPoint3 handInSide = handSide * negLeft;
        float twistAngle2 = -1 * asinf((std::clamp)(handSide.z, -0.599f, 0.999f));

        // Blend the two twist angles together, using the primary angle more when the wrist is pointing downward
        //float interpTwist = (std::clamp)((handBack.z + 0.866f) * 1.155f, 0.25f, 0.8f); // 0 to 1 as hand points 60 degrees down to horizontal
        float interpTwist = (std::clamp)((handBack.z + 0.866f) * 1.155f, 0.45f, 0.8f); // 0 to 1 as hand points 60 degrees down to horizontal
        //		logger::info("%2f %2f %2f", rads_to_degrees(twistAngle), rads_to_degrees(twistAngle2), interpTwist);
        twistAngle = twistAngle + interpTwist * (twistAngle2 - twistAngle);
        // Wonkiness is bad.  Interpolate twist angle towards zero to correct it when the angles are pointed a certain way.
        /*	float fixWonkiness1 = (std::clamp)(vec3_dot(handSide, vec3_norm(-sidewaysDir - forwardDir * 0.25f + RE::NiPoint3(0, 0, -0.25f))), 0.0f, 1.0f);
            float fixWonkiness2 = 1.0f - (std::clamp)(vec3_dot(handBack, vec3_norm(forwardDir + sidewaysDir)), 0.0f, 1.0f);
            twistAngle = twistAngle + fixWonkiness1 * fixWonkiness2 * (-PI / 2.0f - twistAngle);*/

        //		logger::info("final angle %2f", rads_to_degrees(twistAngle));

        // Smooth out sudden changes in the twist angle over time to reduce elbow shake (retention 0.75 per frame at 90 Hz)
        const auto side = isLeft ? 0 : 1;
        const float twistMove = smoothingMoveFraction(0.75f, _frameTime, g_config.armFrameRateIndependentSmoothing);
        twistAngle = _twistAnglePrevFrame[side] + (twistAngle - _twistAnglePrevFrame[side]) * twistMove;
        _twistAngleThisFrame[side] = twistAngle;

        // Calculate the hand's distance behind the body - It will increase the minimum elbow rotation angle
        float size = 1.0;
        float behindD = -(forwardDir.x * arm.shoulder->world.translate.x + forwardDir.y * arm.shoulder->world.translate.y) - 10.0f;
        float handBehindDist = -(handPos.x * forwardDir.x + handPos.y * forwardDir.y + behindD);
        float behindAmount = (std::clamp)(handBehindDist / (40.0f * size), 0.0f, 1.0f);

        // Holding hands in front of chest increases the minimum elbow rotation angle (elbows lift) and decreases the maximum angle
        RE::NiPoint3 planeDir = MatrixUtils::rotateXY(forwardDir, negLeft * MatrixUtils::degreesToRads(135));
        float planeD = -(planeDir.x * arm.shoulder->world.translate.x + planeDir.y * arm.shoulder->world.translate.y) + 16.0f * size;
        float armCrossAmount = (std::clamp)((handPos.x * planeDir.x + handPos.y * planeDir.y + planeD) / (20.0f * size), 0.0f, 1.0f);

        // The arm lift limits how much the crossing amount can influence minimum elbow rotation
        // The maximum rotation is also decreased as hands lift higher (elbows point further downward)
        float armLiftLimitZ = _chest->world.translate.z * size;
        float armLiftThreshold = 60.0f * size;
        float armLiftLimit = (std::clamp)((armLiftLimitZ + armLiftThreshold - handPos.z) / armLiftThreshold, 0.0f, 1.0f); // 1 at bottom, 0 at top
        float upLimit = (std::clamp)((1.0f - armLiftLimit) * 1.4f, 0.0f, 1.0f); // 0 at bottom, 1 at a much lower top

        // Determine overall amount the elbows minimum rotation will be limited. The blends are smooth across their switch points
        // so the elbow's angular velocity does not step when a pose crosses one (bands: a tenth of the amount range, 9 degrees).
        constexpr float kAmountBand = 0.1f;
        const float kAngleBand = MatrixUtils::degreesToRads(9.0f);
        const bool smoothBlends = g_config.armSmoothBlends;
        float adjustMinAmount = smoothBlends ? smoothMax(behindAmount, smoothMin(armCrossAmount, armLiftLimit, kAmountBand), kAmountBand)
                                             : (std::max)(behindAmount, (std::min)(armCrossAmount, armLiftLimit));

        // Get the minimum and maximum angles at which the elbow is allowed to twist
        float twistMinAngle = MatrixUtils::degreesToRads(-85.0) + MatrixUtils::degreesToRads(50) * adjustMinAmount;
        const float crossPenalty = MatrixUtils::degreesToRads(90) * armCrossAmount;
        const float liftPenalty = MatrixUtils::degreesToRads(70) * upLimit;
        float twistMaxAngle = MatrixUtils::degreesToRads(55.0) - (smoothBlends ? smoothMax(crossPenalty, liftPenalty, kAngleBand) : (std::max)(crossPenalty, liftPenalty));
        // the window never closes, so the wrist keeps steering the elbow through a low-ready or a reload
        twistMaxAngle = (std::max)(twistMaxAngle, twistMinAngle + MatrixUtils::degreesToRads(g_config.armTwistWindowMinDeg));

        // Twist angle ranges from -PI/2 to +PI/2; map that range to go from the minimum to the maximum instead
        float twistLimitAngle = twistMinAngle + (twistAngle + std::numbers::pi_v<float> / 2.0f) / std::numbers::pi_v<float> * (twistMaxAngle - twistMinAngle);

        //logger::info("{} {} {} {}", rads_to_degrees(twistAngle), rads_to_degrees(twistAngle2), rads_to_degrees(twistAngle), rads_to_degrees(twistLimitAngle));
        // The bendDownDir vector points in the direction the player faces, and bends up/down with the final elbow angle
        RE::NiMatrix3 rot = MatrixUtils::getRotationAxisAngle(sidewaysDir * negLeft, twistLimitAngle);
        RE::NiPoint3 bendDownDir = rot.Transpose() * (forwardDir);

        // Get the "X" direction vectors pointing to the shoulder
        RE::NiPoint3 xDir = MatrixUtils::vec3Norm(handToShoulder);

        // Get the final "Y" vector, perpendicular to "X", and pointing in elbow direction (as in the diagram above)
        float sideD = -(sidewaysDir.x * arm.shoulder->world.translate.x + sidewaysDir.y * arm.shoulder->world.translate.y) - 1.0f * 8.0f;
        float acrossAmount = -(handPos.x * sidewaysDir.x + handPos.y * sidewaysDir.y + sideD) / (16.0f * 1.0f);
        float handSideTwistOutward = MatrixUtils::vec3Dot(handSide, MatrixUtils::vec3Norm(sidewaysDir + forwardDir * 0.5f));
        float armTwist = (std::clamp)(handSideTwistOutward - (std::max)(0.0f, acrossAmount + 0.25f), 0.0f, 1.0f);

        if (acrossAmount < 0) {
            acrossAmount *= 0.2f;
        }

        float handBehindHead = (std::clamp)((handBehindDist + 0.0f * size) / (15.0f * size), 0.0f, 1.0f) * (std::clamp)(upLimit * 1.2f, 0.0f, 1.0f);
        const float acrossTwist = acrossAmount * MatrixUtils::degreesToRads(90);
        const float behindHeadTwist = handBehindHead * MatrixUtils::degreesToRads(120);
        float elbowsTwistForward = smoothBlends ? smoothMax(acrossTwist, behindHeadTwist, kAngleBand) : (std::max)(acrossTwist, behindHeadTwist);
        RE::NiPoint3 elbowDir = MatrixUtils::rotateXY(bendDownDir, -negLeft * (MatrixUtils::degreesToRads(150) - armTwist * MatrixUtils::degreesToRads(25) - elbowsTwistForward));
        RE::NiPoint3 yDir = elbowDir - xDir * MatrixUtils::vec3Dot(elbowDir, xDir);
        yDir = MatrixUtils::vec3Norm(yDir);

        // Get the angle wrist must bend to reach elbow position, for the (possibly ceiling-clamped) shoulder-to-hand length.
        // If that is still impossible (hand too close to shoulder with the ceiling off), set forearmLen = upperLen so there is always a solution
        float wristAngle = acosf((forearmLen * forearmLen + triangleLen * triangleLen - upperLen * upperLen) / (2 * forearmLen * triangleLen));
        if (isnan(wristAngle) || isinf(wristAngle)) {
            forearmLen = upperLen = (originalUpperLen + originalForearmLen) / 2.0f * adjustedArmLength;
            wristAngle = acosf((forearmLen * forearmLen + triangleLen * triangleLen - upperLen * upperLen) / (2 * forearmLen * triangleLen));
        }

        // Get the desired world coordinate of the elbow
        float xDist = cosf(wristAngle) * forearmLen;
        float yDist = sinf(wristAngle) * forearmLen;
        RE::NiPoint3 elbowWorld = handPos + xDir * xDist + yDir * yDist;

        // This code below rotates and positions the upper arm, forearm, and hand bones
        // Notation: C=Clavicle, U=Upper arm, F=Forearm, H=hand   w=world, l=local   p=position, r=rotation, s=scale
        //    Rules: World position = Parent world pos + Parent world rot * (Local pos * Parent World scale)
        //           World Rotation = Parent world rotation * Local Rotation
        // ---------------------------------------------------------------------------------------------------------

        // The upper arm bone must be rotated from its forward vector to its shoulder-to-elbow vector in its local space
        // Calculate Ulr:  baseUwr * rotTowardElbow = Cwr * Ulr   ===>   Ulr = Cwr' * baseUwr * rotTowardElbow
        RE::NiMatrix3 Uwr = arm.upper->world.rotate;
        RE::NiPoint3 pos = elbowWorld - Uwp;
        RE::NiPoint3 uLocalDir = Uwr * (MatrixUtils::vec3Norm(pos) / arm.upper->world.scale);

        arm.upper->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(uLocalDir, arm.forearm1->local.translate) * arm.upper->local.rotate;

        Uwr = arm.upper->local.rotate * arm.shoulder->world.rotate;

        // Find the angle of the forearm twisted around the upper arm and twist the upper arm to align it
        //    Uwr * twist = Cwr * Ulr   ===>   Ulr = Cwr' * Uwr * twist
        pos = handPos - elbowWorld;
        RE::NiPoint3 uLocalTwist = Uwr * (MatrixUtils::vec3Norm(pos));
        uLocalTwist.x = 0;
        RE::NiPoint3 upperSide = preAimUpperWorldRot.Transpose() * (RE::NiPoint3(0, 1, 0));
        RE::NiPoint3 uloc = arm.shoulder->world.rotate * (upperSide);
        uloc.x = 0;
        // rounding can put the dot of two unit vectors just past 1 when the roll is near zero, and acos of that is NaN for the whole arm
        const float upperCos = std::clamp(MatrixUtils::vec3Dot(MatrixUtils::vec3Norm(uLocalTwist), MatrixUtils::vec3Norm(uloc)), -1.0f, 1.0f);
        float upperAngle = acosf(upperCos) * (uLocalTwist.z > 0 ? 1.f : -1.f);

        arm.upper->local.rotate = MatrixUtils::getMatrixFromEulerAngles(-upperAngle, 0, 0) * arm.upper->local.rotate;

        Uwr = arm.upper->local.rotate * arm.shoulder->world.rotate;

        arm.forearm1->local.rotate = MatrixUtils::getMatrixFromEulerAngles(-upperAngle, 0, 0) * arm.forearm1->local.rotate;

        // The twist bone under the upper arm rolls back part of the roll, so the shoulder end of the mesh rolls less than the elbow end.
        // It has no scene-graph node, so the roll is applied to its flattened-tree entry once the arm's worlds are final (applyUpperTwist)
        _upperTwistRoll[side] = g_config.armUpperTwistSplit > 0.0f ? g_config.armUpperTwistSplit * upperAngle : 0.0f;

        // The forearm arm bone must be rotated from its forward vector to its elbow-to-hand vector in its local space
        // Calculate Flr:  Fwr * rotTowardHand = Uwr * Flr   ===>   Flr = Uwr' * Fwr * rotTowardHand
        RE::NiMatrix3 Fwr = arm.forearm1->local.rotate * Uwr;
        RE::NiPoint3 elbowHand = handPos - elbowWorld;
        RE::NiPoint3 fLocalDir = Fwr * (MatrixUtils::vec3Norm(elbowHand));

        arm.forearm1->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(fLocalDir, RE::NiPoint3(1, 0, 0)) * arm.forearm1->local.rotate;
        Fwr = arm.forearm1->local.rotate * Uwr;

        RE::NiMatrix3 Fwr3;

        if (!_inPowerArmor && arm.forearm2 != nullptr && arm.forearm3 != nullptr) {
            auto Fwr2 = arm.forearm2->local.rotate * Fwr;
            Fwr3 = arm.forearm3->local.rotate * Fwr2;

            // Find the angle the wrist is pointing and twist forearm3 appropriately
            //    Fwr * twist = Uwr * Flr   ===>   Flr = (Uwr' * Fwr) * twist = (Flr) * twist

            RE::NiPoint3 wLocalDir = Fwr3 * (MatrixUtils::vec3Norm(handInSide));
            wLocalDir.x = 0;
            RE::NiPoint3 forearm3Side = Fwr3.Transpose() * (RE::NiPoint3(0, 0, -1));
            // forearm is rotated 90 degrees already from hand so need this vector instead of 0,-1,0
            RE::NiPoint3 floc = Fwr2 * (MatrixUtils::vec3Norm(forearm3Side));
            floc.x = 0;
            float fcos = MatrixUtils::vec3Dot(MatrixUtils::vec3Norm(wLocalDir), MatrixUtils::vec3Norm(floc));
            float fsin = MatrixUtils::vec3Det(MatrixUtils::vec3Norm(wLocalDir), MatrixUtils::vec3Norm(floc), RE::NiPoint3(-1, 0, 0));
            float forearmAngle = -1 * negLeft * atan2f(fsin, fcos);

            // spread the roll over the three forearm bones (the wrist is set exactly below, so the weights only shape the skin twist)
            if (!common::fEqual(g_config.armForearmTwistWeight1, 0.0f)) {
                arm.forearm1->local.rotate = MatrixUtils::getMatrixFromEulerAngles(negLeft * forearmAngle * g_config.armForearmTwistWeight1, 0, 0) * arm.forearm1->local.rotate;
                Fwr = arm.forearm1->local.rotate * Uwr;
            }
            arm.forearm2->local.rotate = MatrixUtils::getMatrixFromEulerAngles(negLeft * forearmAngle * g_config.armForearmTwistWeight2, 0, 0) * arm.forearm2->local.rotate;
            arm.forearm3->local.rotate = MatrixUtils::getMatrixFromEulerAngles(negLeft * forearmAngle * g_config.armForearmTwistWeight3, 0, 0) * arm.forearm3->local.rotate;

            Fwr2 = arm.forearm2->local.rotate * Fwr;
            Fwr3 = arm.forearm3->local.rotate * Fwr2;
        }

        // Calculate Hlr:  Fwr * Hlr = handRot   ===>   Hlr = Fwr' * handRot
        arm.hand->local.rotate = handRot * (_inPowerArmor ? Fwr : Fwr3).Transpose();

        // Calculate Flp:  Fwp = Uwp + Uwr * (Flp * Uws) = elbowWorld   ===>   Flp = Uwr' * (elbowWorld - Uwp) / Uws
        arm.forearm1->local.translate = Uwr * ((elbowWorld - Uwp) / arm.upper->world.scale);

        float origEHLen = MatrixUtils::vec3Len(arm.hand->world.translate - arm.forearm1->world.translate);
        float forearmRatio = forearmLen / origEHLen * _root->local.scale;

        if (arm.forearm2 && !_inPowerArmor) {
            arm.forearm2->local.translate *= forearmRatio;
            arm.forearm3->local.translate *= forearmRatio;
        }
        arm.hand->local.translate *= forearmRatio;

        return true;
    }

    void Skeleton::dampenHand(RE::NiNode* node, const bool isLeft)
    {
        if (!g_config.dampenHands) {
            if (g_frik.isMainConfigurationModeActive()) {
                // small hack to prevent jarring effect when dampen is enabled for the first time via main config
                if (isLeft) {
                    _leftHandPrevFrame = node->world;
                } else {
                    _rightHandPrevFrame = node->world;
                }
            }
            return;
        }

        const bool isInScopeMenu = g_frik.isLookingThroughScope();

        // the looking-through edge is where the damping pair switches; debug-only, the kScopeEnter / kScopeExit broadcast already marks the edge at info
        if (isInScopeMenu != _dampenHandsInScopePrevFrame) {
            _dampenHandsInScopePrevFrame = isInScopeMenu;
            logger::debug("Damping pair -> {}", isInScopeMenu ? "in-scope" : "general");
        }

        if (isInScopeMenu && (!g_config.dampenHandsInVanillaScope || g_scopeAuthority.hasCapability(ScopeCapability::OwnsDamping))) {
            // keep the anchor fresh while no damping runs, so the general pair resumes from the current pose instead of catching up
            if (isLeft) {
                _leftHandPrevFrame = node->world;
            } else {
                _rightHandPrevFrame = node->world;
            }
            return;
        }

        // A fresh skeleton has no anchor yet: snap to the current pose for one frame instead of easing in from an unrelated one,
        // so the hand targets (and everything an external mod binds to them on kSkeletonReady) are right from the first frame.
        if (bool& seeded = isLeft ? _leftHandDampenSeeded : _rightHandDampenSeeded; !seeded) {
            seeded = true;
            (isLeft ? _leftHandPrevFrame : _rightHandPrevFrame) = node->world;
            return;
        }

        // Get the previous frame transform
        const RE::NiTransform& prevFrame = isLeft ? _leftHandPrevFrame : _rightHandPrevFrame;

        // The damping strengths are per-frame retentions at 90 Hz; the move fraction keeps that feel at any frame rate
        const float rotationMove =
            smoothingMoveFraction(isInScopeMenu ? g_config.dampenHandsRotationInVanillaScope : g_config.dampenHandsRotation, _frameTime, g_config.armFrameRateIndependentSmoothing);
        const float translationMove = smoothingMoveFraction(isInScopeMenu ? g_config.dampenHandsTranslationInVanillaScope : g_config.dampenHandsTranslation,
            _frameTime,
            g_config.armFrameRateIndependentSmoothing);

        // Spherical interpolation between previous frame and current frame for the world rotation matrix
        Quaternion rq, rt;
        rq.fromMatrix(prevFrame.rotate);
        rt.fromMatrix(node->world.rotate);
        rq = slerpToward(rq, rt, rotationMove);
        node->world.rotate = rq.getMatrix();

        // Linear interpolation between the position from the previous frame to current frame
        const RE::NiPoint3 dir = _curentPosition - _lastPosition; // Offset the player movement from this interpolation
        RE::NiPoint3 deltaPos = node->world.translate - prevFrame.translate - dir; // Add in player velocity
        deltaPos *= 1.0f - translationMove;
        node->world.translate -= deltaPos;

        // Update the previous frame transform
        if (isLeft) {
            _leftHandPrevFrame = node->world;
        } else {
            _rightHandPrevFrame = node->world;
        }

        RE::NiNodeUtils::UpdateWorldDataDown(node, false);
    }

    /**
     * Default skeleton nodes position and rotation to be used for resetting skeleton before each frame update manipulations.
     * Required because loading a game does NOT reset the skeleton nodes resulting in incorrect positions/rotations.
     * Entering/Existing power-armor fixes the skeleton but loading the game over and over makes it worse.
     * By forcing the hardcoded default values the issue is prevented as we always start with the same initial values.
     * The values were collected by reading them from the skeleton nodes on first load of a saved game before any manipulations.
     */
    std::unordered_map<std::string, RE::NiTransform> Skeleton::getSkeletonNodesDefaultTransforms()
    {
        return std::unordered_map<std::string, RE::NiTransform>{
            { "Root", MatrixUtils::getTransform(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "COM", MatrixUtils::getTransform(0.0f, 0.0f, 68.91130f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f) },
            { "Pelvis", MatrixUtils::getTransform(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "LLeg_Thigh",
                MatrixUtils::getTransform(0.0f, 0.00040f, 6.61510f, -0.99112f, -0.00017f, -0.13297f, -0.03860f, 0.95730f, 0.28650f, 0.12725f, 0.28909f, -0.94881f, 1.0f) },
            { "LLeg_Calf", MatrixUtils::getTransform(31.59520f, 0.0f, 0.0f, 0.99210f, 0.12266f, -0.02618f, -0.12266f, 0.99245f, 0.00159f, 0.02617f, 0.00164f, 0.99966f, 1.0f) },
            { "LLeg_Foot", MatrixUtils::getTransform(31.94290f, 0.0f, 0.0f, 0.45330f, -0.88555f, -0.10159f, 0.88798f, 0.45855f, -0.03499f, 0.07757f, -0.07435f, 0.99421f, 1.0f) },
            { "RLeg_Thigh",
                MatrixUtils::getTransform(0.0f, 0.00040f, -6.61510f, -0.99307f, 0.00520f, 0.11741f, -0.02903f, 0.95721f, -0.28795f, -0.11389f, -0.28936f, -0.95042f, 1.0f) },
            { "RLeg_Calf", MatrixUtils::getTransform(31.59510f, 0.0f, 0.0f, 0.99108f, 0.13329f, 0.00011f, -0.13329f, 0.99108f, 0.00139f, 0.00007f, -0.00140f, 1.0f, 1.0f) },
            { "RLeg_Foot", MatrixUtils::getTransform(31.94260f, 0.0f, 0.0f, 0.44741f, -0.88731f, 0.11181f, 0.89061f, 0.45344f, 0.03463f, -0.08143f, 0.08409f, 0.99313f, 1.0f) },
            { "SPINE1", MatrixUtils::getTransform(3.792f, -0.00290f, 0.0f, 0.99246f, -0.12254f, 0.0f, 0.12254f, 0.99246f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "SPINE2", MatrixUtils::getTransform(8.70470f, 0.0f, 0.0f, 0.98463f, 0.17464f, 0.0f, -0.17464f, 0.98463f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "Chest", MatrixUtils::getTransform(9.95630f, 0.0f, 0.0f, 0.99983f, -0.01837f, 0.0f, 0.01837f, 0.99983f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "LArm_Collarbone",
                MatrixUtils::getTransform(19.15320f, -0.51040f, 1.69510f, -0.40489f, -0.00599f, -0.91434f, -0.26408f, 0.95813f, 0.11066f, 0.87540f, 0.28627f, -0.38952f, 1.0f) },
            { "LArm_UpperArm",
                MatrixUtils::getTransform(12.53660f, 0.0f, 0.0f, 0.91617f, -0.25279f, -0.31102f, 0.25328f, 0.96658f, -0.03954f, 0.31062f, -0.04255f, 0.94958f, 1.0f) },
            { "LArm_ForeArm1",
                MatrixUtils::getTransform(17.96830f, 0.0f, 0.0f, 0.85511f, -0.51462f, -0.06284f, 0.51548f, 0.85690f, -0.00289f, 0.05534f, -0.02992f, 0.99802f, 1.0f) },
            { "LArm_ForeArm2", MatrixUtils::getTransform(6.15160f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.99999f, -0.00536f, 0.0f, 0.00536f, 0.99999f, 1.0f) },
            { "LArm_ForeArm3", MatrixUtils::getTransform(6.15160f, -0.00010f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.99999f, -0.00536f, 0.0f, 0.00536f, 0.99999f, 1.0f) },
            { "LArm_Hand", MatrixUtils::getTransform(6.15160f, 0.0f, -0.00010f, 0.98845f, 0.14557f, -0.04214f, 0.04136f, 0.00839f, 0.99911f, 0.14579f, -0.98931f, 0.00227f, 1.0f) },
            { "RArm_Collarbone",
                MatrixUtils::getTransform(19.15320f, -0.51040f, -1.69510f, -0.40497f, -0.00602f, 0.91431f, -0.26413f, 0.95811f, -0.11069f, -0.87535f, -0.28632f, -0.38960f, 1.0f) },
            { "RArm_UpperArm", MatrixUtils::getTransform(12.53430f, 0.0f, 0.0f, 0.91620f, -0.25314f, 0.31064f, 0.25365f, 0.96649f, 0.03947f, -0.31022f, 0.04263f, 0.94971f, 1.0f) },
            { "RArm_ForeArm1",
                MatrixUtils::getTransform(17.97050f, 0.00010f, -0.00010f, 0.85532f, -0.51419f, 0.06360f, 0.51507f, 0.85714f, 0.00288f, -0.05599f, 0.03030f, 0.99797f, 1.0f) },
            { "RArm_ForeArm2", MatrixUtils::getTransform(6.15280f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.99999f, 0.00536f, 0.0f, -0.00536f, 0.99999f, 1.0f) },
            { "RArm_ForeArm3", MatrixUtils::getTransform(6.15290f, 0.0f, -0.00010f, 1.0f, 0.0f, 0.0f, 0.0f, 0.99999f, 0.00536f, 0.0f, -0.00536f, 0.99999f, 1.0f) },
            { "RArm_Hand", MatrixUtils::getTransform(6.15290f, 0.0f, 0.0f, 0.98845f, 0.14557f, 0.04214f, 0.04136f, 0.00839f, -0.99911f, -0.14579f, 0.98931f, 0.00227f, 1.0f) },
            { "Neck", MatrixUtils::getTransform(22.084f, -3.767f, 0.0f, 0.91268f, -0.40867f, -0.00003f, 0.40867f, 0.91268f, 0.0f, 0.00002f, -0.00001f, 1.0f, 1.0f) },
            { "Head", MatrixUtils::getTransform(8.22440f, 0.0f, 0.0f, 0.94872f, 0.31613f, 0.00002f, -0.31613f, 0.94872f, -0.00001f, -0.00003f, 0.0f, 1.0f, 1.0f) },
        };
    }

    // See "getSkeletonNodesDefaultTransforms" above
    std::unordered_map<std::string, RE::NiTransform> Skeleton::getSkeletonNodesDefaultTransformsInPA()
    {
        return std::unordered_map<std::string, RE::NiTransform>{
            { "Root", MatrixUtils::getTransform(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "COM", MatrixUtils::getTransform(0.0f, -3.74980f, 89.41950f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f) },
            { "Pelvis", MatrixUtils::getTransform(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "LLeg_Thigh",
                MatrixUtils::getTransform(4.54870f, -1.33f, 6.90830f, -0.98736f, 0.14491f, 0.06416f, 0.06766f, 0.01940f, 0.99752f, 0.14331f, 0.98925f, -0.02896f, 1.0f) },
            { "LLeg_Calf", MatrixUtils::getTransform(34.298f, 0.0f, 0.0f, 0.99681f, -0.00145f, 0.07983f, 0.00170f, 0.99999f, -0.00305f, -0.07982f, 0.00318f, 0.99680f, 1.0f) },
            { "LLeg_Foot", MatrixUtils::getTransform(52.54120f, 0.0f, 0.0f, 0.63109f, -0.76168f, -0.14685f, -0.07775f, 0.12624f, -0.98895f, 0.77180f, 0.63554f, 0.02045f, 1.0f) },
            { "RLeg_Thigh",
                MatrixUtils::getTransform(4.54760f, -1.32430f, -6.898f, -0.98732f, 0.14533f, -0.06381f, 0.06732f, 0.01938f, -0.99754f, -0.14374f, -0.98919f, -0.02892f, 1.0f) },
            { "RLeg_Calf", MatrixUtils::getTransform(34.29790f, 0.0f, 0.0f, 0.99684f, -0.00096f, -0.07937f, 0.00120f, 0.99999f, 0.00307f, 0.07937f, -0.00316f, 0.99684f, 1.0f) },
            { "RLeg_Foot", MatrixUtils::getTransform(52.54080f, 0.0f, 0.0f, 0.63118f, -0.76162f, 0.14677f, -0.07771f, 0.12618f, 0.98896f, -0.77173f, -0.63562f, 0.02046f, 1.0f) },
            { "SPINE1", MatrixUtils::getTransform(5.75050f, -0.00290f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "SPINE2", MatrixUtils::getTransform(5.62550f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "Chest", MatrixUtils::getTransform(5.53660f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "LArm_Collarbone",
                MatrixUtils::getTransform(22.192f, 0.34820f, 1.00420f, -0.34818f, -0.05435f, -0.93585f, -0.26919f, 0.96207f, 0.04428f, 0.89794f, 0.26734f, -0.34961f, 1.0f) },
            { "LArm_UpperArm",
                MatrixUtils::getTransform(14.59840f, 0.00010f, 0.00010f, 0.77214f, -0.19393f, -0.60514f, 0.08574f, 0.97538f, -0.20318f, 0.62964f, 0.10499f, 0.76976f, 1.0f) },
            { "LArm_ForeArm1",
                MatrixUtils::getTransform(19.53690f, 0.41980f, 0.04580f, 0.92233f, -0.38166f, -0.06030f, 0.38176f, 0.92420f, -0.01042f, 0.05971f, -0.01341f, 0.99813f, 1.0f) },
            { "LArm_ForeArm2", MatrixUtils::getTransform(0.00020f, 0.00020f, 0.00020f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "LArm_ForeArm3", MatrixUtils::getTransform(10.000494f, 0.000162f, -0.000004f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "LArm_Hand",
                MatrixUtils::getTransform(26.96440f, 0.00020f, 0.00040f, 0.98604f, 0.16503f, 0.02218f, 0.00691f, -0.17364f, 0.98479f, 0.16638f, -0.97088f, -0.17236f, 1.0f) },
            { "RArm_Collarbone",
                MatrixUtils::getTransform(22.19190f, 0.34810f, -1.004f, -0.34818f, -0.06482f, 0.93518f, -0.26918f, 0.96251f, -0.03351f, -0.89795f, -0.26340f, -0.35257f, 1.0f) },
            { "RArm_UpperArm",
                MatrixUtils::getTransform(14.59880f, 0.0f, 0.0f, 0.77213f, -0.19339f, 0.60533f, 0.09277f, 0.97667f, 0.19369f, -0.62866f, -0.09340f, 0.77205f, 1.0f) },
            { "RArm_ForeArm1",
                MatrixUtils::getTransform(19.53660f, 0.41990f, -0.04620f, 0.92233f, -0.38166f, 0.06029f, 0.38171f, 0.92422f, 0.01129f, -0.06003f, 0.01260f, 0.99812f, 1.0f) },
            { "RArm_ForeArm2", MatrixUtils::getTransform(-0.00010f, -0.00010f, -0.00010f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "RArm_ForeArm3", MatrixUtils::getTransform(10.00050f, -0.00010f, 0.00010f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f) },
            { "RArm_Hand",
                MatrixUtils::getTransform(26.96460f, 0.00010f, 0.00120f, 0.98604f, 0.16503f, -0.02218f, 0.00691f, -0.17364f, -0.98479f, -0.16638f, 0.97088f, -0.17236f, 1.0f) },
            { "Neck", MatrixUtils::getTransform(24.29350f, -2.84160f, 0.0f, 0.92612f, -0.37723f, -0.00002f, 0.37723f, 0.92612f, 0.00001f, 0.00002f, -0.00002f, 1.0f, 1.0f) },
            { "Head", MatrixUtils::getTransform(8.22440f, 0.0f, 0.0f, 0.94891f, 0.31555f, 0.00002f, -0.31555f, 0.94891f, 0.0f, -0.00002f, -0.00001f, 1.0f, 1.0f) },
        };
    }
}
