#include "BodyGraph.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>
#include <cstring>
#include <format>
#include <mutex>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <Windows.h>

#include "f4vr/BSFlattenedBoneTree.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include "vrshim/Bridge.h"
#include "fo4flatvr/FrikBridgeApi.h"  // IniPath (test flag)

// C43 - see BodyGraph.h. 1.11.240 RVAs read in Ghidra (analysis/first-person-body-mods.md sections 2-4; AE ids in comments).
namespace frik::bodygraph
{
    namespace
    {
        using EventArray = RE::BSScrapArray<RE::BSTTuple<RE::BSFixedString, RE::BSFixedString>>;
        using Graph = std::uint8_t;  // BShkbAnimationGraph, raw

        // ---- engine ------------------------------------------------------------------------------------------------
        constexpr std::uintptr_t kUpdateCall = 0x818CDC;    // IAnimationGraphManagerHolder::UpdateAnimationGraphManager (818C80, 2214536) -> Update
        constexpr std::uintptr_t kMgrUpdate = 0x130E630;    // BSAnimationGraphManager::Update (2256660)
        constexpr std::uintptr_t kEventCall = 0x130EFDB;    // BSAnimationGraphManager::ProcessGraphEvent (130EED0, 2256664) -> ReceiveRequestImpl
        constexpr std::uintptr_t kRecvRequest = 0x13213D0;  // BShkbAnimationGraph::ReceiveRequestImpl (2256862)
        constexpr std::uintptr_t kSwitchCall = 0xD796A9;    // PlayerCharacter::SwitchBehaviorGraph (D79530, 2233239) -> SetActiveGraph
        constexpr std::uintptr_t kSetActive = 0x130F1E0;    // BSAnimationGraphManager::SetActiveGraph (2256665)
        constexpr std::uintptr_t kHolderVtbl = 0x256D358;   // VTABLE PlayerCharacter[5] (IAnimationGraphManagerHolder, id 1276847)
        constexpr int            kPostUpdateSlot = 0x17;    // PostUpdateAnimationGraphManager
        constexpr std::uintptr_t kBoneCopy = 0xD7E380;      // PlayerCharacter::PostUpdateAnimationGraphManager (2233265): 1st -> 3rd person copy
        constexpr std::uintptr_t kActivate = 0x1320820;     // BShkbAnimationGraph::ActivateImpl (2256856)
        constexpr std::uintptr_t kDeactivate = 0x1326760;   // ::DeactivateImpl (2256872)
        constexpr std::uintptr_t kRecvChannels = 0x1321290; // ::ReceiveChannelsImpl (2256861)
        constexpr std::uintptr_t kUpdateImpl = 0x1320920;   // ::UpdateImpl (2256858)
        constexpr std::uintptr_t kGenerate = 0x1321030;     // ::GenerateImpl (2256860)
        constexpr std::uintptr_t kGetFloat = 0x1326CE0;     // ::GetWordVariableImplHelper<float> (2256881)
        constexpr std::uintptr_t kSetFloat = 0x1326980;     // ::SetWordVariableImplHelper<float> (2256876)
        constexpr std::uintptr_t kGetInt = 0x1326DC0;       // ::GetWordVariableImplHelper<int> (2256882)
        constexpr std::uintptr_t kSetInt = 0x1326A60;       // ::SetWordVariableImplHelper<int> (2256877)

        // C43b (20 Nov): graph 0's own Speed. The engine's Speed channel (ActorSpeedChannel<Actor,float>::operator() DF3710)
        // = Actor::GetDesiredSpeed / channel scale, then - when contours are on and the graph is graph-driven / synced -
        // mapped through the *active* graph's speed contour (AnimationSpeedContour::GetGraphSpeedForRequestedSpeedAndDirection),
        // whose "adjustments" (playback-speed variables, state events) are queued to AIProcess and applied to the active graph
        // only (aid_2231781 CFEB00). In first person that is graph 1, so graph 0 got the first-person channel value and never
        // its own contour / adjustments. TFP does the same per-graph contour run for graph 0 (FUN_18003f370 / FUN_180040ef0).
        constexpr std::uintptr_t kGetSlot = 0x131B560;        // BShkbAnimationGraph::GetVariableSlot(BSFixedString&) (2256829) -> word*
        constexpr std::uintptr_t kDesiredSpeed = 0xCA69A0;    // Actor::GetDesiredSpeed (2230410)
        constexpr std::uintptr_t kActorDir = 0xCA69E0;        // Actor::GetDirection
        constexpr std::uintptr_t kFreezeLoco = 0xCA5760;      // Actor::GetFreezeGraphLocomotionChannel (2230387)
        constexpr std::uintptr_t kContourData = 0x131BDF0;    // BShkbAnimationGraph::GetCurrentSpeedContourData (2256835)
        constexpr std::uintptr_t kSpeedInfo = 0x1335480;      // AnimationSpeedInformationSingleton::GetSpeedInformationForSubgraph (2257112)
        constexpr std::uintptr_t kContourForSM = 0x1388870;   // AnimationSpeedInformation::GetContourForStateMachine (2257825)
        constexpr std::uintptr_t kGraphSpeedFor = 0x1388280;  // AnimationSpeedContour::GetGraphSpeedForRequestedSpeedAndDirection (2257819)
        constexpr std::uintptr_t kArenaCtor = 0x165B970;      // BSTObjectArenaScrapAllocBase ctor (2267964)
        constexpr std::uintptr_t kArenaDtor = 0xDF4E10;       // BSTObjectArena<AnimationStateAdjustment,...,16> dtor (2235236)
        constexpr std::uintptr_t kArenaToArray = 0xD28860;    // arena.AppendTo(BSTArray&) (2232160)
        constexpr std::uintptr_t kArrayToArena = 0xD290F0;    // BSTArray.AppendTo(arena&), arena cleared first (2232168)
        constexpr std::uintptr_t kUseContours = 0x2F36620;    // bool read by UseSpeedContoursForMovementCalculations (2664322)
        constexpr std::uintptr_t kContourEvents = 0x2F33F80;  // bool: queued adjustment events are sent (1344811, read in CFEB00)
        constexpr std::uintptr_t kMinContourSpeed = 0x29294BC;  // float 5.0 (the channel's contour / moveStart threshold)

