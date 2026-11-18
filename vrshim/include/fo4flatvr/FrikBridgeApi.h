#pragma once
// fo4flatvr <-> FRIK-flat (FRIK.dll built against flat 1.11.240, G:\Fallout4ToVR\frik-flat) C ABI.
// Plain C types only: this header is included by both DLLs (frik-flat includes it by relative path).
//
// FRIK.dll calls GetProcAddress(GetModuleHandleW(L"fo4flatvr.dll"), "FO4FLATVR_GetFrikBridge")(FO4FLATVR_FRIK_BRIDGE_VERSION)
// from its F4SEPlugin_Load. A null result means FRIK-flat must stay dormant (fo4flatvr.ini [FRIK] bEnable=0, or a version
// mismatch): FRIK.dll then installs no hooks and patches nothing.
// v2: FRIK.dll does not patch the per-frame call site itself. fo4flatvr owns one hook at C351F3 (PlayerUpdateEvent notify at
// the end of Main::OnIdle_UpdatePlayer, byte-checked, chained with fo4flatvr's floating gun) and calls the callback registered
// with SetFrameCallback from there, before the floating gun and the original notify.
// v3: [FRIK] bBody / bSmoothMovement. BodyEnabled() = 0 -> FRIK.dll loads in smooth-movement-only mode (config + the camera
// offset callback only: no hooks, no patches, no skeleton, no frame callback, never SetFrikActive -> the floating gun stays).
// SetCameraOffsetCallback: FRIK's SmoothMovementVR, which on FO4VR offsets PlayerWorldNode from Main::OnIdle (VR D83EC4 ->
// 1BA7BA0, VR-only), is stepped by fo4flatvr once per first-person camera update instead and its offset is added to the
// camera position fo4flatvr builds (GameBridge.cpp FirstPersonState hooks) - the flat equivalent of PlayerWorldNode.
//
// Poses: the OpenXR reference space the plugin renders with (metres, +X right, +Y up, -Z forward = OpenVR's tracking
// space convention) and, for convenience, head-relative in the game's camera basis (X right, Y forward, Z up, metres) -
// the same frames fo4flatvr's floating gun uses (FloatingGunInput.cpp), i.e. relative to the head pose handed to the camera
// for the next game frame.
#include <stdint.h>

// v4 (16 Nov, gestures): GetSelfie - fo4flatvr's VRIK-style selfie (both hands up -> FRIK's mirror body in front of the
// viewer, steered by the wrist roll; Gestures.h). Backwards compatible: fo4flatvr hands the same table to a v3 or v4 request
// (fields are only appended), and FRIK.dll asks for v4 first, then v3, and calls GetSelfie only when version >= 4.
#define FO4FLATVR_FRIK_BRIDGE_VERSION 4u
#define FO4FLATVR_FRIK_BRIDGE_MIN_VERSION 3u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FO4FlatVR_Pose
{
    float q[4];  // orientation x y z w
    float p[3];  // position
} FO4FlatVR_Pose;

typedef struct FO4FlatVR_HeadRel
{
    uint8_t valid;
    uint8_t pad[3];
    float   pos[3];     // grip position, metres, camera basis
    float   rot[3][3];  // grip frame (X = grip +X, Y = grip -Z through the fist, Z = grip +Y) -> camera basis, r[row][col]
} FO4FlatVR_HeadRel;

typedef struct FO4FlatVR_Hand
{
    uint8_t           gripValid, aimValid;
    uint8_t           a, b;         // right: A / B; left: X / Y
    uint8_t           stickClick;
    uint8_t           menu;         // left only
    uint8_t           pad[2];
    float             stickX, stickY;  // -1..1, +Y up
    float             trigger, grip;   // 0..1
    FO4FlatVR_Pose    gripPose;        // reference space
    FO4FlatVR_Pose    aimPose;         // reference space
    FO4FlatVR_HeadRel gripInHead;      // relative to `head`
} FO4FlatVR_Hand;

