// FRIK-flat: fo4flatvr bridge client. Poses the VR player node table (vrshim/VrNodes.cpp) from fo4flatvr's OpenXR snapshot
// each game frame, and keeps the snapshot for the OpenVR IVRSystem shim (vrshim/src/OpenVRStub.cpp).
//
// Frames (analysis/frik-flat-port.md section 2.1):
//  HMDNode world        = PlayerCamera::cameraRoot world at the frame hook (C351F3), the camera fo4flatvr drives from the head
//  RoomNode world       = heading-only: yaw(camera) - yaw(head in the reference space); origin at the player's feet (z) so
//                         HMDNode/UprightHmdNode local z = eye height above the feet (FRIK's body pitch and height use it)
//  PrimaryWandNode etc. = camera . grip-in-head (metres x unitsPerMeter) . C, C = fo4flatvr's [Hands] grip calibration
//                         (fGripRotX/Y/ZDeg, fGripOffsetX/Y/Z; the same grip -> FO4VR-wand frame fo4flatvr's hands code uses)
//  PlayerWorldNode      = parent of RoomNode, identity (RoomNode carries the world placement)
// Same column-vector math and NiTransform storage convention as fo4flatvr Hands.cpp (FromNi / ToNi), verified in game there.
#include "vrshim/Bridge.h"
#include "vrshim/VrNodes.h"
#include "vrshim/XfMath.h"

#include "fo4flatvr/FrikBridgeApi.h"

#include <Windows.h>

#include <atomic>
#include <cmath>

namespace vrshim::bridge
{
    using namespace vrshim::xf;

    namespace
    {
        const FO4FlatVR_FrikBridge* g_api = nullptr;
        FO4FlatVR_Frame             g_frame{};
        bool                        g_valid = false;
        std::uint64_t               g_frames = 0;
        std::uint64_t               g_posed = 0;
        std::atomic<float>          g_pwnZSmoothed{ 0.0f };
        std::atomic<std::uint64_t>  g_pwnZSmoothedFrame{ ~0ull };  // g_frames value when set (smoothing runs before frameUpdate)
        std::atomic<float>          g_pwnZDefault{ 0.0f };
        float                       g_pwnZ = 0.0f;

        // OpenXR quaternion (reference space) -> rotation in the game basis (X right, Y forward, Z up): B R B^T, B = (x, -z, y)
        M3 GameRotFromXr(const float q[4])
        {
            const double x = q[0], y = q[1], z = q[2], w = q[3];
            M3 r;  // OpenXR column-vector rotation
            r.m[0][0] = 1 - 2 * (y * y + z * z), r.m[0][1] = 2 * (x * y - z * w), r.m[0][2] = 2 * (x * z + y * w);
            r.m[1][0] = 2 * (x * y + z * w), r.m[1][1] = 1 - 2 * (x * x + z * z), r.m[1][2] = 2 * (y * z - x * w);
            r.m[2][0] = 2 * (x * z - y * w), r.m[2][1] = 2 * (y * z + x * w), r.m[2][2] = 1 - 2 * (x * x + y * y);
            M3 B;
            B.m[0][0] = 1, B.m[0][1] = 0, B.m[0][2] = 0;
            B.m[1][0] = 0, B.m[1][1] = 0, B.m[1][2] = -1;
            B.m[2][0] = 0, B.m[2][1] = 1, B.m[2][2] = 0;
            return Mul(Mul(B, r), T(B));
        }
        V    GameVecFromXr(const float p[3]) { return { p[0], -p[2], p[1] }; }
        // heading of the forward (Y) axis about Z, radians, 0 = +Y
        double Yaw(const M3& R) { return std::atan2(-R.m[0][1], R.m[1][1]); }

        Xf HandXf(const FO4FlatVR_HeadRel& h, double upm)
        {
            Xf x;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) x.R.m[r][c] = h.rot[r][c];
            x.R = Ortho(x.R);
            x.t = V{ h.pos[0], h.pos[1], h.pos[2] } * upm;
            return x;
        }

        struct Calib
        {
            bool loaded = false;
            Xf   right, left;
            Xf   leftMirror;        // C34: left wand for right-handed mode (FRIK's off-hand chain -> mirror of the right hand)
            bool leftMirrorOn = false;
        };
        Calib g_calib;
        Xf    g_gripWorld[2];          // C32 diag
        bool  g_gripWorldValid[2]{};

