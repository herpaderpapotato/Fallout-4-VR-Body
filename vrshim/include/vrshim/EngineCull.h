#pragma once
// FRIK-flat engine cull (22 Nov, VATS head agent): every FRIK-flat node hide / show goes through here.
//
// The engine's NiAVObject::SetAppCulled (vtable 0x2D, flat 16C86C0) sets the app-cull bit (flags bit 0) AND marks the node and
// every parent up to one already marked with 0x20000000 ("changed below"). Flipping bit 0 directly skipped the mark: a node
// shown again outside the frame it was hidden in could stay undrawn (22 Nov: the player's head in VATS playback).
//
// Set(): bit 0 is written at once on the caller's thread (FRIK's frame: every reader sees the new state immediately, the hide /
// show timing is unchanged), then the engine's own SetAppCulled runs on the main thread (fo4flatvr bridge v9 RunOnMain = the
// tq main queue at OnIdle C34794, after the frame's job threads, before the next render): it re-applies the same state through
// the engine, so the node and its parents (shared scene-graph nodes) get the engine's mark from the main thread only.
// fo4flatvr.ini [FRIK] bEngineCullFlag=0 (live) or a fo4flatvr without bridge v9: the direct bit only, as before.
#include "RE/Fallout.h"

namespace vrshim::cull
{
    void Set(RE::NiAVObject* a_node, bool a_culled);
    [[nodiscard]] bool On();

    struct Stats
    {
        std::uint64_t sets = 0, queued = 0, applied = 0, skippedChanged = 0, fallbackDirect = 0;
    };
    [[nodiscard]] Stats GetStats();
}
