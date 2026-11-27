#pragma once
// FRIK-flat C44 (20 Nov, VATS agent): first / third person switch kept in step with FRIK's shown body.
//
// PlayerCharacter::DoShow1stPerson(show) (D78C30) first stores the new view in player+0xDFE bit 4 (vfunc 0x470,
// "third person"), then returns early when the cull flags of the two 3D roots already look like the requested view
// (exactly one root culled, and it is the one the switch would hide). FRIK's body patch (GameHooks patchBody, D78CFE)
// never culls the third-person root, so the vanilla pair "first-person root shown / third-person root culled" no longer
// exists; while VATS targeting culls the first-person root (GameState::ChangeState(2)) the pair reads "third person
// already", and the VATS playback camera's DoShow1stPerson(false) returns after setting the flag: no
// SwitchBehaviorGraph, no fire-node cache reset. Then (user report 20 Nov 23:2x):
//  - VATS playback: the third-person body is not animated (graph 1 stays active, graph 0 handed back by C43);
//  - VATS shots leave from the stale first-person ProjectileNode in player-relative space (fo4flatvr VatsShotFix);
//  - the switch back to first person runs completely and toggles the graph (SwitchBehaviorGraph alternates 0 <-> 1),
//    so the third-person graph stays active in first person: graph 1 no longer poses the first-person skeleton, its
//    "Camera" bone stays at the last (sneak) height = "stuck in crouch height".
// Fix: the three DoShow1stPerson calls (D6890F Revert, D7E31F PreLoadAnimationGraphManager, C4F142 task unpack) are
// wrapped; when the call really changes the view (flag differs from the request) and the cull flags would make it return
// early, the first-person root's cull flag is made equal to the third-person root's first, so the engine's switch runs
// completely (FRIK's patch still keeps the third-person root shown). Safety net in tick(): first person (flag clear,
// camera state 0/4, not seated / paired / knocked down / dead) with the manager's active graph 0 for a second ->
// PlayerCharacter::SwitchBehaviorGraph (D79530) once, back to graph 1.
// Off: fo4flatvr.ini [FRIK] bPovSwitchSync=0 (live; the engine's behaviour with FRIK's body patch, as before).
namespace frik::povswitch
{
    // F4SEPlugin_Load (after hook::patchAll): byte-checked call-site wrappers.
    void install();

    // FRIK's frame (player update thread, outside the animation update): the graph / view safety net.
    void tick();
}
