#pragma once
// FRIK-flat (20 Nov): FRIK's options in fo4flatvr's in-VR settings panel (user: "The frik config ui, was meant to configure
// frik's options, like the frik menu used to ... something is needed for FRIK's menus").
//
// FRIK's own in-world config UI (vrui panels) does not render on flat (no FO4VR compositor / player-space pass), so fo4flatvr's
// ImGui panel renders FRIK's own Mods Config spec (Data/ModsConfigFO4/Config/FRIK/spec.json, its generic spec.json renderer
// plugin/src/plugin/SpecPages.h) and edits FRIK.toml through the table registered here (bridge v6,
// FO4FlatVR_FrikConfigApi), with exactly Mods Config FO4 VR's mechanism (fw-src/modsconfig/ConfigPreviews.cpp):
//   preview = a session override of FRIK's config (owner "fo4flatvr VR panel"), the members reload as for a file change
//             (live; never written);
//   discard = the override cleared;
//   save    = FRIK's own ConfigBase::saveConfigValues (TOML editor: only the values change, comments / layout stay), then the
//             override cleared with a reload that reads the file.
// The calls come from fo4flatvr's Presenter thread; they are queued and applied by drain() on FRIK's own frame (game thread,
// also the paused frame), and in the smooth-movement camera step (bBody=0 has no FRIK frame), so a reload never races FRIK.

namespace frik::flatpanel
{
    // F4SEPlugin_Load, after FRIK's config is loaded: hands the table to fo4flatvr (no-op with a bridge older than v6)
    void registerWithBridge();
    // FRIK's frame start / the smooth-movement camera step: apply the queued previews, discards and saves (cheap when empty)
    void drain();
}
