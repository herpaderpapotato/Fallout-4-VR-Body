#include "SelfieHandler.h"

#include "FRIK.h"

#ifdef FRIK_FLAT
#include "fo4flatvr/FrikBridgeApi.h"
#include "vrshim/Bridge.h"

#include <cmath>
#endif

using namespace common;

namespace frik
{
#ifdef FRIK_FLAT
    namespace
    {
        // FRIK-flat selfie (16 Nov): VRIK's selfie mode on FRIK's mirror body. fo4flatvr's gestures (both hands up, Gestures.h)
        // send the request through bridge v4 (GetSelfie); FRIK's own selfie toggle (config mode A, Papyrus, the test flag)
        // uses the same placement without steering. Placement (VRIK VrikBody::update3rdPerson): the body stands distance in
        // front of the camera, facing it; while the request follows (hands up) it is re-placed every frame and turned by the
        // wrist roll (yaw); otherwise it stays where it was put, in the world.
        struct FlatSelfie
        {
            bool reqActive = false; // last bridge request
            unsigned reqSeq = 0;
            bool anchorValid = false;
            float anchor[2]{}; // world x, y (game units)
            float facing[2]{ 0, 1 }; // body forward at placement (toward the camera), world xy unit
            float yaw = 0;
        };

        FlatSelfie g_flatSelfie;

        // true = the flat placement ran (skip FRIK's FO4VR basicSelfie)
        bool flatSelfie()
        {
            auto& st = g_flatSelfie;
            FO4FlatVR_Selfie req{};
            const bool have = vrshim::bridge::selfie(req);
            bool follow = false;
            float distM = g_config.selfieOutFrontDistance / 70.0f;
            if (have) {
                const bool on = req.active != 0;
                if (on != st.reqActive || req.seq != st.reqSeq) {
                    if (on != st.reqActive) {
                        logger::info("FRIK-flat: selfie {} (fo4flatvr gesture request #{})", on ? "on" : "off", req.seq);
                        g_frik.setSelfieMode(on);
                    }
                    st.reqActive = on;
                    st.reqSeq = req.seq;
                    st.anchorValid = false;
                }
                if (on) {
                    follow = req.follow != 0;
                    distM = req.distanceM;
                    st.yaw = req.yawRad;
                }
            }
            if (!g_frik.isSelfieModeOn()) {
                st.anchorValid = false;
                return true; // nothing to place (FRIK's basicSelfie returns too)
            }
            if (!(have && st.reqActive)) {
                st.yaw = 0; // FRIK's own toggle: no steering
            }
            const auto root = f4vr::getRootNode();
            const auto cam = f4vr::getPlayerCamera();
            if (!root || !root->parent || !cam || !cam->cameraRoot) {
                return true;
            }
            const auto& cw = cam->cameraRoot->world;
            if (follow || !st.anchorValid) {
                float fx = cw.rotate.entry[1][0], fy = cw.rotate.entry[1][1]; // camera forward (row 1), horizontal
                const float l = std::sqrt(fx * fx + fy * fy);
                if (l < 1e-3f) {
                    return true;
                }
                fx /= l, fy /= l;
                float upm = 70.0f;
                if (vrshim::bridge::valid()) {
                    upm = vrshim::bridge::frame().unitsPerMeter > 1.0f ? vrshim::bridge::frame().unitsPerMeter : 70.0f;
                }
                const float d = distM * upm;
                st.anchor[0] = cw.translate.x + fx * d;
                st.anchor[1] = cw.translate.y + fy * d;
                st.facing[0] = -fx, st.facing[1] = -fy;
                st.anchorValid = true;
            }
            // facing turned by yaw about +Z (counter-clockwise seen from above)
            const float c = std::cos(st.yaw), s = std::sin(st.yaw);
            const float f0 = st.facing[0] * c - st.facing[1] * s, f1 = st.facing[0] * s + st.facing[1] * c;
            // world rotation, rows = the root's local axes in world (X right, Y forward, Z up)
            RE::NiMatrix3 w{};
            w.entry[0][0] = f1, w.entry[0][1] = -f0, w.entry[0][2] = 0;
            w.entry[1][0] = f0, w.entry[1][1] = f1, w.entry[1][2] = 0;
            w.entry[2][0] = 0, w.entry[2][1] = 0, w.entry[2][2] = 1;
            const auto body = root->parent;
            const float z = root->local.translate.z; // the skeleton's own height under its parent (as basicSelfie)
            root->local.rotate = w * body->world.rotate.Transpose();
            // world -> parent local: local = parent.rotate * (world - parent.translate) / scale (FRIK's convention)
            const RE::NiPoint3 delta(st.anchor[0] - body->world.translate.x, st.anchor[1] - body->world.translate.y, 0.0f);
            const float sc = body->world.scale > 1e-4f ? body->world.scale : 1.0f;
            const RE::NiPoint3 loc = body->world.rotate * delta;
            root->local.translate = RE::NiPoint3(loc.x / sc, loc.y / sc, z);
            f4vr::updateTransformsDown(root, true);
            return true;
        }
    }
#endif

    void SelfieHandler::onFrameUpdate() const
    {
#ifdef FRIK_FLAT
        if (flatSelfie()) {
            return;
        }
#endif
        basicSelfie();
    }

