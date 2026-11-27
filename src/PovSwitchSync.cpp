#include "PovSwitchSync.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include "vrshim/Bridge.h"

// C44 - see PovSwitchSync.h. 1.11.240 RVAs read in Ghidra (VATS agent, 20 Nov).
namespace frik::povswitch
{
    namespace
    {
        constexpr std::uintptr_t kDoShow1st = 0xD78C30;   // PlayerCharacter::DoShow1stPerson(PlayerCharacter*, bool show)
        constexpr std::uintptr_t kSwitchGraph = 0xD79530; // PlayerCharacter::SwitchBehaviorGraph(PlayerCharacter*): active graph ^= 1
        constexpr std::uintptr_t kCallSites[] = {
            0xD6890F,  // PlayerCharacter::Revert
            0xD7E31F,  // PlayerCharacter::PreLoadAnimationGraphManager
            0xC4F142,  // TaskQueueInterface unpack (queued 1st / 3rd person switches: VATS camera, POV, scripts)
        };
        constexpr std::array<std::uint8_t, 10> kShowPro{ 0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
        constexpr std::array<std::uint8_t, 7>  kSwitchPro{ 0x48, 0x8B, 0xC4, 0x55, 0x57, 0x48, 0x8D };

        using Show_t = void (*)(RE::PlayerCharacter*, bool);
        using Switch_t = void (*)(RE::PlayerCharacter*);
        Show_t   g_orig[std::size(kCallSites)]{};
        Switch_t g_switch = nullptr;
        bool     g_installed = false;

        std::atomic<std::uint32_t> g_calls{ 0 }, g_evened{ 0 }, g_heals{ 0 };
        int                        g_mismatchFrames = 0;
        std::chrono::steady_clock::time_point g_mismatchSince{}, g_lastHeal{};

        std::uintptr_t Base() { return REL::Offset(0).address(); }

        bool CallTo(const std::uintptr_t a_site, const std::uintptr_t a_target)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(Base() + a_site);
            if (p[0] != 0xE8) {
                return false;
            }
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            return Base() + a_site + 5 + rel == Base() + a_target;
        }

        bool enabled() { return vrshim::bridge::iniIntLive("bPovSwitchSync", 1) != 0; }

        bool ThirdFlag(const RE::PlayerCharacter* a_pc)
        {
            return ((*(reinterpret_cast<const std::uint8_t*>(a_pc) + 0xDFE)) >> 4 & 1) != 0;  // PlayerCharacter vfunc 0x470
        }

        int CamState()
        {
            const auto cam = f4vr::getPlayerCamera();
            return cam && cam->currentState ? static_cast<int>(cam->currentState->id.get()) : -1;
        }