        float IniFloat(const char* a_section, const char* a_key, float a_def)
        {
            const char* path = g_api && g_api->IniPath ? g_api->IniPath() : nullptr;
            if (!path) return a_def;
            char buf[64]{};
            ::GetPrivateProfileStringA(a_section, a_key, "", buf, sizeof(buf), path);
            return buf[0] ? std::strtof(buf, nullptr) : a_def;
        }

        void LoadCalib(double a_upm)
        {
            if (g_calib.loaded) return;
            constexpr double d2r = 3.14159265358979 / 180.0;
            // fo4flatvr's floating-gun calibration: [Weapon], else the retired [Hands] section, else the default
            auto cal = [](const char* k, float d) { return IniFloat("Weapon", k, IniFloat("Hands", k, d)); };
            const double rx = cal("fGripRotXDeg", -60.0f), ry = cal("fGripRotYDeg", 0.0f), rz = cal("fGripRotZDeg", 0.0f);
            const V off{ cal("fGripOffsetX", 0.0f), cal("fGripOffsetY", 0.0f), cal("fGripOffsetZ", 0.0f) };
            Xf c;
            c.R = Mul(Rz(rz * d2r), Mul(Ry(ry * d2r), Rx(rx * d2r)));
            c.t = off * (0.01 * a_upm);
            g_calib.right = c;
            // left hand: mirror through the grip's YZ plane (M C M, M = diag(-1,1,1))
            M3 M;
            M.m[0][0] = -1;
            g_calib.left.R = Mul(Mul(M, c.R), M);
            g_calib.left.t = Mul(M, c.t);
            // C34 (user 15 Nov, after C33: "the left hand's orientation seems to be around 45 degrees wrong and different to the
            // right"): FRIK's off-hand chain (Skeleton.cpp updateHandTarget: SecondaryMeleeWeaponOffsetNode = primary offset
            // local x Euler(0,180,0), t(-2,-9,2); placeFirstPersonArm: off-hand glue = primary glue x Euler(0,45,0)) applied to
            // the mirrored wand (M C M) put the left hand 57.2 deg about the wand's X axis off the mirror image of the right hand
            // (bone convention from the skeleton's own L/R defaults: left = mirror with Z negated, Skeleton.cpp 1741-1757).
            // Left wand calibration chosen so that chain yields exactly the mirror of the right hand:
            //   C_L = M . C . G^T . Sz . E45 . G . E180^T   (column form; G = FRIK's glue, E = FRIK's Euler(0,a,0) = Ry(-a))
            {
                const double gr[3][3] = { { -0.122, 0.990, 0.069 }, { 0.987, 0.114, 0.109 }, { 0.100, 0.081, -0.992 } };  // glue storage rows
                M3 G;
                for (int r = 0; r < 3; ++r)
                    for (int col = 0; col < 3; ++col) G.m[r][col] = gr[col][r];
                G = Ortho(G);
                M3 Sz;
                Sz.m[2][2] = -1;
                const M3 E45 = Ry(-45.0 * d2r), E180 = Ry(-180.0 * d2r);
                g_calib.leftMirror.R = Mul(Mul(Mul(Mul(Mul(Mul(M, c.R), T(G)), Sz), E45), G), T(E180));
                g_calib.leftMirror.t = g_calib.left.t;
                g_calib.leftMirrorOn = IniFloat("FRIK", "bLeftHandMirror", 1.0f) != 0.0f;
                logger::info("FRIK-flat C34: left wand mirrors the right hand through FRIK's off-hand chain: {} ([FRIK] bLeftHandMirror; right-handed mode)",
                    g_calib.leftMirrorOn ? "on" : "OFF");
            }
            g_calib.loaded = true;
            logger::info("FRIK-flat: wand calibration from fo4flatvr.ini [Weapon]/[Hands]: rot ({:.1f},{:.1f},{:.1f}) deg, offset ({:.1f},{:.1f},{:.1f}) cm, {:.1f} units/m",
                rx, ry, rz, off.x, off.y, off.z, a_upm);
        }

        void Set(RE::NiNode* a_node, const Xf& a_parentWorld, const Xf& a_world)
        {
            if (!a_node) return;
            a_node->local = ToNi(Mul(Inv(a_parentWorld), a_world));
            a_node->world = ToNi(a_world);
        }