        struct Fn
        {
            std::uintptr_t rva;
            std::array<std::uint8_t, 6> pro;
            const char*    name;
        };
        constexpr Fn kFns[] = {
            { kActivate, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 }, "ActivateImpl" },
            { kDeactivate, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48 }, "DeactivateImpl" },
            { kRecvChannels, { 0x40, 0x55, 0x41, 0x56, 0x48, 0x83 }, "ReceiveChannelsImpl" },
            { kUpdateImpl, { 0x4C, 0x8B, 0xDC, 0x56, 0x41, 0x56 }, "UpdateImpl" },
            { kGenerate, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "GenerateImpl" },
            { kGetFloat, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "GetVariable<float>" },
            { kSetFloat, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55 }, "SetVariable<float>" },
            { kGetInt, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "GetVariable<int>" },
            { kSetInt, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "SetVariable<int>" },
            { kRecvRequest, { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B }, "ReceiveRequestImpl" },
            { kSetActive, { 0x48, 0x8B, 0xC4, 0x53, 0x57, 0x41 }, "SetActiveGraph" },
            { kMgrUpdate, { 0x40, 0x57, 0x41, 0x55, 0x48, 0x83 }, "BSAnimationGraphManager::Update" },
            { kBoneCopy, { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x20 }, "PostUpdateAnimationGraphManager" },
        };
        // C43b speed path (byte-checked separately: if these differ only the speed fix is off, the body graph still runs)
        constexpr Fn kSpeedFns[] = {
            { kGetSlot, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "GetVariableSlot" },
            { kDesiredSpeed, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }, "Actor::GetDesiredSpeed" },
            { kActorDir, { 0x48, 0x8B, 0x89, 0x00, 0x03, 0x00 }, "Actor::GetDirection" },
            { kFreezeLoco, { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B }, "Actor::GetFreezeGraphLocomotionChannel" },
            { kContourData, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }, "GetCurrentSpeedContourData" },
            { kSpeedInfo, { 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 }, "GetSpeedInformationForSubgraph" },
            { kContourForSM, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "GetContourForStateMachine" },
            { kGraphSpeedFor, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "GetGraphSpeedForRequestedSpeedAndDirection" },
            { kArenaCtor, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }, "BSTObjectArenaScrapAllocBase" },
            { kArenaDtor, { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x57 }, "~BSTObjectArena<AnimationStateAdjustment>" },
            { kArenaToArray, { 0x40, 0x57, 0x41, 0x55, 0x41, 0x56 }, "arena.AppendTo(array)" },
            { kArrayToArena, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48 }, "array.AppendTo(arena)" },
        };

        using MgrUpdate_t = void (*)(RE::BSAnimationGraphManager*, const void*);
        using RecvRequest_t = bool (*)(Graph*, const RE::BSFixedString&, EventArray*);
        using SetActive_t = bool (*)(RE::BSAnimationGraphManager*, std::uint32_t);
        using PostUpdate_t = void (*)(void*, const RE::BSTSmartPointer<RE::BSAnimationGraphManager>&);
        using Activate_t = bool (*)(Graph*);
        using Deactivate_t = bool (*)(Graph*, EventArray*);
        using RecvChannels_t = void (*)(Graph*, void*);
        using UpdateImpl_t = void (*)(Graph*, const void*);
        using Generate_t = void (*)(Graph*, EventArray*);
        using GetF_t = bool (*)(Graph*, const RE::BSFixedString&, float&);
        using SetF_t = bool (*)(Graph*, const RE::BSFixedString&, float);
        using GetI_t = bool (*)(Graph*, const RE::BSFixedString&, std::int32_t&);
        using SetI_t = bool (*)(Graph*, const RE::BSFixedString&, std::int32_t);

        MgrUpdate_t    g_origUpdate = nullptr;
        RecvRequest_t  g_origRecv = nullptr;
        SetActive_t    g_origSetActive = nullptr;
        PostUpdate_t   g_origPostUpdate = nullptr;
        Activate_t     g_activate = nullptr;
        Deactivate_t   g_deactivate = nullptr;
        RecvChannels_t g_recvChannels = nullptr;
        UpdateImpl_t   g_updateImpl = nullptr;
        Generate_t     g_generate = nullptr;
        GetF_t         g_getF = nullptr;
        SetF_t         g_setF = nullptr;
        GetI_t         g_getI = nullptr;
        SetI_t         g_setI = nullptr;
        bool           g_installed = false;

        // ---- C43b speed path -----------------------------------------------------------------------------------------
        // AnimationSpeedInformationTypes::AnimationStateAdjustment (engine BSTArray element, 0x10; CFEB00 / D28860)
        struct Adj
        {
            RE::BSFixedString name;        // 00 variable or event name
            std::uint32_t     value;       // 08 float or int bits
            bool              isFloat;     // 0C
            bool              isVariable;  // 0D false = an event
            std::uint8_t      pad[2];
        };
        static_assert(sizeof(Adj) == 0x10);
        using AdjArray = RE::BSTArray<Adj>;  // = BSTArray<AnimationStateAdjustment, BSTArrayHeapAllocator> (data, capacity, size)
        static_assert(sizeof(AdjArray) == 0x18);
        struct Arena  // BSTObjectArena<AnimationStateAdjustment, BSTObjectArenaScrapAlloc, 16>, 0x40 (DF3710 builds it on the stack)
        {
            alignas(8) std::uint8_t b[0x40];
        };

        using GetSlot_t = std::int32_t* (*)(Graph*, const RE::BSFixedString&);
        using ActorF_t = float (*)(RE::Actor*);
        using ActorB_t = bool (*)(RE::Actor*);
        using ContourData_t = bool (*)(Graph*, std::uint64_t*, RE::BSFixedString*);
        using SpeedInfo_t = void* (*)(const std::uint64_t*);
        using ContourForSM_t = bool (*)(void*, const RE::BSFixedString*, void**);
        using GraphSpeedFor_t = std::int32_t (*)(void*, const float*, float, Arena*, float*, Arena*);
        using ArenaFn_t = void (*)(Arena*);
        using ArenaToArray_t = bool (*)(Arena*, AdjArray*);
        using ArrayToArena_t = bool (*)(const AdjArray*, Arena*);

        GetSlot_t       g_getSlot = nullptr;
        ActorF_t        g_desiredSpeed = nullptr;
        ActorF_t        g_actorDir = nullptr;
        ActorB_t        g_freezeLoco = nullptr;
        ContourData_t   g_contourData = nullptr;
        SpeedInfo_t     g_speedInfo = nullptr;
        ContourForSM_t  g_contourForSM = nullptr;
        GraphSpeedFor_t g_graphSpeedFor = nullptr;
        ArenaFn_t       g_arenaCtor = nullptr;
        ArenaFn_t       g_arenaDtor = nullptr;
        ArenaToArray_t  g_arenaToArray = nullptr;
        ArrayToArena_t  g_arrayToArena = nullptr;
        bool            g_speedInstalled = false;
        AdjArray        g_adjLast;  // graph 0's adjustments applied last step (the engine keeps the active graph's at middleHigh+0x508)

        // ---- state (graph update thread: the player's animation update; the FRIK frame reads the counters) -------
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> g_mgr;  // the player's manager graph 0 runs for (ref held)
        std::atomic<RE::BSAnimationGraphManager*>         g_mgrRaw{ nullptr };
        std::atomic<Graph*>                               g_graph0{ nullptr };
        std::atomic<Graph*>                               g_graph1{ nullptr };
        std::atomic<bool>                                 g_ours{ false };         // graph 0 activated by us, still running
        std::atomic<bool>                                 g_suspendSave{ false };  // kPreSaveGame .. kPostSaveGame / pre-load
        std::atomic<RE::BSAnimationGraphManager*>         g_skipCopyFor{ nullptr };
        std::atomic<std::uint64_t>                        g_stepSeq{ 0 };
        std::atomic<bool>                                 g_resetRequest{ false };
        std::atomic<std::uint32_t>                        g_updateTid{ 0 };
        std::atomic<float>                                g_engineDir{ 0.0f };   // graph 1 "Direction" (fraction of a turn, clockwise)
        std::recursive_mutex                              g_mx;  // our state (the hooks can run on any thread)

        bool g_wasDrawn = false, g_wasSneak = false, g_needSync = false;
        int  g_footIdx = -1;
        void* g_footTree = nullptr;
        RE::NiTransform g_prevFoot{};
        int  g_footSame = 0;

        struct Diag
        {
            std::atomic<int> steps{ 0 }, copySkipped{ 0 }, fwd{ 0 }, fwdOk{ 0 }, refused{ 0 }, subs{ 0 }, footChanged{ 0 }, resets{ 0 },
                activations{ 0 }, handbacks{ 0 }, events0{ 0 };
            float spd0{ 0 }, dir0{ 0 }, spd1{ 0 }, dir1{ 0 }, turn0{ 0 };
            std::string refusedNames, pause;
            // C43b (under g_mx): speed path + gait cadence while moving
            int         spdSteps{ 0 }, contourSteps{ 0 }, drivenSteps{ 0 }, frozenSteps{ 0 }, adjApplied{ 0 }, adjEvents{ 0 };
            double      rawSum{ 0 }, graphSum{ 0 };
            std::string adjSeen;
            struct Gait
            {
                double         t;
                RE::NiPoint3   d;    // left foot - right foot, in the pelvis frame (animation locals)
                RE::NiPoint3   pos;  // actor position
                float          raw, graph;
            };
            std::vector<Gait> gait;
        } g_d;
        int g_pelvisIdx = -1, g_rFootIdx = -1;

        // ---- helpers -----------------------------------------------------------------------------------------------
        std::uintptr_t Base() { return REL::Offset(0).address(); }

        bool Prologue(const Fn& f)
        {
            return std::memcmp(reinterpret_cast<const void*>(Base() + f.rva), f.pro.data(), f.pro.size()) == 0;
        }

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

        std::uint32_t GraphCount(const RE::BSAnimationGraphManager* m)
        {
            return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(m) + 0x50);
        }
        std::uint32_t& ActiveGraph(RE::BSAnimationGraphManager* m)
        {
            return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(m) + 0xD8);
        }
        Graph* GraphAt(const RE::BSAnimationGraphManager* m, const std::uint32_t i)
        {
            const auto* b = reinterpret_cast<const std::uint8_t*>(m);
            if (i >= GraphCount(m)) {
                return nullptr;
            }
            // BSTSmallArray: capacity >= 0 -> heap pointer at +0x48, else inline storage at +0x48 (engine code)
            const auto* arr = reinterpret_cast<Graph* const*>(b + 0x48);
            if (*reinterpret_cast<const std::int32_t*>(b + 0x40) >= 0) {
                arr = *reinterpret_cast<Graph* const* const*>(b + 0x48);
            }
            return arr ? arr[i] : nullptr;
        }
        bool GraphActive(const Graph* g)
        {
            const auto* bg = *reinterpret_cast<const std::uint8_t* const*>(g + 0x378);  // hkbBehaviorGraph
            return bg && bg[0x1AA] != 0;
        }
        void* Channels(RE::BSAnimationGraphManager* m) { return reinterpret_cast<std::uint8_t*>(m) + 0x10; }  // boundChannel

        const RE::BSFixedString& Str(const char* a_s)
        {
            // fixed set of names; created on first use (string pool is up in game)
            static std::mutex                                                  s_mx;
            static std::vector<std::pair<const char*, RE::BSFixedString*>> s_map;
            std::lock_guard l(s_mx);
            for (auto& [k, v] : s_map) {
                if (k == a_s) {
                    return *v;
                }
            }
            s_map.emplace_back(a_s, new RE::BSFixedString(a_s));
            return *s_map.back().second;
        }

        bool Send(Graph* g, const char* a_ev)
        {
            EventArray out;
            const bool ok = g_origRecv(g, Str(a_ev), &out);
            g_d.events0 += static_cast<int>(out.size());
            return ok;  // out: graph 0's own output events, discarded (no dispatch)
        }
        bool SendEither(Graph* g, const char* a, const char* b) { return Send(g, a) || (b && Send(g, b)); }

        // ---- C43b: graph 0's Speed, the engine's Speed-channel logic run against graph 0 --------------------------------
        bool speedEnabled() noexcept { return vrshim::bridge::iniIntLive("bBodyGraphSpeedContour", 1) != 0; }

        bool Word(Graph* g, const char* a_var)  // bool / int graph variable (0 when the graph has no such variable)
        {
            const std::int32_t* w = g_getSlot(g, Str(a_var));
            return w && *w != 0;
        }

        float SpeedChannelScale(RE::BSAnimationGraphManager* m)  // ActorSpeedChannel scale (channel +0x24, 1.0 by default)
        {
            const auto* arr = reinterpret_cast<const std::uint8_t*>(Channels(m));
            auto* const* data = *reinterpret_cast<std::uint8_t* const* const*>(arr);
            const auto   n = *reinterpret_cast<const std::uint32_t*>(arr + 0x10);
            const auto&  speed = Str("Speed");
            for (std::uint32_t i = 0; data && i < n && i < 64; ++i) {
                const std::uint8_t* ch = data[i];
                if (ch && *reinterpret_cast<const RE::BSFixedString*>(ch + 0x10) == speed) {
                    const float s = *reinterpret_cast<const float*>(ch + 0x24);
                    return s > 0.0f ? s : 1.0f;
                }
            }
            return 1.0f;
        }

        void ArenaInit(Arena& a)  // as DF3710: allocator from the thread's scrap heap, empty block list
        {
            std::memset(&a, 0, sizeof(a));
            g_arenaCtor(&a);
            *reinterpret_cast<void**>(a.b + 0x10) = a.b + 0x8;
        }

        void ReleaseContour(void*& a_c)  // BSTSmartPointer<AnimationSpeedContour> release (refcount +8, vfunc 0 = deleting dtor)
        {
            if (!a_c) {
                return;
            }
            auto* rc = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(a_c) + 8);
            if (_InterlockedDecrement(rc) == 0) {
                using Del_t = void (*)(void*, std::uint32_t);
                (*reinterpret_cast<Del_t* const*>(a_c))[0](a_c, 1);
            }
            a_c = nullptr;
        }

        // The engine's queued-adjustment apply (CFEB00) for graph 0: a variable / event is applied when it differs from the
        // previous step's set; events only while the engine's own switch (1344811) is on. Graph 0's answers are discarded.
        void ApplyAdjustments(Graph* g0, AdjArray& a_now)
        {
            const bool events = *reinterpret_cast<const bool*>(Base() + kContourEvents);
            for (auto& a : a_now) {
                bool same = false;
                for (const auto& p : g_adjLast) {
                    if (p.name == a.name && p.isVariable == a.isVariable && p.isFloat == a.isFloat && p.value == a.value) {
                        same = true;
                        break;
                    }
                }
                if (same || a.name.empty()) {
                    continue;
                }
                if (!a.isVariable) {
                    if (events) {
                        EventArray out;
                        g_origRecv(g0, a.name, &out);
                        g_d.events0 += static_cast<int>(out.size());
                        ++g_d.adjEvents;
                    }
                } else if (a.isFloat) {
                    float f;
                    std::memcpy(&f, &a.value, 4);
                    g_setF(g0, a.name, f);
                } else {
                    g_setI(g0, a.name, static_cast<std::int32_t>(a.value));
                }
                ++g_d.adjApplied;
                if (g_d.adjSeen.size() < 240 && g_d.adjSeen.find(a.name.c_str()) == std::string::npos) {
                    float f;
                    std::memcpy(&f, &a.value, 4);
                    g_d.adjSeen += !a.isVariable ? std::format(" ev:{}", a.name.c_str()) :
                                   a.isFloat     ? std::format(" {}={:.3f}", a.name.c_str(), f) :
                                                   std::format(" {}={}", a.name.c_str(), static_cast<std::int32_t>(a.value));
                }
            }
            g_adjLast = std::move(a_now);
        }

        // ActorSpeedChannel<Actor,float>::operator() (DF3710) with graph 0 in place of the active graph. Runs after
        // ReceiveChannelsImpl (which wrote graph 1's channel value), before UpdateImpl. Caller holds g_mx + the manager lock.
        void FeedSpeed(RE::PlayerCharacter* pl, RE::BSAnimationGraphManager* m, Graph* g0, float& a_raw, float& a_graph)
        {
            a_raw = a_graph = -1.0f;
            if (!g_speedInstalled || !speedEnabled()) {
                return;  // off: graph 0 keeps graph 1's channel value (C43 as first shipped)
            }
            if (Word(g0, "bFreezeSpeedUpdate") || g_freezeLoco(pl)) {
                ++g_d.frozenSteps;
                return;  // the engine keeps the previous value: so does graph 0
            }
            const float raw = g_desiredSpeed(pl) / SpeedChannelScale(m);
            float       speed = raw;
            AdjArray    now;
            const bool  driven = Word(g0, "bGraphDriven") || Word(g0, "bIsSynced");
            if (driven) {
                ++g_d.drivenSteps;
            }
            if (driven && *reinterpret_cast<const bool*>(Base() + kUseContours) && raw >= *reinterpret_cast<const float*>(Base() + kMinContourSpeed)) {
                std::uint64_t     sub = 0;
                RE::BSFixedString sm;
                if (g_contourData(g0, &sub, &sm)) {
                    void* contour = nullptr;
                    if (void* info = g_speedInfo(&sub); info && g_contourForSM(info, &sm, &contour) && contour) {
                        Arena in, out;
                        ArenaInit(in);
                        ArenaInit(out);
                        g_arrayToArena(&g_adjLast, &in);  // AIProcess::GetLastQueuedAnimationGraphAdjustments equivalent
                        float graphSpeed = 0.0f;
                        if (g_graphSpeedFor(contour, &raw, g_actorDir(pl), &in, &graphSpeed, &out) != 0) {
                            speed = graphSpeed;
                            g_arenaToArray(&out, &now);
                            ++g_d.contourSteps;
                        }
                        g_arenaDtor(&out);
                        g_arenaDtor(&in);
                    }
                    ReleaseContour(contour);
                }
            }
            g_setF(g0, Str("Speed"), speed);
            ApplyAdjustments(g0, now);  // empty when no contour ran: the engine's "last" list then empties too
            ++g_d.spdSteps;
            g_d.rawSum += raw;
            g_d.graphSum += speed;
            a_raw = raw;
            a_graph = speed;
        }

        RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

        bool IsPlayerMgr(RE::BSAnimationGraphManager* m)
        {
            if (GraphCount(m) != 2) {
                return false;
            }
            auto* pl = Player();
            if (!pl) {
                return false;
            }
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> sp;
            return static_cast<RE::IAnimationGraphManagerHolder*>(pl)->GetAnimationGraphManagerImpl(sp) && sp.get() == m;
        }

        // why graph 0 must not run now (empty = may run)
        const char* PauseReason(RE::PlayerCharacter* pl)
        {
            if (!enabled()) {
                return "off ([FRIK] bBodyGraph=0)";
            }
            if (g_suspendSave) {
                return "save / load";
            }
            if (!vrshim::bridge::bodyEnabled()) {
                return "FRIK body off ([FRIK] bBody=0)";
            }
            {  // FRIK's body only in first person (0) / iron sights (4) (C23); elsewhere the engine's own presentation stays
                const auto cam = f4vr::getPlayerCamera();
                const int  cs = cam && cam->currentState ? static_cast<int>(cam->currentState->id.get()) : -1;
                if (cs != 0 && cs != 4) {
                    return "camera not first person";
                }
            }
            const auto* st = static_cast<const RE::ActorState*>(pl);
            if (st->knockState != 0) {
                return "knocked down";
            }
            if (st->lifeState != 0) {
                return "not alive (dying / dead / bleedout)";
            }
            if (st->inSyncAnim) {
                return "paired animation";
            }
            if (static_cast<std::uint32_t>(st->DoGetSitSleepState()) != 0) {
                return "seated / furniture / power armour enter-exit";
            }
            return "";
        }

        void HandBack(RE::BSAnimationGraphManager* m, const char* a_why)
        {
            // caller holds g_mx; deactivate graph 0 under the manager's lock, its events discarded
            if (!g_ours) {
                return;
            }
            g_ours = false;
            if (m && GraphCount(m) == 2) {
                if (Graph* g0 = GraphAt(m, 0); g0 && ActiveGraph(m) != 0) {
                    auto& lk = *reinterpret_cast<RE::BSSpinLock*>(reinterpret_cast<std::uint8_t*>(m) + 0xC8);
                    lk.lock();
                    EventArray out;
                    g_deactivate(g0, &out);
                    lk.unlock();
                }
            }
            ++g_d.handbacks;
            logger::info("FRIK-flat C43 body graph: graph 0 handed back to the engine ({})", a_why);
        }

        void LogBoneMapOnce(RE::PlayerCharacter* pl)
        {
            static bool s_done = false;
            if (s_done) {
                return;
            }
            s_done = true;
            const auto* b = reinterpret_cast<const std::uint8_t*>(pl);
            const auto* map = *reinterpret_cast<const std::int32_t* const*>(b + 0xDC8);
            const auto  cnt = *reinterpret_cast<const std::uint32_t*>(b + 0xDD8);
            auto*       tree = f4vr::getFlattenedBoneTree();
            std::string names;
            for (std::uint32_t i = 0; map && tree && tree->transforms && i < cnt && i < 64; ++i) {
                const int t = map[i * 2 + 1];
                names += std::format(" {}", t >= 0 && t < tree->numTransforms && tree->transforms[t].name.c_str() ? tree->transforms[t].name.c_str() : "?");
            }
            logger::info("FRIK-flat C43 body graph: the engine's first-person bone copy (D7E380) maps {} bones into the third-person tree:{}", cnt, names);
        }

        // C43b: a foot's origin in the pelvis frame, composed from the animation locals up the parent chain
        bool FootInPelvis(const f4vr::BSFlattenedBoneTree* tree, const int a_foot, RE::NiPoint3& a_out)
        {
            if (a_foot < 0 || g_pelvisIdx < 0) {
                return false;
            }
            auto local = [&](const int i) -> const RE::NiTransform& {
                const auto& b = tree->transforms[i];
                return b.refNode ? b.refNode->local : b.local;
            };
            RE::NiTransform t = local(a_foot);
            int             i = tree->transforms[a_foot].parPos;
            for (int n = 0; n < 8 && i >= 0 && i < tree->numTransforms; ++n) {
                if (i == g_pelvisIdx) {
                    a_out = t.translate;
                    return true;
                }
                t = local(i) * t;
                i = tree->transforms[i].parPos;
            }
            return false;
        }

        // after Generate: is the third-person left foot posed differently from the last step? (stuck detection)
        // C43b: also samples the gait (left - right foot in the pelvis frame) while moving, for the cadence diag line
        void TrackFoot(RE::PlayerCharacter* a_pl, const bool a_moving, const float a_raw, const float a_graph)
        {
            auto* tree = f4vr::getFlattenedBoneTree();
            if (!tree || !tree->transforms || tree->numTransforms <= 0) {
                return;
            }
            if (g_footTree != tree) {
                g_footTree = tree;
                g_footIdx = -1;
                g_rFootIdx = -1;
                g_pelvisIdx = -1;
                for (int i = 0; i < tree->numTransforms; ++i) {
                    const char* n = tree->transforms[i].name.c_str();
                    if (!n) {
                        continue;
                    }
                    if (_stricmp(n, "LLeg_Foot") == 0) {
                        g_footIdx = i;
                    } else if (_stricmp(n, "RLeg_Foot") == 0) {
                        g_rFootIdx = i;
                    } else if (_stricmp(n, "Pelvis") == 0) {
                        g_pelvisIdx = i;
                    }
                }
            }
            if (g_footIdx < 0) {
                return;
            }
            if (RE::NiPoint3 l, r; a_moving && FootInPelvis(tree, g_footIdx, l) && FootInPelvis(tree, g_rFootIdx, r) && g_d.gait.size() < 2000) {
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                g_d.gait.push_back({ t, l - r, a_pl->data.location, a_raw, a_graph });
            }
            const auto& b = tree->transforms[g_footIdx];
            const RE::NiTransform& l = b.refNode ? b.refNode->local : b.local;
            const bool same = std::memcmp(&l.rotate, &g_prevFoot.rotate, sizeof(l.rotate)) == 0 && std::memcmp(&l.translate, &g_prevFoot.translate, sizeof(l.translate)) == 0;
            g_prevFoot = l;
            if (same) {
                ++g_footSame;
            } else {
                g_footSame = 0;
                ++g_d.footChanged;
            }
        }

        // ---- the body-graph step (after the engine's update of the player's manager) --------------------------------
        void Step(RE::BSAnimationGraphManager* m, const void* a_updateData)
        {
            auto* pl = Player();
            if (!pl) {
                return;
            }
            std::lock_guard l(g_mx);
            if (g_mgrRaw.load() != m) {  // load / 3D reload: a new manager
                if (g_ours) {
                    logger::info("FRIK-flat C43 body graph: the player's animation graph manager changed ({} -> {})", static_cast<void*>(g_mgrRaw.load()),
                        static_cast<void*>(m));
                }
                g_ours = false;
                g_mgr.reset(m);
                g_mgrRaw = m;
                g_footTree = nullptr;
                g_adjLast.clear();
            }
            Graph* g0 = GraphAt(m, 0);
            Graph* g1 = GraphAt(m, 1);
            g_graph0 = g0;
            g_graph1 = g1;
            if (!g0 || !g1 || ActiveGraph(m) != 1) {
                if (g_ours && ActiveGraph(m) == 0) {
                    g_ours = false;  // the engine took graph 0 itself (third person) - it is its own now
                    logger::info("FRIK-flat C43 body graph: the engine runs graph 0 itself (active graph 0)");
                }
                return;
            }
            const char* why = PauseReason(pl);
            if (*why) {
                g_d.pause = why;
                if (g_ours) {
                    HandBack(m, why);
                }
                return;
            }
            g_d.pause.clear();
            LogBoneMapOnce(pl);

            auto& lk = *reinterpret_cast<RE::BSSpinLock*>(reinterpret_cast<std::uint8_t*>(m) + 0xC8);
            lk.lock();
            if (!g_ours || !GraphActive(g0)) {
                if (f4vr::isJumpingOrInAir()) {  // TFP: activation held while in the air (the graph would start in the wrong state)
                    lk.unlock();
                    return;
                }
                g_activate(g0);
                g_ours = true;
                g_needSync = true;
                ++g_d.activations;
                g_footSame = 0;
                g_adjLast.clear();  // C43b: graph 0 starts fresh, so does its adjustment history
                logger::info("FRIK-flat C43 body graph: third-person graph activated for the first-person body (graph 0 active {}, manager {})",
                    GraphActive(g0), static_cast<void*>(m));
            }
            g_updateTid = GetCurrentThreadId();

            // inputs: the manager's bound channels (Speed / Direction / TurnDelta / Pitch / ...), as Update does for graph 1
            g_recvChannels(g0, Channels(m));
            float raw = -1.0f, graphSpeed = -1.0f;
            FeedSpeed(pl, m, g0, raw, graphSpeed);  // C43b: graph 0's own Speed (+ its contour's playback adjustments)
            if (graphSpeed < 0.0f) {  // off / frozen: the gait diag shows the Speed graph 0 runs with (graph 1's channel value)
                float v = 0.0f;
                g_getF(g0, Str("Speed"), v);
                raw = graphSpeed = v;
            }
            g_updateImpl(g0, a_updateData);

            // state sync: activation, draw / holster, sneak (the first-person events are mirrored too; these make sure)
            const bool drawn = f4vr::isWeaponDrawn();
            const bool sneak = f4vr::isPlayerSneaking();
            const auto moveMode = static_cast<const RE::ActorState*>(pl)->moveMode;
            const bool moving = (moveMode & 0xF) != 0;
            if (g_needSync || g_resetRequest.exchange(false)) {
                const bool wasReset = !g_needSync;
                Send(g0, "g_archetypeBaseStateStartInstant");
                const bool eq = drawn ? SendEither(g0, "g_weapForceEquipInstant", "WeapEquip") : false;
                const bool sn = sneak ? Send(g0, "sneakStart") : false;
                const bool mv = moving ? SendEither(g0, "moveStart", "moveStartAnimated") : false;
                logger::info("FRIK-flat C43 body graph: {} - drawn {} (equip taken {}), sneaking {} (taken {}), moving {} (move start taken {})",
                    wasReset ? "stuck pose: base state reset" : "graph 0 initialised", drawn, eq, sneak, sn, moving, mv);
                if (wasReset) {
                    ++g_d.resets;
                }
                g_needSync = false;
            } else {
                if (drawn != g_wasDrawn) {
                    if (drawn) {
                        SendEither(g0, "g_weapForceEquipInstant", "WeapEquip");
                    } else {
                        Send(g0, "g_archetypeBaseStateStart");
                        if (moving) {
                            SendEither(g0, "moveStart", "moveStartAnimated");
                        }
                    }
                }
                if (sneak != g_wasSneak) {
                    Send(g0, sneak ? "sneakStart" : "sneakStop");
                }
            }
            g_wasDrawn = drawn;
            g_wasSneak = sneak;

            EventArray out;  // graph 0's output events (footsteps, hits, sounds): collected and discarded, never dispatched
            g_generate(g0, &out);
            g_d.events0 += static_cast<int>(out.size());
            lk.unlock();

            // diag values (graph 0 vs graph 1)
            g_getF(g0, Str("Speed"), g_d.spd0);
            g_getF(g0, Str("Direction"), g_d.dir0);
            g_getF(g0, Str("TurnDelta"), g_d.turn0);
            g_getF(g1, Str("Speed"), g_d.spd1);
            g_getF(g1, Str("Direction"), g_d.dir1);
            g_engineDir = g_d.dir1;

            TrackFoot(pl, moving, raw, graphSpeed);
            if (moving && g_footSame >= 45) {  // ~0.75 s moving with the foot never re-posed: stuck (TFP / FBFP both recover this)
                g_footSame = 0;
                g_resetRequest = true;
            }
            g_skipCopyFor = m;
            ++g_d.steps;
            ++g_stepSeq;
        }

        // ---- hooks -----------------------------------------------------------------------------------------------
        void HkMgrUpdate(RE::BSAnimationGraphManager* a_mgr, const void* a_data)
        {
            g_origUpdate(a_mgr, a_data);
            if (a_mgr && a_data && IsPlayerMgr(a_mgr)) {
                Step(a_mgr, a_data);
            }
        }

        bool HkRecvRequest(Graph* a_graph, const RE::BSFixedString& a_ev, EventArray* a_out)
        {
            const bool r = g_origRecv(a_graph, a_ev, a_out);
            if (a_graph && a_graph == g_graph1.load() && g_ours) {
                Graph* g0 = g_graph0.load();
                if (g0 && g0 != a_graph && GraphActive(g0)) {
                    ++g_d.fwd;
                    EventArray out;  // discarded: graph 0's answers are never broadcast
                    bool ok = g_origRecv(g0, a_ev, &out);
                    if (!ok && a_ev.c_str()) {
                        const char* e = a_ev.c_str();
                        const char* sub = nullptr;
                        if (_stricmp(e, "MoveStart") == 0) {
                            sub = "moveStartAnimated";
                        } else if (_stricmp(e, "MTSwimStop") == 0) {
                            sub = "SwimStop";
                        } else if (_strnicmp(e, "jump", 4) == 0 && std::strstr(e, "and")) {  // jumpLand / JumpLandEnd / ... (FBFP)
                            sub = "jumpLand";
                        }
                        if (sub) {
                            ok = g_origRecv(g0, Str(sub), &out);
                            ++g_d.subs;
                        }
                        if (!ok) {
                            ++g_d.refused;
                            std::lock_guard l(g_mx);
                            if (g_d.refusedNames.size() < 300 && g_d.refusedNames.find(e) == std::string::npos) {
                                g_d.refusedNames += std::format(" {}", e);
                            }
                        }
                    }
                    if (ok) {
                        ++g_d.fwdOk;
                    }
                    g_d.events0 += static_cast<int>(out.size());
                }
            }
            return r;
        }

        bool HkSwitchSetActive(RE::BSAnimationGraphManager* a_mgr, const std::uint32_t a_idx)
        {
            if (a_mgr && a_idx == 0 && g_ours && a_mgr == g_mgrRaw.load()) {
                std::lock_guard l(g_mx);
                HandBack(a_mgr, "the engine switches to the third-person graph (SwitchBehaviorGraph)");  // it then starts it fresh
            }
            return g_origSetActive(a_mgr, a_idx);
        }

        void HkPostUpdate(void* a_holder, const RE::BSTSmartPointer<RE::BSAnimationGraphManager>& a_mgr)
        {
            auto* want = g_skipCopyFor.exchange(nullptr);
            if (want && a_mgr.get() == want && g_ours) {
                ++g_d.copySkipped;  // graph 0 posed the third-person bones: no first-person bone copy over them
                return;
            }
            g_origPostUpdate(a_holder, a_mgr);
        }

        void OnMessage(F4SE::MessagingInterface::Message* a_msg)
        {
            if (!a_msg) {
                return;
            }
            if (a_msg->type == F4SE::MessagingInterface::kPreSaveGame || a_msg->type == F4SE::MessagingInterface::kPostSaveGame ||
                a_msg->type == F4SE::MessagingInterface::kPreLoadGame) {
                logger::info("FRIK-flat C43 body graph: F4SE message {} ({}), graph 0 ours {}", a_msg->type,
                    a_msg->type == F4SE::MessagingInterface::kPreSaveGame ? "pre-save" : (a_msg->type == F4SE::MessagingInterface::kPostSaveGame ? "post-save" : "pre-load"),
                    g_ours.load());
            }
            switch (a_msg->type) {
            case F4SE::MessagingInterface::kPreSaveGame:
            case F4SE::MessagingInterface::kPreLoadGame:
                {
                    g_suspendSave = true;
                    std::lock_guard l(g_mx);
                    if (g_ours) {
                        HandBack(g_mgrRaw.load(), a_msg->type == F4SE::MessagingInterface::kPreSaveGame ? "saving" : "loading");
                    }
                    if (a_msg->type == F4SE::MessagingInterface::kPreLoadGame) {
                        g_mgr.reset();
                        g_mgrRaw = nullptr;
                        g_graph0 = nullptr;
                        g_graph1 = nullptr;
                    }
                }
                break;
            case F4SE::MessagingInterface::kPostSaveGame:
            case F4SE::MessagingInterface::kPostLoadGame:
            case F4SE::MessagingInterface::kNewGame:
                g_suspendSave = false;
                break;
            default:
                break;
            }
        }

        std::uint64_t g_seenSeq = 0;
        bool          g_posing = false;
    }

    bool enabled() noexcept
    {
        // test only: flag file fo4flatvr_frik_bodygraph_off.test next to fo4flatvr.ini = off (legs_session.py live off-switch check;
        // the panel row needs a fo4flatvr build), checked once a second
        static std::atomic<bool>      s_flagOff{ false };
        static std::atomic<long long> s_flagT{ 0 };
        const long long               now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000;  // ms
        if (now - s_flagT.load() > 1000) {
            s_flagT = now;
            bool off = false;
            if (const auto* api = vrshim::bridge::api(); api && api->IniPath) {
                std::string flag = api->IniPath();
                if (const auto slash = flag.find_last_of("\\/"); slash != std::string::npos) {
                    flag.resize(slash + 1);
                }
                std::error_code ec;
                off = std::filesystem::exists(flag + "fo4flatvr_frik_bodygraph_off.test", ec);
            }
            if (off != s_flagOff.exchange(off)) {
                logger::info("FRIK-flat C43 body graph: test flag fo4flatvr_frik_bodygraph_off.test {}", off ? "present: OFF" : "gone");
            }
        }
        return vrshim::bridge::iniIntLive("bBodyGraph", 1) != 0 && !s_flagOff.load();
    }

    bool engineMoveDirection(float& a_turns) noexcept
    {
        if (!g_installed || !g_ours) {
            return false;
        }
        a_turns = g_engineDir.load();
        return true;
    }

    bool posingLegs() noexcept
    {
        return g_installed && g_posing;
    }

    void install()
    {
        std::string bad;
        for (const auto& f : kFns) {
            if (!Prologue(f)) {
                bad += std::format(" {}@{:X}", f.name, f.rva);
            }
        }
        const auto  base = Base();
        auto* const slot = reinterpret_cast<std::uintptr_t*>(base + kHolderVtbl + kPostUpdateSlot * 8);
        if (*slot != base + kBoneCopy) {
            bad += std::format(" holder-vtable-slot-0x17({:X})", *slot - base);
        }
        if (!CallTo(kUpdateCall, kMgrUpdate)) {
            bad += " UpdateCall";
        }
        if (!CallTo(kEventCall, kRecvRequest)) {
            bad += " EventCall";
        }
        if (!CallTo(kSwitchCall, kSetActive)) {
            bad += " SwitchCall";
        }
        if (!bad.empty()) {
            logger::error("FRIK-flat C43 body graph: NOT installed (bytes differ:{}) - legs use FRIK's walk", bad);
            return;
        }
        g_activate = reinterpret_cast<Activate_t>(base + kActivate);
        g_deactivate = reinterpret_cast<Deactivate_t>(base + kDeactivate);
        g_recvChannels = reinterpret_cast<RecvChannels_t>(base + kRecvChannels);
        g_updateImpl = reinterpret_cast<UpdateImpl_t>(base + kUpdateImpl);
        g_generate = reinterpret_cast<Generate_t>(base + kGenerate);
        g_getF = reinterpret_cast<GetF_t>(base + kGetFloat);
        g_setF = reinterpret_cast<SetF_t>(base + kSetFloat);
        g_getI = reinterpret_cast<GetI_t>(base + kGetInt);
        g_setI = reinterpret_cast<SetI_t>(base + kSetInt);

        {  // C43b speed path: own byte check; on a mismatch only graph 0's own Speed is off (it then keeps graph 1's channel value)
            std::string badSpeed;
            for (const auto& f : kSpeedFns) {
                if (!Prologue(f)) {
                    badSpeed += std::format(" {}@{:X}", f.name, f.rva);
                }
            }
            if (badSpeed.empty()) {
                g_getSlot = reinterpret_cast<GetSlot_t>(base + kGetSlot);
                g_desiredSpeed = reinterpret_cast<ActorF_t>(base + kDesiredSpeed);
                g_actorDir = reinterpret_cast<ActorF_t>(base + kActorDir);
                g_freezeLoco = reinterpret_cast<ActorB_t>(base + kFreezeLoco);
                g_contourData = reinterpret_cast<ContourData_t>(base + kContourData);
                g_speedInfo = reinterpret_cast<SpeedInfo_t>(base + kSpeedInfo);
                g_contourForSM = reinterpret_cast<ContourForSM_t>(base + kContourForSM);
                g_graphSpeedFor = reinterpret_cast<GraphSpeedFor_t>(base + kGraphSpeedFor);
                g_arenaCtor = reinterpret_cast<ArenaFn_t>(base + kArenaCtor);
                g_arenaDtor = reinterpret_cast<ArenaFn_t>(base + kArenaDtor);
                g_arenaToArray = reinterpret_cast<ArenaToArray_t>(base + kArenaToArray);
                g_arrayToArena = reinterpret_cast<ArrayToArena_t>(base + kArrayToArena);
                g_speedInstalled = true;
            }
            logger::info("FRIK-flat C43b body graph speed: {} (graph 0's own Speed from Actor::GetDesiredSpeed + its own speed contour and "
                         "playback adjustments); [FRIK] bBodyGraphSpeedContour={}; engine contours {} (min speed {:.1f}), contour events {}",
                badSpeed.empty() ? "installed" : std::format("NOT installed (bytes differ:{}) - graph 0 keeps graph 1's channel Speed", badSpeed),
                speedEnabled() ? 1 : 0, *reinterpret_cast<const bool*>(base + kUseContours), *reinterpret_cast<const float*>(base + kMinContourSpeed),
                *reinterpret_cast<const bool*>(base + kContourEvents));
        }

        auto& tramp = F4SE::GetTrampoline();
        g_origRecv = reinterpret_cast<RecvRequest_t>(tramp.write_call<5>(base + kEventCall, &HkRecvRequest));
        g_origSetActive = reinterpret_cast<SetActive_t>(tramp.write_call<5>(base + kSwitchCall, &HkSwitchSetActive));
        g_origPostUpdate = reinterpret_cast<PostUpdate_t>(*slot);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(slot), reinterpret_cast<std::uintptr_t>(&HkPostUpdate));
        g_origUpdate = reinterpret_cast<MgrUpdate_t>(tramp.write_call<5>(base + kUpdateCall, &HkMgrUpdate));  // last: the step needs the others
        bool listening = false;
        if (const auto* msg = F4SE::GetMessagingInterface()) {
            listening = msg->RegisterListener(OnMessage);
        }
        logger::info("FRIK-flat C43 body graph: F4SE message listener (pre-save / pre-load hand-back) {}", listening ? "registered" : "NOT registered");
        g_installed = true;
        logger::info("FRIK-flat C43 body graph: installed (update call {:X}, event call {:X}, graph switch call {:X}, bone copy slot {:X}); [FRIK] bBodyGraph={}",
            kUpdateCall, kEventCall, kSwitchCall, kHolderVtbl + kPostUpdateSlot * 8, enabled() ? 1 : 0);
    }

    void frameTick()
    {
        if (!g_installed) {
            return;
        }
        const auto seq = g_stepSeq.load();
        g_posing = seq != g_seenSeq && g_ours;
        g_seenSeq = seq;

        static auto s_t = std::chrono::steady_clock::now();
        const auto  now = std::chrono::steady_clock::now();
        if (now - s_t < std::chrono::seconds(2)) {
            return;
        }
        s_t = now;
        std::string refused, pause;
        {
            std::lock_guard l(g_mx);
            refused.swap(g_d.refusedNames);
            pause = g_d.pause;
        }
        const int steps = g_d.steps.exchange(0);
        logger::info(
            "FRIK-flat C43 body graph: {} | steps {} ours {} graph0 active {} | bone copy skipped {} | events mirrored {} ok {} subst {} refused {}{} | graph0 "
            "out events {} (discarded) | foot re-posed {}/{} | activations {} hand-backs {} resets {} | graph0 Speed {:.1f} Direction {:.3f} TurnDelta {:.2f} | "
            "graph1 Speed {:.1f} Direction {:.3f} | update thread {} (FRIK frame thread {})",
            enabled() ? (pause.empty() ? "on" : std::format("paused: {}", pause)) : "OFF", steps, g_ours.load(), g_graph0.load() ? GraphActive(g_graph0.load()) : false,
            g_d.copySkipped.exchange(0), g_d.fwd.exchange(0), g_d.fwdOk.exchange(0), g_d.subs.exchange(0), g_d.refused.exchange(0),
            refused.empty() ? "" : std::format(" ({})", refused), g_d.events0.exchange(0), g_d.footChanged.exchange(0), steps, g_d.activations.exchange(0),
            g_d.handbacks.exchange(0), g_d.resets.exchange(0), g_d.spd0, g_d.dir0, g_d.turn0, g_d.spd1, g_d.dir1, g_updateTid.load(), GetCurrentThreadId());

        // C43b: graph 0's own Speed + the gait cadence (foot cycle period vs ground speed) over the window
        std::vector<Diag::Gait> gait;
        std::string             adjSeen;
        int                     spdSteps, contourSteps, drivenSteps, frozenSteps, adjApplied, adjEvents;
        double                  rawSum, graphSum;
        {
            std::lock_guard l(g_mx);
            gait.swap(g_d.gait);
            adjSeen.swap(g_d.adjSeen);
            spdSteps = std::exchange(g_d.spdSteps, 0);
            contourSteps = std::exchange(g_d.contourSteps, 0);
            drivenSteps = std::exchange(g_d.drivenSteps, 0);
            frozenSteps = std::exchange(g_d.frozenSteps, 0);
            adjApplied = std::exchange(g_d.adjApplied, 0);
            adjEvents = std::exchange(g_d.adjEvents, 0);
            rawSum = std::exchange(g_d.rawSum, 0.0);
            graphSum = std::exchange(g_d.graphSum, 0.0);
        }
        std::string cadence = "not moving";
        if (gait.size() >= 20) {
            // the axis (pelvis frame) along which left - right foot swings most; a full cycle = two crossings of its midline
            float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
            for (const auto& s : gait) {
                const float v[3] = { s.d.x, s.d.y, s.d.z };
                for (int k = 0; k < 3; ++k) {
                    lo[k] = (std::min)(lo[k], v[k]);
                    hi[k] = (std::max)(hi[k], v[k]);
                }
            }
            int ax = 0;
            for (int k = 1; k < 3; ++k) {
                if (hi[k] - lo[k] > hi[ax] - lo[ax]) {
                    ax = k;
                }
            }
            const float  mid = (lo[ax] + hi[ax]) * 0.5f, hyst = (hi[ax] - lo[ax]) * 0.15f;
            int          side = 0, ups = 0;
            double       tFirst = 0, tLast = 0;
            for (const auto& s : gait) {
                const float v = ax == 0 ? s.d.x : (ax == 1 ? s.d.y : s.d.z);
                if (v > mid + hyst) {
                    if (side < 0) {  // an upward crossing: one per gait cycle
                        (ups++ == 0 ? tFirst : tLast) = s.t;
                    }
                    side = 1;
                } else if (v < mid - hyst) {
                    side = -1;
                }
            }
            const double dt = gait.back().t - gait.front().t;
            const float  dx = gait.back().pos.x - gait.front().pos.x, dy = gait.back().pos.y - gait.front().pos.y;
            const double ground = dt > 0 ? std::sqrt(dx * dx + dy * dy) / dt : 0.0;
            double       rawAvg = 0, graphAvg = 0;
            for (const auto& s : gait) {
                rawAvg += s.raw;
                graphAvg += s.graph;
            }
            rawAvg /= static_cast<double>(gait.size());
            graphAvg /= static_cast<double>(gait.size());
            const double period = ups >= 2 ? (tLast - tFirst) / (ups - 1) : 0.0;
            cadence = std::format("{} samples / {:.2f} s, foot swing axis {} range {:.1f} u, {} cycles, cycle period {}, ground speed {:.1f} u/s, "
                                  "cycle length {}, Speed raw {:.1f} -> graph0 {:.1f}",
                gait.size(), dt, "xyz"[ax], hi[ax] - lo[ax], ups, period > 0 ? std::format("{:.3f} s", period) : std::string("n/a (< 2 cycles)"), ground,
                period > 0 ? std::format("{:.1f} u", ground * period) : std::string("n/a"), rawAvg, graphAvg);
        }
        logger::info("FRIK-flat C43b body graph speed: {} | steps {} graph-driven {} contour {} frozen {} | Speed raw avg {:.1f} graph0 avg {:.1f} | "
                     "adjustments applied {} (events {}){} | gait: {}",
            !g_speedInstalled ? "NOT installed" : (speedEnabled() ? "on" : "OFF ([FRIK] bBodyGraphSpeedContour=0)"), spdSteps, drivenSteps, contourSteps,
            frozenSteps, spdSteps ? rawSum / spdSteps : 0.0, spdSteps ? graphSum / spdSteps : 0.0, adjApplied, adjEvents,
            adjSeen.empty() ? "" : std::format(" [{} ]", adjSeen), cadence);
    }
}
