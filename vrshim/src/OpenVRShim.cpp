// FRIK-flat: the openvr_api entry points (FRIK.dll imports no openvr_api.dll; built with VR_API_EXPORT) and an IVRSystem
// backed by fo4flatvr's OpenXR snapshot (vrshim::bridge). This is the framework's input seam: vrcf::VRControllersManager,
// VRControllersHaptic and VRControllersSuppressor all poll vr::VRSystem(), so they run unchanged on Touch controllers
// mapped the way SteamVR's legacy input reports them. Every other OpenVR interface (compositor, overlay, chaperone...) is
// null, which the framework already treats as "no runtime" (overlays, submit hook, keyboard, frame timing stay off).
//
// Device indices: 0 = HMD, 1 = left hand, 2 = right hand. Tracking space = fo4flatvr's OpenXR reference space (metres,
// +Y up, -Z forward: the OpenVR convention), poses = OpenXR grip poses.
// Buttons (SteamVR legacy Oculus Touch): A/X -> k_EButton_A, B/Y -> k_EButton_ApplicationMenu, grip >= 0.7 -> k_EButton_Grip,
// trigger >= 0.9 -> k_EButton_SteamVR_Trigger (Axis1), stick click -> k_EButton_SteamVR_Touchpad (Axis0). Axes: [0] stick,
// [1].x trigger, [2].x grip. Touch bits: pressed buttons, trigger > 0.05, stick deflection > 0.1 (OpenXR gives no touch
// states through fo4flatvr yet).
#include <cmath>
#include <cstdint>
#include <cstring>

#include <openvr.h>

#include "vrshim/Bridge.h"

#include "fo4flatvr/FrikBridgeApi.h"

namespace
{
    using namespace vr; // the generated IVRSystemDefaults.inl uses openvr.h's unqualified names

    constexpr vr::TrackedDeviceIndex_t kHmd = 0, kLeft = 1, kRight = 2;

    std::uint64_t Bit(const vr::EVRButtonId a_id)
    {
        return 1ull << a_id;
    }

    const FO4FlatVR_Hand* HandFor(const vr::TrackedDeviceIndex_t a_index)
    {
        if (!vrshim::bridge::valid() || (a_index != kLeft && a_index != kRight))
            return nullptr;
        return &vrshim::bridge::frame().hands[a_index == kLeft ? 0 : 1];
    }

    void PoseToMatrix(const FO4FlatVR_Pose& a_pose, vr::HmdMatrix34_t& a_m)
    {
        const float x = a_pose.q[0], y = a_pose.q[1], z = a_pose.q[2], w = a_pose.q[3];
        a_m.m[0][0] = 1 - 2 * (y * y + z * z), a_m.m[0][1] = 2 * (x * y - z * w), a_m.m[0][2] = 2 * (x * z + y * w), a_m.m[0][3] = a_pose.p[0];
        a_m.m[1][0] = 2 * (x * y + z * w), a_m.m[1][1] = 1 - 2 * (x * x + z * z), a_m.m[1][2] = 2 * (y * z - x * w), a_m.m[1][3] = a_pose.p[1];
        a_m.m[2][0] = 2 * (x * z - y * w), a_m.m[2][1] = 2 * (y * z + x * w), a_m.m[2][2] = 1 - 2 * (x * x + y * y), a_m.m[2][3] = a_pose.p[2];
    }

    void FillPose(const vr::TrackedDeviceIndex_t a_index, vr::TrackedDevicePose_t& a_out)
    {
        a_out = {};
        if (!vrshim::bridge::valid())
            return;
        const auto& f = vrshim::bridge::frame();
        if (a_index == kHmd) {
            a_out.bDeviceIsConnected = true;
            a_out.bPoseIsValid = f.headValid != 0;
            PoseToMatrix(f.head, a_out.mDeviceToAbsoluteTracking);
        } else if (const auto* h = HandFor(a_index)) {
            a_out.bDeviceIsConnected = f.active != 0;
            a_out.bPoseIsValid = h->gripValid != 0;
            PoseToMatrix(h->gripPose, a_out.mDeviceToAbsoluteTracking);
        }
        a_out.eTrackingResult = a_out.bPoseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
    }

    bool FillState(const vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state)
    {
        if (!a_state)
            return false;
        *a_state = {};
        const auto* h = HandFor(a_index);
        if (!h || !vrshim::bridge::frame().active)
            return false;
        a_state->unPacketNum = static_cast<std::uint32_t>(vrshim::bridge::frame().seq);
        std::uint64_t pressed = 0, touched = 0;
        if (h->a)
            pressed |= Bit(vr::k_EButton_A);
        if (h->b)
            pressed |= Bit(vr::k_EButton_ApplicationMenu);
        if (h->grip >= 0.7f)
            pressed |= Bit(vr::k_EButton_Grip);
        if (h->trigger >= 0.9f)
            pressed |= Bit(vr::k_EButton_SteamVR_Trigger);
        if (h->stickClick)
            pressed |= Bit(vr::k_EButton_SteamVR_Touchpad);
        touched = pressed;
        if (h->trigger > 0.05f)
            touched |= Bit(vr::k_EButton_SteamVR_Trigger);
        if (std::sqrt(h->stickX * h->stickX + h->stickY * h->stickY) > 0.1f)
            touched |= Bit(vr::k_EButton_SteamVR_Touchpad);
        a_state->ulButtonPressed = pressed;
        a_state->ulButtonTouched = touched;
        a_state->rAxis[0] = { h->stickX, h->stickY };
        a_state->rAxis[1] = { h->trigger, 0.0f };
        a_state->rAxis[2] = { h->grip, 0.0f };
        return true;
    }

