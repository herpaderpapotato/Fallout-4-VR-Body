#pragma once
// FRIK-flat C23: the player's third-person body is skinned from a per-frame copy of FRIK's pose (vrshim/src/StableSkin.cpp).
#include "RE/Fallout.h"

namespace vrshim::skin
{
    struct Stats
    {
        std::uint32_t instances = 0, bones = 0;
        std::uint64_t rebuilds = 0;
        // C23b (22 Nov): last restore - entries put back on their live bone that the remembered-pointer restore missed
        // (they would have kept pointing at a freed copy: that piece skinned from garbage outside first person)
        std::uint32_t lastRestoreMissed = 0, lastRestoreInstances = 0;
        std::uint64_t restoreMissedTotal = 0;
    };
    // End of FRIK's frame (game thread): copy the bone worlds of every skin instance under a_root and point the instances at them.
    void refresh(RE::NiAVObject* a_root);
    // Skeleton released / body off: the instances still under a_root read the live bones again.
    void restore(RE::NiAVObject* a_root);
    [[nodiscard]] const Stats& stats() noexcept;
}
