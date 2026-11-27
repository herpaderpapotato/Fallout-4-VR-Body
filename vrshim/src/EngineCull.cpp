// FRIK-flat engine cull (vrshim/EngineCull.h): bit 0 at once, the engine's SetAppCulled on the main thread.
#include "vrshim/EngineCull.h"
#include "vrshim/Bridge.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace vrshim::cull
{
    namespace
    {
        struct Pending
        {
            RE::NiPointer<RE::NiAVObject> node;  // keeps the node alive until the main thread ran
            bool                          culled;
        };
        std::mutex           g_lock;
        std::vector<Pending> g_pending;
        bool                 g_drainQueued = false;
        std::atomic<std::uint64_t> g_sets{ 0 }, g_queued{ 0 }, g_applied{ 0 }, g_skipped{ 0 }, g_direct{ 0 };

        void DirectBit(RE::NiAVObject* a_node, bool a_culled)
        {
            if (a_culled) {
                a_node->flags.flags |= 1;
            } else {
                a_node->flags.flags &= ~static_cast<std::uint64_t>(1);
            }
        }

        // main thread (tq main queue)
        void Drain(void*)
        {
            std::vector<Pending> work;
            {
                std::scoped_lock l(g_lock);
                work.swap(g_pending);
                g_drainQueued = false;
            }
            for (auto& p : work) {
                auto* n = p.node.get();
                if (!n) continue;
                if (((n->flags.flags & 1) != 0) != p.culled) {
                    ++g_skipped;  // changed again since (a later entry carries the newer state)
                    continue;
                }
                // the engine call no-ops on an unchanged state: step it through the other state, both on the main thread with
                // no render running (OnIdle C34794): it sets bit 0 and marks the node and its parents (0x20000000)
                n->SetAppCulled(!p.culled);
                n->SetAppCulled(p.culled);
                ++g_applied;
            }
        }
    }

    bool On()
    {
        return vrshim::bridge::iniIntLive("bEngineCullFlag", 1) != 0;
    }

    void Set(RE::NiAVObject* a_node, bool a_culled)
    {
        if (!a_node) return;
        ++g_sets;
        const bool changed = ((a_node->flags.flags & 1) != 0) != a_culled;
        DirectBit(a_node, a_culled);
        if (!changed || !On()) {
            return;  // nothing changed (callers re-assert every frame), or switched off: the direct bit only
        }
        bool queueDrain = false;
        {
            std::scoped_lock l(g_lock);
            g_pending.push_back({ RE::NiPointer<RE::NiAVObject>(a_node), a_culled });
            if (!g_drainQueued) {
                g_drainQueued = queueDrain = true;
            }
        }
        if (queueDrain && !vrshim::bridge::runOnMain(&Drain, nullptr)) {
            // fo4flatvr without bridge v9: no main-thread queue - the direct bit stays (old behaviour)
            std::scoped_lock l(g_lock);
            g_direct += g_pending.size();
            g_pending.clear();
            g_drainQueued = false;
            return;
        }
        ++g_queued;
    }

    Stats GetStats()
    {
        return { g_sets.load(), g_queued.load(), g_applied.load(), g_skipped.load(), g_direct.load() };
    }
}