    class FlatVRSystem final : public vr::IVRSystem
    {
    public:
        vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole a_role) override
        {
            return a_role == vr::TrackedControllerRole_LeftHand ? kLeft : a_role == vr::TrackedControllerRole_RightHand ? kRight : vr::k_unTrackedDeviceIndexInvalid;
        }

        vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t a_index) override
        {
            return a_index == kLeft ? vr::TrackedControllerRole_LeftHand : a_index == kRight ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_Invalid;
        }

        vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t a_index) override
        {
            return a_index == kHmd ? vr::TrackedDeviceClass_HMD : (a_index == kLeft || a_index == kRight) ? vr::TrackedDeviceClass_Controller : vr::TrackedDeviceClass_Invalid;
        }

        bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t a_index) override
        {
            return a_index <= kRight && vrshim::bridge::valid();
        }

        bool GetControllerState(vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state, std::uint32_t) override
        {
            return FillState(a_index, a_state);
        }

        bool GetControllerStateWithPose(vr::ETrackingUniverseOrigin, vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state, std::uint32_t,
            vr::TrackedDevicePose_t* a_pose) override
        {
            if (a_pose)
                FillPose(a_index, *a_pose);
            return FillState(a_index, a_state);
        }

        void TriggerHapticPulse(vr::TrackedDeviceIndex_t a_index, std::uint32_t, unsigned short a_micros) override
        {
            const auto* api = vrshim::bridge::api();
            if (api && api->Vibrate && (a_index == kLeft || a_index == kRight)) {
                api->Vibrate(a_index == kLeft ? 0 : 1, 1.0f, a_micros * 1e-6f);
            }
        }

        void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count) override
        {
            for (std::uint32_t i = 0; a_poses && i < a_count; ++i)
                FillPose(i, a_poses[i]);
        }

        bool IsInputAvailable() override
        {
            return true;
        }

        bool GetTimeSinceLastVsync(float* a_seconds, std::uint64_t* a_frame) override
        {
            if (a_seconds)
                *a_seconds = 0.0f;
            if (a_frame)
                *a_frame = vrshim::bridge::valid() ? vrshim::bridge::frame().seq : 0;
            return vrshim::bridge::valid();
        }

#include "IVRSystemDefaults.inl"
    };

    FlatVRSystem g_system;
}

namespace vr
{
    void* VR_CALLTYPE VR_GetGenericInterface(const char* a_version, vr::EVRInitError* a_error)
    {
        // only once fo4flatvr handed us the bridge: before that (or dormant) everything is "no runtime"
        if (vrshim::bridge::api() && a_version && std::strcmp(a_version, vr::IVRSystem_Version) == 0) {
            if (a_error)
                *a_error = vr::VRInitError_None;
            return static_cast<vr::IVRSystem*>(&g_system);
        }
        if (a_error)
            *a_error = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }

    bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char* a_version)
    {
        return a_version && std::strcmp(a_version, vr::IVRSystem_Version) == 0;
    }

    // the token changes when the bridge appears, so a cached null VRSystem() is re-queried
    uint32_t VR_CALLTYPE VR_GetInitToken()
    {
        return vrshim::bridge::api() ? 2u : 1u;
    }

    uint32_t VR_CALLTYPE VR_InitInternal2(vr::EVRInitError* a_error, vr::EVRApplicationType, const char*)
    {
        if (a_error)
            *a_error = vrshim::bridge::api() ? vr::VRInitError_None : vr::VRInitError_Init_InstallationNotFound;
        return VR_GetInitToken();
    }

    void VR_CALLTYPE VR_ShutdownInternal()
    {}

    bool VR_CALLTYPE VR_IsHmdPresent()
    {
        return vrshim::bridge::api() != nullptr;
    }

    bool VR_CALLTYPE VR_IsRuntimeInstalled()
    {
        return vrshim::bridge::api() != nullptr;
    }

    const char* VR_CALLTYPE VR_RuntimePath()
    {
        return "fo4flatvr (OpenXR)";
    }

    const char* VR_CALLTYPE VR_GetVRInitErrorAsSymbol(vr::EVRInitError)
    {
        return "VRInitError_FRIKFlat";
    }

    const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError)
    {
        return "FRIK-flat: OpenVR is emulated from fo4flatvr's OpenXR snapshot";
    }
}
