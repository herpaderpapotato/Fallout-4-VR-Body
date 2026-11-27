#pragma once
// FRIK-flat C43 (20 Nov): the third-person body graph run in first person (analysis/first-person-body-mods.md).
//
// In first person the player's BSAnimationGraphManager has two graphs: 0 = the third-person body graph, 1 = the
// first-person arms graph (active, manager+0xD8). The engine updates only the active graph, and afterwards
// PlayerCharacter::PostUpdateAnimationGraphManager (D7E380, holder vtable slot 0x17) copies the first-person
// skeleton's locals into the third-person bone tree for a mapped bone list - which is why the third-person legs never
// walked in first person (C42 rollback: 0 of 120 frames). This module, while the engine is in first person:
//  - activates graph 0 next to graph 1, feeds it the manager's bound channels (Speed / Direction / TurnDelta / Pitch ...),
//    mirrors the events the engine sends to graph 1 (move / sneak / jump / swim / draw), updates and generates it right
//    after the engine's own update (the BSAnimationGraphManager::Update call in 818C80) - TFP's direct-call design: our
//    own output-event array, discarded, so graph 0 never dispatches footsteps / hits / attack events;
//  - skips the D7E380 bone copy for that update, so the third-person bones keep graph 0's pose for FRIK (C37 capture, C42);
//  - hands graph 0 back (deactivated, its events discarded) before the engine switches to it itself (SwitchBehaviorGraph:
//    knock-down, third person), before a save, while seated / in furniture / in a paired animation / knocked down / dead,
//    and when the switch goes off.
// Off: fo4flatvr.ini [FRIK] bBodyGraph=0 (live; the engine's first-person behaviour, graph 0 deactivated).
#include <cstdint>

namespace frik::bodygraph
{
    // F4SEPlugin_Load (FRIK::onModLoaded): byte-checked hooks + the F4SE message listener (pre-save / pre-load).
    void install();

    // FRIK's frame (main thread, before the skeleton update): diag window, stuck detection, the posing flag below.
    void frameTick();

    // True when graph 0 posed the third-person body since FRIK's previous frame (C42 then reads a real locomotion pose).
    [[nodiscard]] bool posingLegs() noexcept;

    // [FRIK] bBodyGraph (live).
    [[nodiscard]] bool enabled() noexcept;

    // The engine's move direction while graph 0 runs: graph 1's "Direction" variable, a fraction of a turn clockwise from the
    // actor's facing (0 forward, 0.25 right, 0.5 back, 0.75 left). False when the body graph is not running.
    bool engineMoveDirection(float& a_turns) noexcept;
}