        // world of a child whose local is kept (the fixed offset nodes under the wands)
        void Propagate(RE::NiAVObject* a_obj, const RE::NiTransform& a_parentWorld)
        {
            if (!a_obj) return;
            a_obj->world = ToNi(Mul(FromNi(a_parentWorld), FromNi(a_obj->local)));
            if (auto* node = a_obj->IsNode()) {
                for (auto& child : node->children) {
                    if (child) Propagate(child.get(), a_obj->world);
                }
            }
        }
    }

    bool connect() noexcept
    {
        const auto mod = ::GetModuleHandleW(L"fo4flatvr.dll");
        const auto get = mod ? reinterpret_cast<FO4FLATVR_GetFrikBridge_t>(::GetProcAddress(mod, "FO4FLATVR_GetFrikBridge")) : nullptr;
        g_api = get ? get(FO4FLATVR_FRIK_BRIDGE_VERSION) : nullptr;
        return g_api != nullptr;
    }

    const FO4FlatVR_FrikBridge* api() noexcept { return g_api; }

    bool registerFrame(void (*a_cb)(void*), void* a_ctx) noexcept
    {
        return g_api && g_api->SetFrameCallback && g_api->SetFrameCallback(a_cb, a_ctx) != 0;
    }
    bool bodyEnabled() noexcept
    {
        return g_api && g_api->BodyEnabled && g_api->BodyEnabled() != 0;
    }

    bool registerCameraOffset(std::uint8_t (*a_cb)(void*, float*), void* a_ctx) noexcept
    {
        return g_api && g_api->SetCameraOffsetCallback && g_api->SetCameraOffsetCallback(a_cb, a_ctx) != 0;
    }

    void setSmoothedPlayerWorldZ(float a_z) noexcept
    {
        g_pwnZSmoothed.store(a_z, std::memory_order_relaxed);
        g_pwnZSmoothedFrame.store(g_frames, std::memory_order_release);
    }
    void setDefaultPlayerWorldZ(float a_z) noexcept { g_pwnZDefault.store(a_z, std::memory_order_relaxed); }

    bool gripWorld(const int a_side, float a_rot[3][3], float a_pos[3]) noexcept
    {
        if (a_side < 0 || a_side > 1 || !g_gripWorldValid[a_side]) return false;
        const RE::NiTransform n = ToNi(g_gripWorld[a_side]);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) a_rot[i][j] = n.rotate.entry[i][j];
        a_pos[0] = n.translate.x, a_pos[1] = n.translate.y, a_pos[2] = n.translate.z;
        return true;
    }

    const FO4FlatVR_Frame&      frame() noexcept { return g_frame; }
    bool                        valid() noexcept { return g_valid; }
    bool                        leftHanded() noexcept { return g_valid && g_frame.leftHanded; }
    std::uint64_t               frameCount() noexcept { return g_frames; }

    void onLoaded()
    {
        installCrashLog();
        const auto s = addressStats();
        const bool body = bodyEnabled();
        logger::info("FRIK-flat initialised, {}/{} addresses resolved ({} low-confidence); bridge v{}; mode: {}", s.resolved, s.total, s.low,
            g_api ? g_api->version : 0, body ? "full (bBody=1)" : "smooth movement only (bBody=0)");
        logAddressReport();
        if (body && g_api && g_api->SetFrikActive) {  // C17: only with the body does fo4flatvr's floating gun step aside
            g_api->SetFrikActive(1);
        }
    }

    void frameUpdate()
    {
        // C18: smoothing ran this frame (FirstPersonState::Update, before this hook) -> its z, else the HMD offset alone
        g_pwnZ = g_pwnZSmoothedFrame.load(std::memory_order_acquire) == g_frames ? g_pwnZSmoothed.load(std::memory_order_relaxed)
                                                                                 : g_pwnZDefault.load(std::memory_order_relaxed);
        ++g_frames;
        if (!g_api) return;
        FO4FlatVR_Frame f{};
        f.size = sizeof(f);
        g_valid = g_api->GetFrame(&f) != 0 && f.headValid;
        if (g_valid) {
            g_frame = f;
        } else {
            // no XR frame (no runtime / headset off / no-XR test run): head = camera, hands at a rest pose in front
            f = {};
            f.headValid = 1;
            f.head.q[3] = 1.0f;
            f.unitsPerMeter = g_frame.unitsPerMeter;
            for (int side = 0; side < 2; ++side) {
                auto& g = f.hands[side].gripInHead;
                g.valid = 1;
                g.pos[0] = side == 0 ? -0.2f : 0.2f, g.pos[1] = 0.35f, g.pos[2] = -0.3f;
                g.rot[0][0] = g.rot[1][1] = g.rot[2][2] = 1.0f;
            }
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* camRoot = camera ? camera->cameraRoot.get() : nullptr;
        if (!player || !camRoot) return;
        if (!vrNodesCreated()) createVrNodes();
        selectFpHands();  // C19: real first-person hand/weapon nodes, or stand-ins without a drawn weapon
        // C20: the third-person body is drawn in first person. FRIK's DoShow1stPerson patch (D78CFE) only acts when the view is
        // switched; loading a save already in first person returns early (1st/3rd cull flags already differ) and leaves Get3D(0)
        // app-culled (measured 15 Nov 14:05: cull3p=1). First person = the first-person root shown.
        if (auto* third = player->Get3D(false), *first = player->Get3D(true); third && first && third->GetAppCulled() && !first->GetAppCulled()) {
            third->SetAppCulled(false);
            static std::uint32_t s_unculls = 0;
            if (++s_unculls <= 3 || (s_unculls % 1000) == 0) {
                logger::info("FRIK-flat: third-person root was app-culled in first person - shown (#{})", s_unculls);
            }
        }
        auto* n = getVRPlayerNodes();
        const double upm = f.unitsPerMeter > 1.0f ? f.unitsPerMeter : 70.0;
        LoadCalib(upm);

        Xf cam = FromNi(camRoot->world);
        cam.s = 1.0;
        const M3 headR = GameRotFromXr(f.head.q);
        const V  headP = GameVecFromXr(f.head.p) * upm;

        // room: heading only, so the head's room-relative pose is its reference-space pose
        // C26 (user headset 15 Nov 15:02: a 180 stick turn left the body facing the old way): FRIK takes the body heading from
        // HMDNode's LOCAL rotation (Skeleton::setBodyUnderHMD -> root world = rotation of that forward, independent of the
        // actor). A room yawed by (camera - head-in-reference-space) hid every stick/snap turn from it. The room is
        // world-aligned now (R = identity), so HMD local = camera world and the body follows every turn source (stick, snap,
        // physical head turn, game SetHeading) through the camera. HMD local x/y = 0 (the room sits under the head).
        const double roomYaw = 0.0;
        Xf room;
        room.R = Rz(roomYaw);
        const V headXY{ headP.x, headP.y, 0.0 };
        room.t = cam.t;
        (void)headXY;
        // C18: FO4VR's room floor sits PlayerWorldNode z (HMD offset + smoothing) above the feet; HMD local z = eye height
        // above that (FRIK's body pitch reads it), and FRIK adds PlayerWorldNode z to the body height
        room.t.z = player->data.location.z + g_pwnZ;

        const Xf identity{};
        Xf       pwn{};
        pwn.t.z = g_pwnZ;
        Set(n->playerWorldNode, identity, pwn);  // detached root: local = world = lift
        Set(n->roomNode, pwn, room);
        Set(n->hmdNode, room, cam);
        Set(n->offscreenHmdNode, room, cam);
        Xf upright = cam;
        upright.R = Rz(Yaw(cam.R));
        Set(n->uprightHmdNode, room, upright);
        Set(n->uprightHmdLagNode, room, upright);
        Propagate(n->headLightParentNode, n->hmdNode->world);

        const bool lh = f.leftHanded != 0;
        for (int side = 0; side < 2; ++side) {  // 0 left, 1 right
            const auto& h = f.hands[side];
            g_gripWorldValid[side] = h.gripInHead.valid != 0;
            if (!h.gripInHead.valid) continue;
            g_gripWorld[side] = Mul(cam, HandXf(h.gripInHead, upm));
            const bool primary = (side == 1) != lh;
            auto*      wand = primary ? n->primaryWandNode : n->secondaryWandNode;
            const Xf&  cal = side == 1 ? g_calib.right : (!lh && g_calib.leftMirrorOn ? g_calib.leftMirror : g_calib.left);  // C34
            const Xf   world = Mul(Mul(cam, HandXf(h.gripInHead, upm)), cal);
            Set(wand, room, world);
            if (wand && wand->children.size()) {
                for (auto& child : wand->children) {
                    if (child) Propagate(child.get(), wand->world);
                }
            }
        }

        // FRIK's left-hand weapon node: the first-person skeleton's own WeaponLeft (C19: or its stand-in)
        n->weaponLeftNode = fpHands().weaponLeft;
        ++g_posed;
        if (g_posed == 1 || (g_posed % 5000) == 0) {
            const auto& hp = n->hmdNode->local.translate;
            const auto& pw = n->primaryWandNode->world.translate;
            logger::info("FRIK-flat nodes #{}: hmd in room ({:.1f},{:.1f},{:.1f}) eye height {:.1f}u, room yaw {:.1f} deg, primary wand world ({:.1f},{:.1f},{:.1f}), xr seq {}",
                g_posed, hp.x, hp.y, hp.z, hp.z, roomYaw * 57.29578, pw.x, pw.y, pw.z, f.seq);
        }
    }
}
