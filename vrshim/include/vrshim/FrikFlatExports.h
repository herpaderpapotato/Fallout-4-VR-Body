#pragma once
// FRIK-flat host exports for other flat ports that run on top of FRIK.dll (ROCK-flat, analysis/rock-flat.md phase 1).
// Plain C ABI: GetProcAddress(GetModuleHandleW(L"FRIK.dll"), "<name>"). Defined in frik-src/api/FRIKApiV2.cpp.
//
// FO4VR keeps its VR player node table at PlayerCharacter+0x6E0 (CommonLibF4VR RE::VRPlayerNodes). Flat has no such member;
// FRIK-flat's vrshim keeps a table with the same layout and member names (FlatCompat.h RE::VRPlayerNodes), posed from
// fo4flatvr's OpenXR snapshot every frame before FRIK's own update (vrshim::bridge::frameUpdate). ROCK-flat reads the same
// nodes FRIK-flat drives, so its hands ride FRIK's hand frames.
#include <stdint.h>

#define FRIKFLAT_HOST_EXPORTS_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// Version of this export set (FRIKFLAT_HOST_EXPORTS_VERSION).
typedef uint32_t (*FRIKFLAT_GetHostExportsVersion_t)(void);

// The flat VR player node table (RE::VRPlayerNodes layout of FlatCompat.h, 40 pointers). Never null once FRIK.dll is loaded;
// its members stay null until the first FRIK-flat frame created the nodes (FRIKFLAT_VrNodesReady). Game thread.
typedef void* (*FRIKFLAT_GetVRPlayerNodes_t)(void);
typedef uint8_t (*FRIKFLAT_VrNodesReady_t)(void);

// 1 = the frame FRIK-flat is running now is a pause-time frame (UI::menuMode != 0: fo4flatvr calls FRIK's frame from
// Main::OnIdle C33E62; no player update, no animation, Havok does not step). Valid from FRIK's frame begin to its end
// (frame-phase callbacks included). Game thread.
typedef uint8_t (*FRIKFLAT_PausedFrame_t)(void);

// FRIK-flat frames run so far (incremented at each frame begin, paused frames included). Game thread.
typedef uint64_t (*FRIKFLAT_FrameCount_t)(void);

// C47 (28 Nov): flat's first-person rig is player-relative; absolute world = rig world + a_out[0..2] (translation only).
// 1 = written. FRIK-flat's own rig -> world conversions use the same helper. Game thread (FRIK's frame).
typedef uint8_t (*FRIKFLAT_GetFirstPersonRigOffset_t)(float* a_out);

#ifdef __cplusplus
}
#endif
