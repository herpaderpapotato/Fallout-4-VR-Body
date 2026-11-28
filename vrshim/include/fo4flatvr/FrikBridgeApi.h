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
// v5 (20 Nov, calibrate-only body height): RecentreCount - fo4flatvr's recentre counter (F8, the recentre gesture, the pipe,
// the automatic first-pose anchor and its placeholder-pose re-anchor). FRIK-flat's auto body height (C36) samples the camera
// height only after it changes. Backwards compatible as v4: the field is appended, fo4flatvr serves v3..v5 with one table,
// FRIK.dll asks for v5, then v4, then v3, and reads RecentreCount only when version >= 5.
// v6 (20 Nov, VR settings panel + no-scaling switch): NoScaling - fo4flatvr.ini [General] bNoScaling (live): FRIK-flat uses
// the game's default body scale (no FRIK.toml PlayerHeight, no C36 auto height, no arm-length bone scaling). SetFrikConfigApi -
// FRIK.dll hands fo4flatvr a table (FO4FlatVR_FrikConfigApi) through which the VR settings panel previews, discards and saves
// FRIK.toml values the way Mods Config FO4 VR does (FRIK's session overrides + FRIK's own saveConfigValues). Backwards
// compatible as v4/v5: both fields are appended, fo4flatvr serves v3..v6 with one table, FRIK.dll asks v6, v5, v4, v3 and
// uses them only when version >= 6.
// v7 (20 Nov, live settings): ConfigRevision - fo4flatvr's settings revision (ConfigStore.h; increments whenever a merged
// fo4flatvr.ini / MCM / VR-panel value changes). FRIK-flat re-reads its [FRIK] switches when it moves (vrshim iniIntLive), so
// they apply without a restart. Appended as v4..v6; FRIK.dll asks v7 first and reads it only when version >= 7.
// v8 (21 Nov, ROCK-flat phase 1): SetInputClaims - a physics-hands client (ROCK.dll, owner FO4FLATVR_CLAIM_OWNER_ROCK) claims
// controller inputs per physical hand (FO4FLATVR_CLAIM_* bits). While claimed, and only in gameplay / quick loot (menus,
// the wrist Pip-Boy, dialogue, VATS, workshop and lockpicking keep every input; menus keep the grip), fo4flatvr's bindings
// treat the input as released: no pad output, no gesture (stick click), no tap/hold timer, and the glyph remap does not name
// a claimed control for a gameplay action. The raw state in GetFrame / FRIK-flat's IVRSystem shim is never masked (the
// claimer reads it there). Masks of all owners are ORed. fo4flatvr.ini [ROCK] bInputClaims=1 (live; 0 = claims are stored
// but ignored, SetInputClaims returns 0). Appended as v4..v7; a client asks v8 and calls it only when version >= 8.
// v9 (22 Nov, engine cull): RunOnMain - queue a_fn(a_ctx) on fo4flatvr's main-thread queue (tq::RunOnMain: OnIdle C34794,
// after the frame's job threads). FRIK-flat routes its node hides / shows (NiAVObject::SetAppCulled, which also marks every
// parent changed - shared scene-graph nodes) through it. Returns 1 = queued, 0 = not (the caller runs it itself).
// v10 (28 Nov, ROCK-flat R30): SetInputAdds - the presses a client's remap adds (ControllerBindings.h SetAdds).
// v11 (28 Nov, ROCK-flat R32): SetPreFrameCallback - the graph-output point before the floating gun (NativeGraphOutput).
// v12 (28 Nov, ROCK-flat R35): SetKickCallback - the floating gun's FO4VR kickback goes through FRIK's recoil controllers.
#define FO4FLATVR_FRIK_BRIDGE_VERSION 13u
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
// v12: kick callback. FO4VR's engine writes the weapon kickback (translation (0, -d, 0) + the aim model's recoil rotation)
// into PrimaryWeaponKickbackRecoilNode's local (child of PrimaryWeaponToWandNode = wand . T(0,-4,1)) and FRIK offers that
// local to its recoil controllers (ROCK). On flat fo4flatvr's floating gun computes the same kick; once per placed frame,
// BEFORE it places the weapon (player-update job thread, before FRIK's frame callback), it calls this with a_kick = that
// local as an NiTransform (16 floats: 3 rows of 4 (rotation, CommonLib NiMatrix3 layout), translate xyz, scale 1).
// Return 0 = nobody controls it: the gun applies a_kick itself. Return 1 = a controller took it: a_out (same layout) is the
// kick the gun applies instead (identity = none: the controller delivers it through its own weapon pose, or recoil is off).
typedef uint8_t (*FO4FlatVR_KickCallback)(void* a_ctx, const float a_kick[16], float a_out[16]);

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

