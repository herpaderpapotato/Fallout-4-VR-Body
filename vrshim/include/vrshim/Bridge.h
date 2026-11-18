#pragma once
// FRIK-flat <-> fo4flatvr bridge (FrikBridgeApi.h in plugin/src/plugin). Game thread unless noted.
#include <cstdint>

struct FO4FlatVR_Frame;
struct FO4FlatVR_FrikBridge;
struct FO4FlatVR_Selfie;

namespace vrshim::bridge
{
    // F4SEPlugin_Load, before anything else: finds fo4flatvr.dll's FO4FLATVR_GetFrikBridge. False = stay dormant.
    bool connect() noexcept;
    [[nodiscard]] const FO4FlatVR_FrikBridge* api() noexcept;

    // Bridge v4 (16 Nov): fo4flatvr's VRIK-style selfie request (both hands up; Gestures.h). False = none (older fo4flatvr).
    bool selfie(FO4FlatVR_Selfie& a_out) noexcept;

    // After the framework's load succeeded: logs the address report line and tells fo4flatvr FRIK-flat is active.
    void onLoaded();

    // Per game frame, from the frame callback before FRIK's update: fetch the snapshot, pose the VR player nodes.
    void frameUpdate();

    // Bridge v3 (C17): fo4flatvr.ini [FRIK] bBody (0 = smooth movement only) and the camera offset callback (FRIK smooth
    // movement, called from fo4flatvr's FirstPersonState::Update hook).
    [[nodiscard]] bool bodyEnabled() noexcept;
    bool registerCameraOffset(std::uint8_t (*a_cb)(void*, float*), void* a_ctx) noexcept;

    // Bridge v2: fo4flatvr calls a_cb once per game frame from its own hook at C351F3. False = no hook (stay frameless).
    bool registerFrame(void (*a_cb)(void*), void* a_ctx) noexcept;

    // C18 (body): PlayerWorldNode local z, the FO4VR camera lift (FRIK's HMD offset + smooth-movement z). FO4VR hangs the HMD
    // camera under PlayerWorldNode; on flat fo4flatvr builds the camera, so the node is posed here with this z (room = feet + z)
    // and FRIK's body logic (body z += PlayerWorldNode z, HMD local z = eye height above the room) sees FO4VR's relation.
    // setSmoothedPlayerWorldZ: from FRIK's flat smooth movement (this frame, before frameUpdate); setDefaultPlayerWorldZ: the
    // HMD offset alone, used when smooth movement did not run since the last frameUpdate.
    void setSmoothedPlayerWorldZ(float a_z) noexcept;
    void setDefaultPlayerWorldZ(float a_z) noexcept;

    // The snapshot fetched by the last frameUpdate (valid() false before the first XR frame).
    [[nodiscard]] const FO4FlatVR_Frame& frame() noexcept;
    [[nodiscard]] bool valid() noexcept;
    [[nodiscard]] bool leftHanded() noexcept;
    [[nodiscard]] std::uint64_t frameCount() noexcept;

    // 16 Nov: the frame being run is a pause-time frame (UI::menuMode != 0: no player update, no animation, no floating-gun
    // placement this frame; fo4flatvr calls the frame callback from Main::OnIdle C33E62 then, [FRIK] bPausedFrame). Set by frameUpdate.
    [[nodiscard]] bool pausedFrame() noexcept;

    // fo4flatvr.ini [FRIK] integer key (a_def when the key or the ini is missing). Any thread.
    [[nodiscard]] int iniInt(const char* a_key, int a_def) noexcept;

    // C32 diag: the raw controller grip of the last frameUpdate in world (camera . grip-in-head, no wand calibration), side 0 left
    // 1 right; rotation in NiTransform storage (a_rot[i] = local axis i in world). False when that side had no valid grip.
    bool gripWorld(int a_side, float a_rot[3][3], float a_pos[3]) noexcept;
}