typedef struct FO4FlatVR_Frame
{
    uint32_t          size;        // sizeof(FO4FlatVR_Frame), set by the caller
    uint32_t          pad0;
    uint64_t          seq;         // increments once per XR frame (Present)
    double            ageSeconds;  // time since this snapshot was published
    uint8_t           active;      // controller actions synced and session focused
    uint8_t           worldView;   // first-person world view on the headset (not the cinema quad / flat fallback)
    uint8_t           leftHanded;
    uint8_t           headValid;
    float             unitsPerMeter;  // game units per metre the plugin uses for head position (default 70)
    FO4FlatVR_Pose    head;           // head pose handed to the camera for the next game frame (reference space)
    FO4FlatVR_Hand    hands[2];       // [0] left, [1] right (physical sides; leftHanded says which is primary)
} FO4FlatVR_Frame;

// Game frame callback (job thread that runs the player update; the frame's animation, scenegraph and camera update are done).
// Also, while the game is paused (UI::menuMode != 0: no player update), once per frame from Main::OnIdle C33E62 on the main
// thread (FO4VR's own FRIK slot; [FRIK] bPausedFrame=1, never in the same frame as the C351F3 call). Must not throw.
typedef void (*FO4FlatVR_FrameCallback)(void* a_ctx);

// v3: camera offset (FRIK smooth movement). Called by fo4flatvr once per first-person camera update (the thread running
// FirstPersonState::Update, before the camera position is built; the player's position for this frame is final). Writes the
// world-space offset in game units to add to the camera position (FO4VR: PlayerWorldNode local translate without FRIK's HMD
// height offset) and returns 1, or returns 0 = no offset this frame. Must not throw.
typedef uint8_t (*FO4FlatVR_CameraOffsetCallback)(void* a_ctx, float a_offset[3]);

// v4: selfie request (fo4flatvr gestures). FRIK.dll polls it once per frame.
typedef struct FO4FlatVR_Selfie
{
    uint32_t size;       // sizeof(FO4FlatVR_Selfie), set by the caller
    uint32_t seq;        // increments on every on/off toggle
    uint8_t  active;     // selfie mode wanted (FRIK's setSelfieMode)
    uint8_t  follow;     // the hands are up: (re)place the body distanceM in front of the camera, facing it, and steer it
    uint8_t  pad[2];
    float    distanceM;  // metres in front of the camera (horizontal)
    float    yawRad;     // extra turn about up from "facing the viewer", counter-clockwise seen from above (wrist roll)
} FO4FlatVR_Selfie;

typedef struct FO4FlatVR_FrikBridge
{
    uint32_t version;  // FO4FLATVR_FRIK_BRIDGE_VERSION
    uint32_t pad0;
    // Latest controller/head snapshot (any thread). False before the first XR frame.
    uint8_t (*GetFrame)(FO4FlatVR_Frame* a_out);
    // hand 0 = left, 1 = right; amplitude 0..1; seconds <= 0 = shortest pulse. Queued, played on the XR thread.
    void (*Vibrate)(int32_t a_hand, float a_amplitude, float a_seconds);
    // FRIK-flat is (in)active: while active fo4flatvr's floating-gun weapon placement steps aside (FloatingGun.h).
    void (*SetFrikActive)(uint8_t a_active);
    // fo4flatvr.ini path (UTF-8), for FRIK-flat's own [FRIK] keys.
    const char* (*IniPath)();
    // v2: register (null = unregister) the per-game-frame callback. 1 = fo4flatvr's C351F3 hook is installed and will call
    // it; 0 = no hook (byte check failed): FRIK-flat gets no frame.
    uint8_t (*SetFrameCallback)(FO4FlatVR_FrameCallback a_cb, void* a_ctx);
    // v3: fo4flatvr.ini [FRIK] bBody. 0 = FRIK.dll runs smooth movement only (see above).
    uint8_t (*BodyEnabled)();
    // v3: register (null = unregister) the camera offset callback. 1 = fo4flatvr's camera hook will call it ([FRIK]
    // bSmoothMovement=1 and the first-person camera hook installed); 0 = it will not.
    uint8_t (*SetCameraOffsetCallback)(FO4FlatVR_CameraOffsetCallback a_cb, void* a_ctx);
    // v4: the current selfie request (any thread). 0 = none available (a_out untouched).
    uint8_t (*GetSelfie)(FO4FlatVR_Selfie* a_out);
} FO4FlatVR_FrikBridge;

typedef const FO4FlatVR_FrikBridge* (*FO4FLATVR_GetFrikBridge_t)(uint32_t a_version);

#ifdef __cplusplus
}
#endif