    /**
     * Projects the 3rd person body out in front of the player by offset amount
     */
    void SelfieHandler::basicSelfie() const
    {
        if (!g_frik.isSelfieModeOn()) {
            return;
        }

        const auto root = f4vr::getRootNode();
        if (!root) {
            return;
        }

        const float z = root->local.translate.z;
        const auto body = root->parent;

        const auto back = MatrixUtils::vec3Norm(RE::NiPoint3(-_forwardDir.x, -_forwardDir.y, 0));
        const auto bodyDir = RE::NiPoint3(0, 1, 0);

        root->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(back, bodyDir) * body->world.rotate.Transpose();
        root->local.translate = body->world.translate - f4vr::getCameraPosition();
        root->local.translate.y += g_config.selfieOutFrontDistance;
        root->local.translate.z = z;

        f4vr::updateTransformsDown(root, true);
    }

    void SelfieHandler::testSelfie()
    {
        if (!g_frik.isSelfieModeOn()) {
            if (_selfieActive) {
                _selfieActive = false;
                exitSelfieMode();
            }
            return;
        }

        if (!_selfieActive) {
            _selfieActive = true;
            enterSelfieMode();
        }

        const auto root = f4vr::getRootNode();

        const auto back = MatrixUtils::vec3Norm(RE::NiPoint3(-_forwardDir.x, -_forwardDir.y, 0));
        root->local.rotate = MatrixUtils::getMatrixFromRotateVectorVec(back, RE::NiPoint3(0, 1, 0)) * root->parent->world.rotate.Transpose();

        // rotate the skeleton
        // root->local.rotate = root->local.rotate * getMatrixFromDebugFlowFlags();

        const auto diff = root->parent->world.translate - _rootWorldPos;
        logger::sample("Diff: {:.2f}, {:.2f}, {:.2f}", diff.x, diff.y, diff.z);
        root->parent->local.translate -= diff;

        // f4vr::updateDown(pWorldNode, true);
        f4vr::updateDown(f4vr::getRootNode()->parent, true);
        // f4vr::updateDownFromRoot();
    }

    void SelfieHandler::enterSelfieMode()
    {
        const auto hmdRot = f4vr::getVRPlayerNodes()->hmdNode->local.rotate;
        _forwardDir = RE::NiPoint3(hmdRot.entry[1][0], hmdRot.entry[1][1], 0);

        _rootWorldPos = f4vr::getRootNode()->parent->world.translate;

        const auto player = RE::PlayerCharacter::GetSingleton();
        _playerStartPosition = player->GetPosition();

        const auto back = MatrixUtils::vec3Norm(RE::NiPoint3(-_forwardDir.x, -_forwardDir.y, 0));
        player->SetPosition(_playerStartPosition + back * g_config.selfieOutFrontDistance, true);
    }

    void SelfieHandler::exitSelfieMode() const
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        player->SetPosition(_playerStartPosition, true);
    }

    void SelfieHandler::experimental()
    {
        // const auto pWorldNode = f4vr::getVRPlayerNodes()->playerWorldNode;
        //
        // const RE::NiNode* body = _root->parent;
        // const auto back = vec3Norm(RE::NiPoint3(-_forwardDir.x, -_forwardDir.y, 0));
        //
        // _root->local.rotate = getMatrixFromRotateVectorVec(back, RE::NiPoint3(0, 1, 0)) * body->world.rotate.Transpose();
        //
        // // pWorldNode->local.translate += back * g_config.selfieOutFrontDistance;
        //
        // pWorldNode->local.translate += RE::NiPoint3(0, 1, 0) * g_config.selfieOutFrontDistance;
        //
        // // getPlayerCamera()->cameraNode->local.translate += RE::NiPoint3(0, 1, 0) * g_config.selfieOutFrontDistance;
        // // updateDown(getPlayerCamera()->cameraNode, true);
        //
        // const auto playerCamera = RE::PlayerCamera::GetSingleton();
        // const auto cameraNode = playerCamera->cameraRoot.get();
        // cameraNode->local.translate += getPointFromDebugFlowFlags();
        // cameraNode->world.translate += getPointFromDebugFlowFlags();
        //
        // auto& playerLocalTransformPos = pWorldNode->local.translate;
        //
        // // updateUpTo(cameraNode->parent->parent->parent, cameraNode, true);
        //
        // // RE::NiUpdateData* ud = nullptr;
        // // cameraNode->Update(*ud);
        //
        // const auto player = RE::PlayerCharacter::GetSingleton();
        // player->SetPosition(player->GetPosition() + getPointFromDebugFlowFlags(), true);
        //
        // logger::sample("Camera: {} ({:.2f},{:.2f}) ({:.2f},{:.2f})", playerCamera->enabled, playerCamera->rotationInput.x, playerCamera->rotationInput.x,
        //     playerCamera->translationInput.x, playerCamera->translationInput.x);
        //
        // if (g_config.checkDebugDumpDataOnceFor("selfie_update")) {
        //     playerCamera->Update();
        // }
        //
        // if (g_config.checkDebugDumpDataOnceFor("selfie_free")) {
        //     playerCamera->ToggleFreeCameraMode(true);
        // }
    }
}