// v6: FRIK.dll's config table for fo4flatvr's VR settings panel (FRIK.toml values by section + key, values as TOML text:
// "true"/"false", "3", "8.6"). Every call may come from any thread; Preview / Discard / Save are queued and applied on FRIK's
// next frame (game thread, also paused frames, and the smooth-movement camera step), so they never race FRIK's own reads.
typedef struct FO4FlatVR_FrikConfigApi
{
    uint32_t size;  // sizeof(FO4FlatVR_FrikConfigApi) as FRIK.dll built it
    uint32_t pad0;
    // the value in effect (session override if any, else FRIK.toml) into a_out (NUL-terminated); 0 = the file has no such key
    uint8_t (*Get)(const char* a_section, const char* a_key, char* a_out, uint32_t a_cap);
    // live preview: a session override (never written), the members reload as for a file change (Mods Config's mechanism)
    uint8_t (*Preview)(const char* a_section, const char* a_key, const char* a_value);
    // drop this panel's preview of the key (back to FRIK.toml's value)
    uint8_t (*Discard)(const char* a_section, const char* a_key);
    // write the value to FRIK.toml through FRIK's own saveConfigValues (keeps the file's layout), then drop the preview
    uint8_t (*Save)(const char* a_section, const char* a_key, const char* a_value);
    // one status line (config file, queue, applied / saved / refused counts, last message) into a_out
    void (*Status)(char* a_out, uint32_t a_cap);
    // the config file FRIK.dll loaded (absolute path; Mods Config's clientState "configFile")
    void (*ConfigFile)(char* a_out, uint32_t a_cap);
} FO4FlatVR_FrikConfigApi;

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
    // v5: number of recentres so far (any thread; head::RecenterCount: explicit F8 / gesture / pipe recentres and the
    // automatic anchors on the first valid head pose / a start-up placeholder pose jump). A change = the user calibrated.
    uint32_t (*RecentreCount)();
    // v6: fo4flatvr.ini [General] bNoScaling (any thread, live: re-read about once a second). 1 = the game's default scale.
    uint8_t (*NoScaling)();
    // v6: FRIK.dll registers (null = unregisters) its config table for the VR settings panel (pointer kept; static storage).
    void (*SetFrikConfigApi)(const FO4FlatVR_FrikConfigApi* a_api);
    // v7: settings revision (any thread, cheap): a change = re-read the ini keys (IniPath) that are meant to be live.
    uint64_t (*ConfigRevision)();
    // v8: input claims of one owner (any thread, cheap; call whenever the wanted set changes, or every frame). a_left /
    // a_right = FO4FLATVR_CLAIM_* bits of the physical left / right controller; 0, 0 releases the owner's claims. Returns 1
    // when fo4flatvr honours claims now ([ROCK] bInputClaims=1), 0 when they are stored but ignored (switch off, bad owner).
    uint8_t (*SetInputClaims)(uint32_t a_owner, uint32_t a_left, uint32_t a_right);
    // v9: run a_fn(a_ctx) on the main thread (queued; any thread may call). 1 = queued.
    uint8_t (*RunOnMain)(void (*a_fn)(void*), void* a_ctx);
    // v10: input adds of one owner (any thread, cheap; call when the set changes). a_left / a_right = FO4FLATVR_CLAIM_* bits
    // the game should see PRESSED on that physical hand although they are not (a client's remap: ROCK's left-hand fire
    // presents the physical left trigger as the right trigger; its button layout moving an action). Same contexts and switch
    // as the claims (gameplay / quick loot, [ROCK] bInputClaims). Returns 1 when honoured now.
    uint8_t (*SetInputAdds)(uint32_t a_owner, uint32_t a_left, uint32_t a_right);
    // v11: pre-frame callback (null clears): called at the same C351F3 player-update call as SetFrameCallback's, but FIRST -
    // before the floating gun and the original notify move the first-person rig - so the client sees the engine's own
    // animation-graph output of this frame (FRIK-flat fires FRIK's NativeGraphOutput phase from it). Player-update job
    // thread; not called in pause-time frames. Returns 1 when the C351F3 hook is installed.
    uint8_t (*SetPreFrameCallback)(FO4FlatVR_FrameCallback a_cb, void* a_ctx);
    // v12: kick callback (null clears; see FO4FlatVR_KickCallback). Returns 1 when the floating gun will call it now
    // ([ROCK] bRecoilControl=1, live); 0 = stored but not called (switch off).
    uint8_t (*SetKickCallback)(FO4FlatVR_KickCallback a_cb, void* a_ctx);
    // v13 (28 Nov, ROCK-flat R50): fo4flatvr's input context of the last mapped frame (ControllerBindings.h InputContext:
    // 0 gameplay, 1 quick loot, 2 Pip-Boy, 3 quick menu, 4 V.A.T.S., 5 workshop, 6 lockpick, 7 dialogue, 8 other menu). Any thread.
    uint8_t (*GetInputContext)(void);
} FO4FlatVR_FrikBridge;

// v8 input claim bits (per physical hand)
#define FO4FLATVR_CLAIM_GRIP 0x01u        // grip (squeeze)
#define FO4FLATVR_CLAIM_TRIGGER 0x02u     // trigger
#define FO4FLATVR_CLAIM_LOWER 0x04u       // lower face button (right A, left X)
#define FO4FLATVR_CLAIM_UPPER 0x08u       // upper face button (right B, left Y)
#define FO4FLATVR_CLAIM_STICKCLICK 0x10u  // thumbstick click
#define FO4FLATVR_CLAIM_ALL 0x1Fu
// owners (1..7)
#define FO4FLATVR_CLAIM_OWNER_ROCK 1u
#define FO4FLATVR_CLAIM_OWNER_MAX 7u

typedef const FO4FlatVR_FrikBridge* (*FO4FLATVR_GetFrikBridge_t)(uint32_t a_version);

#ifdef __cplusplus
}
#endif