        RE::BSAnimationGraphManager* Manager(RE::PlayerCharacter* a_pc, RE::BSTSmartPointer<RE::BSAnimationGraphManager>& a_sp)
        {
            return static_cast<RE::IAnimationGraphManagerHolder*>(a_pc)->GetAnimationGraphManagerImpl(a_sp) ? a_sp.get() : nullptr;
        }
        std::uint32_t GraphCount(const RE::BSAnimationGraphManager* m)
        {
            return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(m) + 0x50);
        }
        std::uint32_t ActiveGraph(const RE::BSAnimationGraphManager* m)
        {
            return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(m) + 0xD8);
        }

        template <std::size_t I>
        void HkShow(RE::PlayerCharacter* a_pc, bool a_show)
        {
            const auto n = ++g_calls;
            const bool isPlayer = a_pc && a_pc == RE::PlayerCharacter::GetSingleton();
            bool       oldThird = false, evened = false, fpC = false, tpC = false;
            if (isPlayer) {
                oldThird = ThirdFlag(a_pc);
                auto* fp = a_pc->Get3D(true);
                auto* tp = a_pc->Get3D(false);
                if (fp && tp && fp != tp) {
                    fpC = fp->GetAppCulled();
                    tpC = tp->GetAppCulled();
                    // a real switch (the flag differs from the request) that the cull flags would turn into an early return
                    if (enabled() && oldThird == a_show && fpC != tpC && !(a_show ? fpC : tpC)) {
                        fp->SetAppCulled(tpC);
                        evened = true;
                        ++g_evened;
                    }
                }
            }
            g_orig[I](a_pc, a_show);
            if (isPlayer && (n <= 40 || evened || n % 100 == 0)) {
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> sp;
                const auto* m = Manager(a_pc, sp);
                logger::info("FRIK-flat C44 view switch #{} (call {:X}): show first person {} | third flag {} -> {} | cull first {} third {}{} | "
                             "camera state {} | active graph {}",
                    n, kCallSites[I], a_show ? 1 : 0, oldThird ? 1 : 0, ThirdFlag(a_pc) ? 1 : 0, fpC ? 1 : 0, tpC ? 1 : 0,
                    evened ? " -> evened (the engine would have returned early: no graph switch)" : "", CamState(),
                    m && GraphCount(m) == 2 ? static_cast<int>(ActiveGraph(m)) : -1);
            }
        }

        template <std::size_t... Is>
        void* HookFor(std::size_t a_i, std::index_sequence<Is...>)
        {
            void* r = nullptr;
            ((a_i == Is ? (r = reinterpret_cast<void*>(&HkShow<Is>), 0) : 0), ...);
            return r;
        }
    }

    void install()
    {
        std::string bad;
        if (std::memcmp(reinterpret_cast<const void*>(Base() + kDoShow1st), kShowPro.data(), kShowPro.size()) != 0) {
            bad += " DoShow1stPerson-prologue";
        }
        if (std::memcmp(reinterpret_cast<const void*>(Base() + kSwitchGraph), kSwitchPro.data(), kSwitchPro.size()) != 0) {
            bad += " SwitchBehaviorGraph-prologue";
        }
        for (const auto site : kCallSites) {
            if (!CallTo(site, kDoShow1st)) {
                bad += std::format(" call-{:X}", site);
            }
        }
        if (!bad.empty()) {
            logger::error("FRIK-flat C44 view switch sync: NOT installed (bytes differ:{})", bad);
            return;
        }
        auto& tramp = F4SE::GetTrampoline();
        for (std::size_t i = 0; i < std::size(kCallSites); ++i) {
            g_orig[i] = reinterpret_cast<Show_t>(
                tramp.write_call<5>(Base() + kCallSites[i], HookFor(i, std::make_index_sequence<std::size(kCallSites)>{})));
        }
        g_switch = reinterpret_cast<Switch_t>(Base() + kSwitchGraph);
        g_installed = true;
        logger::info("FRIK-flat C44 view switch sync: installed (DoShow1stPerson calls {:X} {:X} {:X}); [FRIK] bPovSwitchSync={}", kCallSites[0],
            kCallSites[1], kCallSites[2], enabled() ? 1 : 0);
    }

    void tick()
    {
        if (!g_installed || !enabled()) {
            g_mismatchFrames = 0;
            return;
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return;
        }
        const auto* st = static_cast<const RE::ActorState*>(pc);
        const int   cs = CamState();
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> sp;
        const auto* m = Manager(pc, sp);
        // first person by every measure but the graph: the third-person graph active, so nothing poses the first-person skeleton
        const bool mismatch = m && GraphCount(m) == 2 && ActiveGraph(m) == 0 && !ThirdFlag(pc) && (cs == 0 || cs == 4) && pc->Get3D(true) &&
                              st->knockState == 0 && st->lifeState == 0 && !st->inSyncAnim &&
                              static_cast<std::uint32_t>(st->DoGetSitSleepState()) == 0;
        const auto now = std::chrono::steady_clock::now();
        if (!mismatch) {
            g_mismatchFrames = 0;
            return;
        }
        if (g_mismatchFrames++ == 0) {
            g_mismatchSince = now;
        }
        if (g_mismatchFrames < 30 || now - g_mismatchSince < std::chrono::seconds(1) || now - g_lastHeal < std::chrono::seconds(5) || g_heals >= 20) {
            return;
        }
        g_lastHeal = now;
        g_mismatchFrames = 0;
        const auto n = ++g_heals;
        g_switch(pc);  // the engine's own toggle (SetActiveGraph(active ^ 1) + movement / transform-delta sinks)
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> sp2;
        const auto* m2 = Manager(pc, sp2);
        logger::info("FRIK-flat C44 view switch sync: first person (camera state {}) but the third-person graph was active for a second -> "
                     "SwitchBehaviorGraph: active graph now {} (heal #{}{})",
            cs, m2 && GraphCount(m2) == 2 ? static_cast<int>(ActiveGraph(m2)) : -1, n, n >= 20 ? ", last one this session" : "");
    }
}
