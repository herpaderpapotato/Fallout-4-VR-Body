#include "FRIK.h"

#include "Config.h"
#include "ExternalAuthority.h"
#include "GameHooks.h"
#include "PapyrusApi.h"
#include "api/ApiCore.h"
#include "api/FRIKApiV2.h"
#include "api/RecoilControllerRuntime.h"
#include "config-mode/PipboyConfigMode.h"
#include "devbench/DevBench.h"
#include "devbench/FrikDevBench.h"
#include "f4vr/DebugDump.h"
#include "f4vr/F4VRSkelly.h"
#include "f4vr/F4VRUtils.h"
#include "modsconfig/ModsConfigFO4Api.h"
#include "perf/Perf.h"
#include "pipboy/Pipboy.h"
#include "skeleton/HandPose.h"
#include "skeleton/Skeleton.h"
#include "smooth-movement/SmoothMovementVR.h"
#include "utils.h"
#include "vrcf/VRControllersManager.h"
#include "vrui/UIManager.h"
#include "vrshim/Bridge.h" // FRIK-flat C12
#include "fo4flatvr/FrikBridgeApi.h" // FRIK-flat selfie test flag
#include "vrshim/VrNodes.h" // FRIK-flat C17
#include "vrshim/XfMath.h" // FRIK-flat C22
#include "vrshim/StableSkin.h" // FRIK-flat C23

using namespace common;

// This is the entry point to the mod.
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_skse, F4SE::PluginInfo* a_info)
{
    #ifdef FRIK_FLAT // C2: the flat build loads on the flat runtime only
    if (a_skse->IsEditor() || a_skse->RuntimeVersion() != F4SE::RUNTIME_LATEST) {
#else
    if (a_skse->IsEditor() || !REL::Module::IsVR() || REL::Module::get().version() != F4SE::RUNTIME_VR_1_2_72) {
#endif
        return false;
    }

    return g_mod->onF4SEPluginQuery(a_skse, a_info);
}

// This is the entry point to the mod.
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
#ifdef FRIK_FLAT // C12: dormant unless fo4flatvr.dll hands over its bridge ([FRIK] bEnable=1): no hooks, no patches, no log
    if (!vrshim::bridge::connect()) {
        return true;
    }
    // C17 (bridge v3): [FRIK] bBody=0 -> smooth movement only: logger + config, then the camera offset callback; none of
    // FRIK's hooks, patches, skeleton or frame (fo4flatvr's floating gun stays in charge of the weapon)
    const bool body = vrshim::bridge::bodyEnabled();
    const bool ok = body ? g_mod->onF4SEPluginLoad(a_f4se) : g_mod->onF4SEPluginLoadSmoothOnly(a_f4se);
    if (ok) {
        vrshim::bridge::registerCameraOffset(
            [](void*, float a_offset[3]) noexcept -> std::uint8_t {
                try {
                    return frik::g_frik.smoothMovementFlat(a_offset) ? 1 : 0;
                } catch (...) {
                    return 0;
                }
            },
            nullptr);
        vrshim::bridge::onLoaded();
    }
    return ok;
#else
    return g_mod->onF4SEPluginLoad(a_f4se);
#endif
}

namespace frik
{
    namespace
    {
        constexpr std::uint32_t kSkeletonInitDelayFramesAfterRelease = 1;

        // The game can report a transient power armor state during enter/exit transitions, require the
        // new state to hold for a few frames before paying the cost of rebuilding the whole skeleton.
        constexpr std::uint32_t kPowerArmorChangeConfirmFrames = 3;
    }

    /**
     * On mod loaded by F4SE.
     */
    void FRIK::onModLoaded(const F4SE::LoadInterface*)
    {
        std::srand(static_cast<unsigned int>(time(nullptr)));

        logger::info("Run patches...");
        hook::patchAll();

        logger::info("Hook main...");
        hook::hookMain();

        // the framework registers the tool once the game has loaded, with everything set up here
        devbench::setupDevBenchTool();

        registerModsConfigEntry();
    }

    /**
     * Adds FRIK's main config to the quick launch section of Mods Config FO4 VR's front page, and the functions of the config
     * spec's buttons that open the body adjustment, the Pip-Boy config and the weapon adjustment, and the state the game's
     * Pip-Boy placement is kept in for the Pip-Boy page. The framework connects the client at kPostLoad, and nothing is
     * called while Mods Config FO4 VR is not installed. The menu calls a press on the game thread, which opens the config at
     * once, unless a config of FRIK is open already, as FRIK builds another main config UI on every open. The menu is
     * minimized while a config of FRIK is open, and shown again as the last of them closes. An adjustment opened from its
     * page hands its changes to the menu (Config::setEditingForModsConfig). One opens only while the skeleton is ready.
     */
    void FRIK::registerModsConfigEntry()
    {
        using namespace mods_config::api;
        // handOver: an adjustment opened from its page, whose changes go to the menu, which shows FRIK; FRIK's own config
        // opens from the menu's quick launch entry, and saves as it does without Mods Config FO4 VR
        const auto openConfig = [this](const auto& open, const bool handOver) {
            if (!isSkeletonReady()) {
                return FunctionResult{ .done = false, .message = "FRIK's configuration opens once a game is loaded." };
            }
            if (!api::core::isConfigOpen()) {
                g_config.setEditingForModsConfig(handOver);
                open();
            }
            _minimizedModsConfig = true;
            ModsConfigFO4Client::get().minimizeMenu();
            return FunctionResult{ .done = true, .message = {} };
        };

        auto& client = ModsConfigFO4Client::get();
        client.addQuickLaunch("openMainConfig",
            { .text = "FRIK Configuration",
                .help = "Opens FRIK's own configuration in the world: body height, arm length, and the weapon and Pip-Boy positions.",
                .buttonText = {} },
            [this, openConfig](const Params&) {
                return openConfig(
                    [this] {
                        _mainConfigMode.openConfigMode();
                    },
                    false);
            });

        client.addFunction("openBodyAdjustmentConfig", [this, openConfig](const Params&) {
            return openConfig(
                [this] {
                    _mainConfigMode.openBodyAdjustmentConfigMode();
                },
                true);
        });

        client.addFunction("openPipboyScreenAdjustmentConfig", [this, openConfig](const Params&) {
            if (!isPipboyEnabled()) {
                return FunctionResult{ .done = false, .message = "FRIK's Pip-Boy is turned off by another mod." };
            }
            return openConfig(
                [this] {
                    openPipboyConfigurationModeActive();
                },
                true);
        });

        client.addFunction("openWeaponAdjustmentConfig", [this, openConfig](const Params&) {
            if (!isWeaponPositionEnabled()) {
                return FunctionResult{ .done = false, .message = "FRIK's weapon positioning is turned off by another mod." };
            }
            return openConfig(
                [this] {
                    toggleWeaponRepositionMode();
                },
                true);
        });

        // the game's Pip-Boy placement, which the Pip-Boy page's stepper sets and its rows key on. The menu calls the setter
        // on the game thread, and again with a value the game has, which changes nothing
        client.addState(
            "pipboyPlacement",
            [] {
                return std::optional<Value>(static_cast<int>(f4vr::getPipboyPlacement()));
            },
            [this](const Value& value) {
                const auto placement = std::get_if<int>(&value);
                if (!placement || *placement < static_cast<int>(f4vr::PipboyPlacement::Wrist) || *placement > static_cast<int>(f4vr::PipboyPlacement::Projected)) {
                    logger::warn("Mods Config FO4 VR gave an unknown Pip-Boy placement");
                    return;
                }
                if (static_cast<f4vr::PipboyPlacement>(*placement) != f4vr::getPipboyPlacement()) {
                    // close the wrist Pip-Boy first: once the game no longer puts it on the wrist, the close is ignored
                    closePipboy();
                    f4vr::setPipboyPlacement(static_cast<f4vr::PipboyPlacement>(*placement));
                }
            });
    }

    /**
     * Shows Mods Config FO4 VR's menu again once no config of FRIK is open, after one was opened from it: the Pip-Boy and
     * weapon configs open from the main config, which closes as they do.
     */
    void FRIK::restoreModsConfigMenuOnConfigClose()
    {
        if (_minimizedModsConfig && !api::core::isConfigOpen()) {
            _minimizedModsConfig = false;
            g_config.setEditingForModsConfig(false);
            mods_config::api::ModsConfigFO4Client::get().restoreMenu();
        }
    }

    /**
     * On game fully loaded initialize things that should be initialized only once.
     */
    void FRIK::onGameLoaded()
    {
        initForFalloutLondonVR();

        // FRIK poses the hands itself, where the framework would point a hand through FRIK's own API
        vrui::g_uiManager->setHandPointingHandler([](const bool primaryHand, const bool toPoint) {
            HandPose::setForceHandPointingPose(primaryHand, toPoint);
        });

        logger::info("Register papyrus native functions...");
        api::initPapyrusApis();
        _boneSpheres.init();

        _gameMenusHandler.init([this](const std::string& name, const bool isOpened) {
            if (isOpened) {
                onGameMenuOpened(name, isOpened);
            }
        });

        addEmbeddedFlashlightKeywordIfNeeded();
        subscribeToConfigChanges();

        // F4SE refuses a listener for a sender no loaded plugin is named, so the registration is what detects
        // BetterScopesVR's plugin, whatever its DLL is called and whether or not its ESP is installed
        _betterScopesVRLoaded = _messaging->RegisterListener(onBetterScopesMessage, BETTER_SCOPES_VR_MOD_NAME);
        if (_betterScopesVRLoaded) {
            logger::info("BetterScopesVR mod detected, registered for its messages");
            _messaging->Dispatch(15, static_cast<void*>(nullptr), sizeof(bool), BETTER_SCOPES_VR_MOD_NAME);
            // legacy path: its near-eye message is the looking-through signal, everything else stays vanilla
            g_scopeAuthority.setProvider(BETTER_SCOPES_VR_MOD_NAME, static_cast<std::uint32_t>(ScopeCapability::PublishesLookingThrough));
        }
    }

    /**
     * Check if the Fallout London VR mod is loaded and initialize compatibility mode if so.
     * Special flag to ignore the mod can be used to use regular Pipboy instead of the Attaboy.
     */
    void FRIK::initForFalloutLondonVR()
    {
        g_config.isFalloutLondonVR = isFalloutLondonVRModLoaded();
        if (g_config.isFalloutLondonVR && g_config.ignoreFalloutLondonVR) {
            // checked before the config swap, so ignoring the mod keeps FRIK.toml and its file watch
            logger::warn("Ignore Fallout London VR flag set, do not treat the game as Fallout London!");
            g_config.isFalloutLondonVR = false;
        } else if (g_config.isFalloutLondonVR) {
            logger::info("Fallout London VR mod detected, enabling compatibility mode...");
            g_config.reloadForFalloutLondonVR();
        }
    }

    /**
     * Game session can be initialized multiple times as it is fired on new game and save loaded events.
     * We should clear and reload as much of the game state as we can.
     */
    void FRIK::onGameSessionLoaded()
    {
        // Any config left open belongs to the previous session and is attached to nodes that are about to be
        // replaced. Main config is owned by FRIK so it must be closed explicitly, the Pipboy and weapon
        // reposition configs are released together with the skeleton they observe.
        _mainConfigMode.closeConfigMode();
#ifdef FRIK_FLAT // C37: a loaded game has a new body; its first first-person frame takes the engine pose afresh
        _flatTrackActive = false;
        _flatTrackTree = nullptr;
        _flatBodyHasWritten = false;
#endif

        if (_skelly) {
            logger::info("Resetting skeleton for new game session...");
            releaseSkeleton();
        }

        // A scope provider's published state was about the weapon and scope of the previous session, so none of
        // them can still know whether the player is looking through one; the registrations stay and each publishes
        // its next edge. Without this a provider that missed its own exit edge leaves FRIK scoped for the rest of the
        // run, and reloading - the player's instinctive fix - is the one action guaranteed not to help.
        // Safe although this function runs off the game thread (docs/features-backlog.md): ScopeAuthority takes its
        // own lock and the merged flag is atomic, so the game thread reading it mid-frame sees one value or the other.
        if (g_scopeAuthority.clearPublishedLookingThrough()) {
            logger::info("New game session: cleared the published looking-through-scope state");
        }

        _smoothMovement.reset();
        configureGameVars();

        _playerControlsHandler.hardReset();
    }

    /**
     * Called on every game frame via hooks into the game engine.
     * This is where all the magic happens by updating game state and nodes.
     */
    void FRIK::onFrameUpdate()
    {
        F4CF_PERF_FUNCTION();

        // Scope providers publish before this frame, so clients hear the flip before any phase runs
        broadcastScopeEvents();

        // the one phase that runs with or without a skeleton
        api::core::invokeFramePhase(FramePhase::FrameBegin);

        onFrameUpdateInner();

        restoreModsConfigMenuOnConfigClose();

        // always paired with FrameBegin, even when the inner update returned early
        api::core::invokeFramePhase(FramePhase::FrameEnd);

        api::core::flushHandClaimLog();
    }

    void FRIK::onFrameUpdateInner()
    {
        F4CF_PERF_FUNCTION();

        if (!RE::PlayerCharacter::GetSingleton()) {
            // game not loaded or existing
            return;
        }
#ifdef FRIK_FLAT // C23: the body only in first person (0) / iron sights (4); other camera states (third person, VATS, dialogue,
        // furniture: fo4flatvr's cinema screen) show the engine's own body
        {
            const auto cam = f4vr::getPlayerCamera();
            const int  cs = cam && cam->currentState ? static_cast<int>(cam->currentState->id.get()) : -1;
            const bool fp = cs == 0 || cs == 4;
            if (!fp) {
                // C35 (user 15 Nov: the player's head must be visible in every view that is not at the head): FRIK's head
                // gear / face cull is applied only in this first-person frame, so leaving first person kept it. Show it again
                // (no-op once restored; only what FRIK's own cull hid).
                if (_skelly) {
                    _skelly->restoreCulledGeometry();
                }
                if (_flatSkinActive) {
                    const auto pl = RE::PlayerCharacter::GetSingleton();
                    vrshim::skin::restore(pl->loadedData && pl->loadedData->data3D ? pl->loadedData->data3D.get() : nullptr);
                    flatTrackRestore(cs);  // C37: what FRIK left on bones the engine does not re-animate goes back
                    _flatSkinActive = false;
                    logger::info("FRIK-flat: camera state {} (not first person): body left to the engine", cs);
                }
                return;
            }
            _flatSkinActive = true;
        }
#endif
#ifdef FRIK_FLAT // test only: flag file fo4flatvr_frik_selfie.test next to fo4flatvr.ini -> FRIK's selfie mode (body projected in front)
        {
            static bool s_checked = false;
            if (!s_checked && vrshim::bridge::api() && vrshim::bridge::api()->IniPath) {
                s_checked = true;
                std::string flag = vrshim::bridge::api()->IniPath();
                if (const auto slash = flag.find_last_of("\\/"); slash != std::string::npos) {
                    flag.resize(slash + 1);
                }
                flag += "fo4flatvr_frik_selfie.test";
                if (std::filesystem::exists(flag)) {
                    logger::info("FRIK-flat: selfie test flag present - selfie mode on");
                    setSelfieMode(true);
                }
            }
        }
#endif
#ifdef FRIK_FLAT // C18: HMD offset as the PlayerWorldNode z when smooth movement does not run (applied at the next frame hook)
        vrshim::bridge::setDefaultPlayerWorldZ(Skeleton::getAdjustedPlayerHMDOffset());
        if (!_skelly) {
            flatBodyDiag();
        }
#endif

        if (_skelly) {
            if (!isRootNodeValid()) {
                logger::warn("Root node released, reset skelly... PowerArmorChange?({})", _inPowerArmor != f4vr::isInPowerArmor());
                releaseSkeleton();
#ifdef FRIK_FLAT // C19: the skeleton caches the first-person hands; they switch between real and stand-in on draw/holster
            } else if (_flatFpHandsGen != vrshim::fpHandsGeneration()) {
                logger::info("FRIK-flat: first-person hands switched ({}), reset skeleton...", vrshim::fpHands().standIn ? "stand-ins" : "real");
                releaseSkeleton();
#endif
            } else if (_inPowerArmor != f4vr::isInPowerArmor()) {
                if (++_powerArmorChangeFrames >= kPowerArmorChangeConfirmFrames) {
                    logger::info("Power Armor state changed for {} frames, reset skeleton...", _powerArmorChangeFrames);
                    releaseSkeleton();
                }
            } else {
                if (_powerArmorChangeFrames > 0) {
                    logger::info("Power Armor state stabilized after {} frames", _powerArmorChangeFrames);
                }
                _powerArmorChangeFrames = 0;
            }
        }

        if (!_skelly) {
            if (_gameMenusHandler.isLoadingMenuOpen()) {
#ifdef FRIK_FLAT // C15
                logger::sample(5000, "FRIK-flat: skeleton init waits for the loading menu to close");
#endif
                return;
            }

            if (_skeletonInitDelayFrames > 0) {
                --_skeletonInitDelayFrames;
                return;
            }

            if (!isGameReadyForSkeletonInitialization()) {
                return;
            }

            initSkeleton();
            if (!_skelly) {
                return;
            }
        }

        if (_weaponPositionEnabled) {
            _weaponPosition->onFrameStart();
        }

#ifdef FRIK_FLAT // C37: the engine's own pose of this frame per bone (before FRIK writes), for C28 and the exit restore
        flatTrackFrameStart();
#endif
#ifdef FRIK_FLAT // C22 diag: what the engine changed in the body tree since FRIK's last frame (render may read it in between)
        if (auto* tree = f4vr::getFlattenedBoneTree(); tree && tree->transforms && static_cast<int>(_flatEndOfFrame.size()) == tree->numTransforms) {
            float mx = 0.0f;
            int   mi = -1, n = 0;
            const auto rootT = tree->transforms[0].world.translate;
            for (int i = 0; i < tree->numTransforms; ++i) {
                const float d = MatrixUtils::vec3Len((tree->transforms[i].world.translate - rootT) - (_flatEndOfFrame[i] - _flatEndOfFrame[0]));
                n += d > 2.0f ? 1 : 0;
                if (d > mx) {
                    mx = d, mi = i;
                }
            }
            _flatBetweenRoot = MatrixUtils::vec3Len(rootT - _flatEndOfFrame[0]);
            // C28 input (the animation's pose of this frame, before FRIK) comes from C37 flatTrackFrameStart now
            // C28 diag (user: Drifter coat skirt splayed "as if the knees are bent all the way"): the engine's leg pose at the start
            // of the frame (knee angle) and the leg-hanging helper bones (children of pelvis/thighs/calves), every 5 s
            static auto s_legT = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            if (std::chrono::steady_clock::now() - s_legT > std::chrono::seconds(5)) {
                s_legT = std::chrono::steady_clock::now();
                auto idx = [&](const char* nm) {
                    for (int i = 0; i < tree->numTransforms; ++i) {
                        if (tree->transforms[i].name.c_str() && _stricmp(tree->transforms[i].name.c_str(), nm) == 0) return i;
                    }
                    return -1;
                };
                auto knee = [&](const char* th, const char* ca, const char* fo) {
                    const int a = idx(th), b = idx(ca), c = idx(fo);
                    if (a < 0 || b < 0 || c < 0) return -1.0f;
                    const auto u = MatrixUtils::vec3Norm(tree->transforms[b].world.translate - tree->transforms[a].world.translate);
                    const auto v = MatrixUtils::vec3Norm(tree->transforms[c].world.translate - tree->transforms[b].world.translate);
                    return std::acos(std::clamp(MatrixUtils::vec3Dot(u, v), -1.0f, 1.0f)) * 57.29578f;
                };
                std::string helpers;
                for (int i = 0; i < tree->numTransforms; ++i) {
                    const auto& b = tree->transforms[i];
                    if (b.parPos < 0 || b.parPos >= tree->numTransforms) continue;
                    const char* pn = tree->transforms[b.parPos].name.c_str();
                    const char* nm = b.name.c_str();
                    if (!nm) continue;
                    const bool skirt = std::strstr(nm, "Skirt") || std::strstr(nm, "skirt") || std::strstr(nm, "Coat") || std::strstr(nm, "Cloth");
                    if (!skirt && (!pn || std::strstr(nm, "_skin"))) continue;
                    if (!skirt && !(std::strstr(pn, "Leg_") || _stricmp(pn, "Pelvis") == 0)) continue;
                    const auto& r = b.local.rotate;
                    const float tr = std::clamp((r.entry[0][0] + r.entry[1][1] + r.entry[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
                    helpers += std::format(" {}<{}{}:{:.0f}", nm, pn ? pn : "-", b.refNode ? "" : "*", std::acos(tr) * 57.29578f);
                }
                logger::info("FRIK-flat legs (engine pose, frame start): knee L {:.0f} R {:.0f} deg | {} tree bones | pelvis/leg children + skirt bones (local rot deg):{}",
                    knee("LLeg_Thigh", "LLeg_Calf", "LLeg_Foot"), knee("RLeg_Thigh", "RLeg_Calf", "RLeg_Foot"), tree->numTransforms, helpers);
            }
            _flatBetweenMax = mx;
            _flatBetweenCount = n;
            _flatBetweenName = mi >= 0 ? tree->transforms[mi].name.c_str() : "-";
        }
#endif
        logger::trace("Update Skeleton...");
        _skelly->onFrameUpdate();

        logger::trace("Update Bone Sphere...");
        _boneSpheres.onFrameUpdate();

        logger::trace("Update player controls...");
        _playerControlsHandler.onFrameUpdate(_mainConfigMode, _pipboy.get(), _weaponPosition.get(), _pipboyConfigMode.get());

        if (_weaponPositionEnabled) {
            logger::trace("Update Weapon Position...");
            _weaponPosition->onFrameUpdate();
        }
        api::core::invokeFramePhase(FramePhase::AfterWeaponPosition);

        logger::trace("Update Pipboy...");
        _pipboy->onFrameUpdate();

        // run here, in place of the framework's own call after this update, so the UI is laid out before the config modes and the final world update
        vrui::g_uiManager->onFrameUpdate();

        _mainConfigMode.onFrameUpdate();

        _pipboyConfigMode->onFrameUpdate();

        api::core::invokeFramePhase(FramePhase::BeforeWorldFinal);
        updateWorldFinal();
#ifdef FRIK_FLAT // C22 / C23 / C28
        flatTransferLegHelpers();
        flatPropagateNodelessBones();
        if (const auto pl = RE::PlayerCharacter::GetSingleton(); pl && pl->loadedData && pl->loadedData->data3D) {
            vrshim::skin::refresh(pl->loadedData->data3D.get());  // the render skins this copy, not the bones the next update animates
        }
        flatTrackFrameEnd();  // C37
#endif
        _skelly->latchRenderedWrists();

        if (!_skeletonReadyPublished) {
            _skeletonReadyPublished = true;
            logger::info("Broadcasting API lifecycle event: kSkeletonReady (generation {})", _skeletonGeneration);
            broadcastSkeletonLifecycle(static_cast<std::uint32_t>(api::FRIKApiV2::LifecycleEvent::kSkeletonReady));
            f4cf::devbench::emit("skeleton.ready", [this] {
                return nlohmann::json{ { "generation", _skeletonGeneration } };
            });
        }
        // After the ready broadcast so a client never sees a phase of a skeleton it was not told about
        api::core::invokeFramePhase(FramePhase::AfterWorldFinal);
#ifdef FRIK_FLAT // C18
        flatBodyDiag();
#endif
    }

    void FRIK::smoothMovement()
    {
        if (!_smoothMovementEnabled) {
            return;
        }
        try {
            _smoothMovement.onFrameUpdate();
        } catch (const std::exception& e) {
            logger::error("Error in FRIK::smoothMovement: {}", e.what());
        }
    }

#ifdef FRIK_FLAT // C17
    /**
     * Flat: FO4VR steps SmoothMovementVR from Main::OnIdle (VR D83EC4 -> 1BA7BA0, which copies the OpenVR HMD pose into the
     * VR world data; no flat counterpart) and the engine places the HMD camera under PlayerWorldNode, whose local translate
     * carries the smoothing offset. On flat fo4flatvr builds the camera: it calls this once per FirstPersonState::Update
     * (player position final for the frame) and adds the offset to the camera position.
     */
    bool FRIK::smoothMovementFlat(float a_offset[3])
    {
        a_offset[0] = a_offset[1] = a_offset[2] = 0.0f;
        if (!_smoothMovementEnabled || g_config.disableSmoothMovement) {
            return false;
        }
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell || !player->Get3D()) {
            // between cells / before the 3D exists: FRIK's isInInternalCell() reads parentCell unchecked
            _smoothMovement.reset();
            return false;
        }
        if (!vrshim::vrNodesCreated()) {
            vrshim::createVrNodes();  // SmoothMovementVR writes PlayerWorldNode (a detached stand-in node on flat)
        }
        // no camera update for a while (loading screen, menu with the camera stopped, third person): start from the current
        // position like FRIK's game-session reset, instead of gliding from where the player was before
        static auto s_last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last > std::chrono::milliseconds(500)) {
            _smoothMovement.reset();
        }
        s_last = now;
        _smoothMovement.onFrameUpdate();
        const auto& o = _smoothMovement.flatOffset();
        a_offset[0] = o.x, a_offset[1] = o.y, a_offset[2] = o.z;
        // C18: the PlayerWorldNode z this frame (smoothing z + HMD offset) for the body (vrshim::bridge, posed at the frame hook)
        if (const auto* nodes = vrshim::getVRPlayerNodes(); nodes && nodes->playerWorldNode) {
            vrshim::bridge::setSmoothedPlayerWorldZ(nodes->playerWorldNode->local.translate.z);
        }
        return true;
    }

    /**
     * C18 diagnostics for the body on flat, once a second while the body runs (compact; FRIK.log):
     * cull flags of the third-person root (Get3D(0), which DoShow1stPerson app-culls in first person unless patched) and the
     * first-person root, heights (feet, camera, eye = camera - feet, PlayerWorldNode z, body root / COM / neck / head),
     * neck-to-camera horizontal offset, and the hand chain: wand -> first-person hand -> third-person hand distances.
     */
    void FRIK::flatBodyDiag()
    {
        static auto s_last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last < std::chrono::milliseconds(1000)) {
            return;
        }
        s_last = now;
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto camera = f4vr::getPlayerCamera();
        if (!player || !camera || !camera->cameraRoot) {
            return;
        }
        const auto third = player->Get3D(false);
        const auto first = player->Get3D(true);
        const auto root = f4vr::getRootNode();
        const auto cam = camera->cameraRoot->world.translate;
        const float feet = player->data.location.z;
        auto z = [](const RE::NiAVObject* o) { return o ? o->world.translate.z : -1.0f; };
        auto dist = [](const RE::NiAVObject* a, const RE::NiAVObject* b) {
            return a && b ? MatrixUtils::vec3Len(a->world.translate - b->world.translate) : -1.0f;
        };
        const auto neck = root ? f4vr::findNode(root, "Neck") : nullptr;
        const auto head = root ? f4vr::findNode(root, "Head") : nullptr;
        const auto com = root ? f4vr::findNode(root, "COM") : nullptr;
        const auto fp = f4vr::getFirstPersonSkeleton();
        const auto fpR = vrshim::fpHands().rHand;
        const auto fpL = vrshim::fpHands().lHand;
        const auto tpR = root ? f4vr::findNode(root, "RArm_Hand") : nullptr;
        const auto tpL = root ? f4vr::findNode(root, "LArm_Hand") : nullptr;
        const auto nodes = f4vr::getVRPlayerNodes();
        const auto pwnZ = nodes && nodes->playerWorldNode ? nodes->playerWorldNode->local.translate.z : -1.0f;
        const float neckXY = neck ? std::hypot(neck->world.translate.x - cam.x, neck->world.translate.y - cam.y) : -1.0f;
        const auto fmtP = [](const RE::NiAVObject* o) {
            return o ? std::format("({:.1f},{:.1f},{:.1f})", o->world.translate.x, o->world.translate.y, o->world.translate.z) : std::string("-");
        };
        {
            const RE::NiAVObject* c0 = fp && !fp->children.empty() ? fp->children[0].get() : nullptr;
            const auto* rtti = c0 ? c0->GetRTTI() : nullptr;
            logger::info("FRIK-flat body: fp children={} child0='{}' rtti={} weaponDrawn={}", fp ? fp->children.size() : 0,
                c0 && c0->name.c_str() ? c0->name.c_str() : "-", rtti && rtti->GetName() ? rtti->GetName() : "-", f4vr::isWeaponDrawn());
        }
        logger::info("FRIK-flat body: nodeless bones {} re-parented, max correction {:.1f} u ({}); stable skin: {} instances, {} bones, {} rebuilds",
            _flatNodelessCount, _flatNodelessMaxErr, _flatNodelessMaxName, vrshim::skin::stats().instances, vrshim::skin::stats().bones,
            vrshim::skin::stats().rebuilds);
        {
            // C26 heading check: body (third-person root +Y), camera, actor heading; degrees, same convention
            auto yawOf = [](const RE::NiMatrix3& r) { return std::atan2(-r.entry[1][0], r.entry[1][1]) * 57.29578f; };
            const float bodyYaw = root ? yawOf(root->world.rotate) : 0.0f;
            const float camYaw = yawOf(camera->cameraRoot->world.rotate);
            float d = bodyYaw - camYaw;
            while (d > 180.0f) d -= 360.0f;
            while (d < -180.0f) d += 360.0f;
            // hands in the camera's yaw frame (x right, y forward, z up; should not change with a stick/snap turn)
            auto inCam = [&](const RE::NiAVObject* o) {
                if (!o) return std::string("-");
                const auto& r = camera->cameraRoot->world.rotate;
                const RE::NiPoint3 fwd = MatrixUtils::vec3Norm(RE::NiPoint3(r.entry[1][0], r.entry[1][1], 0));
                const RE::NiPoint3 right(fwd.y, -fwd.x, 0);
                const auto dv = o->world.translate - camera->cameraRoot->world.translate;
                const auto& w = o->world.rotate;  // hand forward (+Y) and up (+Z) axes in the same yaw frame
                auto loc = [&](const RE::NiPoint3& v) { return RE::NiPoint3(v.x * right.x + v.y * right.y, v.x * fwd.x + v.y * fwd.y, v.z); };
                const auto p = loc(dv), f = loc(RE::NiPoint3(w.entry[1][0], w.entry[1][1], w.entry[1][2])), u = loc(RE::NiPoint3(w.entry[2][0], w.entry[2][1], w.entry[2][2]));
                return std::format("p({:.1f},{:.1f},{:.1f}) y({:.2f},{:.2f},{:.2f}) z({:.2f},{:.2f},{:.2f})", p.x, p.y, p.z, f.x, f.y, f.z, u.x, u.y, u.z);
            };
            logger::info("FRIK-flat heading: body {:.1f} cam {:.1f} body-cam {:.1f} actor {:.1f} deg | grip from gun {}/{} frames | inCam L3p {} R3p {}",
                bodyYaw, camYaw, d, player->data.angle.z * 57.29578f, vrshim::gripFromGunFrames(), vrshim::gripFrames(), inCam(tpL), inCam(tpR));

            // C32 diag (wrist invariant): each stage of the hand's chain as a rotation relative to the raw controller grip
            // (camera . grip-in-head), rotation vector in degrees. With the controllers still in tracking space none of these may
            // change with the world heading. wand = grip . calibration; off = FRIK's offset node (after dampenHand); tgt = FRIK's
            // hand target (first-person stand-in hand); node = third-person hand node after the solve; tree = its bone-tree entry
            // (what the skin copies); fa3 = third-person ForeArm3. ax = tree hand X/Z axes and grip X/-Z in the camera frame.
            {
                using namespace vrshim::xf;
                const Xf camX = FromNi(camera->cameraRoot->world);
                auto rv = [](const M3& r) {
                    const double tr = r.m[0][0] + r.m[1][1] + r.m[2][2];
                    const double a = std::acos(std::clamp((tr - 1.0) * 0.5, -1.0, 1.0));
                    V ax{ r.m[2][1] - r.m[1][2], r.m[0][2] - r.m[2][0], r.m[1][0] - r.m[0][1] };
                    const double s = std::sin(a);
                    const double k = s > 1e-6 ? a / (2.0 * s) : 0.5;
                    return std::format("({:.0f},{:.0f},{:.0f})", ax.x * k * 57.29578, ax.y * k * 57.29578, ax.z * k * 57.29578);
                };
                auto* tree = f4vr::getFlattenedBoneTree();
                auto treeWorld = [&](const char* nm, Xf& out) {
                    if (!tree || !tree->transforms) return false;
                    for (int i = 0; i < tree->numTransforms; ++i) {
                        if (tree->transforms[i].name.c_str() && _stricmp(tree->transforms[i].name.c_str(), nm) == 0) {
                            out = FromNi(tree->transforms[i].world);
                            return true;
                        }
                    }
                    return false;
                };
                const bool lh = vrshim::bridge::leftHanded();
                std::string line;
                for (int side = 0; side < 2; ++side) {
                    const bool isLeft = side == 0;
                    float gr[3][3], gp[3];
                    if (!vrshim::bridge::gripWorld(side, gr, gp)) {
                        line += isLeft ? " L -" : " | R -";
                        continue;
                    }
                    RE::NiTransform gn;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) gn.rotate.entry[i][j] = gr[i][j];
                    const M3 gInv = T(FromNi(gn).R);
                    auto rel = [&](const RE::NiAVObject* o) { return o ? rv(Mul(gInv, FromNi(o->world).R)) : std::string("-"); };
                    const bool primary = isLeft == lh;
                    const RE::NiAVObject* wand = nodes ? (primary ? nodes->primaryWandNode : nodes->secondaryWandNode) : nullptr;
                    const RE::NiAVObject* off = nodes ? (primary ? nodes->primaryWeaponOffsetNode : nodes->secondaryMeleeWeaponOffsetNode) : nullptr;
                    const RE::NiAVObject* tgt = isLeft ? fpL : fpR;
                    const RE::NiAVObject* nd = isLeft ? tpL : tpR;
                    const RE::NiAVObject* fa3 = root ? f4vr::findNode(root, isLeft ? "LArm_ForeArm3" : "RArm_ForeArm3") : nullptr;
                    Xf tw;
                    const bool haveTree = treeWorld(isLeft ? "LArm_Hand" : "RArm_Hand", tw);
                    std::string axes = "-";
                    if (haveTree) {
                        const M3 h = Mul(T(camX.R), tw.R);              // tree hand in the camera frame (columns = axes)
                        const M3 g = Mul(T(camX.R), FromNi(gn).R);      // grip in the camera frame
                        axes = std::format("hX({:.2f},{:.2f},{:.2f}) hZ({:.2f},{:.2f},{:.2f}) gX({:.2f},{:.2f},{:.2f}) g-Z({:.2f},{:.2f},{:.2f})", h.m[0][0], h.m[1][0],
                            h.m[2][0], h.m[0][2], h.m[1][2], h.m[2][2], g.m[0][0], g.m[1][0], g.m[2][0], -g.m[0][2], -g.m[1][2], -g.m[2][2]);
                    }
                    line += std::format("{} {} wand{} off{} tgt{} node{} tree{} fa3{} ax {}", isLeft ? "" : " |", isLeft ? "L" : "R", rel(wand), rel(off), rel(tgt), rel(nd),
                        haveTree ? rv(Mul(gInv, tw.R)) : std::string("-"), rel(fa3), axes);
                }
                logger::info("FRIK-flat wrist: cam {:.1f} body {:.1f} actor {:.1f} drawn {} unarmed {}{}", camYaw, bodyYaw, player->data.angle.z * 57.29578f,
                    f4vr::isWeaponDrawn() ? 1 : 0, f4vr::isUnarmedWeaponDrawn() ? 1 : 0, line);
            }
        }
        logger::info("FRIK-flat body: R hand {} wpnOff {} | L hand {} | 3p R {} L {} | cam ({:.1f},{:.1f},{:.1f})", fmtP(fpR),
            fmtP(nodes ? nodes->primaryWeaponOffsetNode : nullptr), fmtP(fpL), fmtP(tpR), fmtP(tpL), cam.x, cam.y, cam.z);
        logger::info("FRIK-flat body: skel={} standIn={} cull3p={} cull3pRoot={} cull1p={} feet={:.1f} cam={:.1f} eye={:.1f} pwnZ={:.1f} bodyZ={:.1f} rootZ={:.1f} "
                     "comZ={:.1f} neckZ={:.1f} headZ={:.1f} neckXY={:.1f} scale={:.3f} | R wand-fp={:.1f} fp-3p={:.1f} | L wand-fp={:.1f} fp-3p={:.1f}",
            _skelly ? 1 : 0, vrshim::fpHands().standIn ? 1 : 0, third ? (third->GetAppCulled() ? 1 : 0) : -1, root ? (root->GetAppCulled() ? 1 : 0) : -1,
            first ? (first->GetAppCulled() ? 1 : 0) : -1, feet, cam.z, cam.z - feet, pwnZ,
            player->loadedData && player->loadedData->data3D ? player->loadedData->data3D->world.translate.z : -1.0f, z(root), z(com), z(neck), z(head), neckXY,
            root ? root->local.scale : 0.0f,
            nodes ? dist(nodes->primaryWeaponOffsetNode, fpR) : -1.0f, dist(fpR, tpR), nodes ? dist(nodes->secondaryMeleeWeaponOffsetNode, fpL) : -1.0f,
            dist(fpL, tpL));
    }
#endif

    void FRIK::initSkeleton()
    {
        F4CF_PERF_FUNCTION();

        _inPowerArmor = f4vr::isInPowerArmor();
        _powerArmorChangeFrames = 0;
        _skeletonInitDelayFrames = 0;
        ++_skeletonGeneration;

        const auto player = f4vr::getPlayer();
        logger::info("Initialize Skeleton ({}) ; Nodes: Player={}, Data={}, Root={}, Skeleton={}, Common={}",
            _inPowerArmor ? "PowerArmor" : "Regular",
            static_cast<const void*>(player),
            static_cast<const void*>(player->loadedData),
            static_cast<const void*>(player->loadedData->data3D.get()),
            static_cast<const void*>(f4vr::getRootNode()),
            static_cast<const void*>(f4vr::getCommonNode()));

        // init skeleton
        _workingRootNode = f4vr::getRootNode();
#ifdef FRIK_FLAT // C19
        _flatFpHandsGen = vrshim::fpHandsGeneration();
#endif
        _skeletonReadyPublished = false;

        auto skelly = Skeleton::create(f4vr::getRootNode(), _inPowerArmor);
        if (!skelly) {
            logger::warn("Skeleton initialization failed after readiness checks; retrying later.");
            _workingRootNode = nullptr;
            _skeletonInitDelayFrames = kSkeletonInitDelayFramesAfterRelease;
            return;
        }
        _skelly = std::move(skelly);

        // init handlers depending on skeleton
        _pipboy = std::make_unique<Pipboy>(_skelly.get());
        _pipboyConfigMode = std::make_unique<PipboyConfigMode>(_skelly.get());
        _weaponPosition = std::make_unique<WeaponPositionAdjuster>(_skelly.get());
    }

    /**
     * Check if game all nodes exist and ready for skeleton handling flow.
     * Based on random crashes and the objects that were missing.
     * Probably not all checks are required, but it's cheap and only happens when skeleton is not initialized.
     */
    bool FRIK::isGameReadyForSkeletonInitialization()
    {
        const auto player = f4vr::getPlayer();
        if (!player) {
            return false;
        }

        const auto playerData = player->loadedData;
        if (!playerData) {
            return false;
        }

        const auto playerRootNode = playerData->data3D.get();
        const auto rootNode = f4vr::getRootNode();
        const auto worldRootNode = f4vr::getWorldRootNode();
        const auto commonNode = f4vr::getCommonNode();
        const auto playerNodes = f4vr::getVRPlayerNodes();
        const auto flattenedTree = f4vr::getFlattenedBoneTree();
        const auto firstPersonSkeleton = f4vr::getFirstPersonSkeleton();
#ifdef FRIK_FLAT // C19: real first-person hands or stand-ins (vrshim::selectFpHands)
        const auto rightHand = firstPersonSkeleton ? vrshim::fpHands().rHand : nullptr;
        const auto leftHand = firstPersonSkeleton ? vrshim::fpHands().lHand : nullptr;
#else
        const auto rightHand = firstPersonSkeleton ? f4vr::findNode(firstPersonSkeleton, "RArm_Hand") : nullptr;
        const auto leftHand = firstPersonSkeleton ? f4vr::findNode(firstPersonSkeleton, "LArm_Hand") : nullptr;
#endif
        const auto weaponNode = f4vr::getWeaponNode();
        const auto camera = f4vr::getPlayerCamera();
        const auto cameraRoot = camera && camera->cameraRoot ? camera->cameraRoot.get() : nullptr;
#ifdef FRIK_FLAT // C15: name the failing readiness check (rate-limited) while bringing the skeleton up on flat
        logger::sample(5000, "FRIK-flat readiness: playerRoot={} root={} worldRoot={} rootParent={} common={} playerNodes={} tree={} fp={} rHand={} lHand={} weapon={} camRoot={}",
            playerRootNode != nullptr, rootNode != nullptr, worldRootNode != nullptr, rootNode && rootNode->parent, commonNode != nullptr, playerNodes != nullptr,
            flattenedTree != nullptr, firstPersonSkeleton != nullptr, rightHand != nullptr, leftHand != nullptr, weaponNode != nullptr, cameraRoot != nullptr);
#endif

        if (!playerRootNode || !rootNode || !worldRootNode) {
            return false;
        }

        if (!rootNode->parent) {
            return false;
        }

        if (!commonNode || !playerNodes || !flattenedTree) {
            return false;
        }

        if (!rightHand || !leftHand) {
            return false;
        }

        if (!weaponNode) {
            return false;
        }

        if (!camera || !cameraRoot) {
            return false;
        }

        return true;
    }

    /**
     * The game can change the basic root object under us.
     * It doesn't happen often but when it does, we should reinitialize the skeleton.
     * Known root release: entering/exiting power armor, after character creation in new game.
     */
    bool FRIK::isRootNodeValid() const
    {
        if (!_workingRootNode) {
            return false;
        }
        if (_workingRootNode != f4vr::getRootNode()) {
            return false;
        }
        if (_workingRootNode->parent == nullptr) {
            return false;
        }
        return true;
    }

    /**
     * On game menu open check is loading menu is open and reset skelly if it does.
     * We want to reinit skeleton after player moves to a new location by fast travel or other.
     */
    void FRIK::onGameMenuOpened(const std::string& name, const bool isOpened)
    {
        if (isOpened && _skelly && _gameMenusHandler.isLoadingMenuOpen()) {
            logger::info("Loading menu is open, reset skeleton...");
            releaseSkeleton();
        }
        if (isOpened && _pipboy && _pipboy->isOpen() && name == "TerminalMenu") {
            logger::info("Close Pipboy due to terminal open...");
            _pipboy->openClose(false);
        }
    }

    /**
     * On switch from normal and power armor, reset the skelly and all dependencies with persistent data.
     */
    void FRIK::releaseSkeleton()
    {
        F4CF_PERF_FUNCTION();
#ifdef FRIK_FLAT // C23
        if (const auto pl = RE::PlayerCharacter::GetSingleton()) {
            vrshim::skin::restore(pl->loadedData && pl->loadedData->data3D ? pl->loadedData->data3D.get() : nullptr);
        }
#endif

        // Every skeleton FRIK created gets its destroy event, whether or not its ready event was reached: a client may have registered
        // against this generation before the first world-final frame, and its registrations are dropped below either way
        if (_skelly) {
            logger::info("Broadcasting API lifecycle event: kSkeletonDestroying (generation {})", _skeletonGeneration);
            broadcastSkeletonLifecycle(static_cast<std::uint32_t>(api::FRIKApiV2::LifecycleEvent::kSkeletonDestroying));
            f4cf::devbench::emit("skeleton.destroying", [this] {
                return nlohmann::json{ { "generation", _skeletonGeneration } };
            });
        }
        _skeletonReadyPublished = false;

        // Every registration was published against nodes that are about to go away, so each registry
        // drops its own here and clients republish after the next skeleton-ready event.
        g_externalAuthority.clearForSkeletonRelease();
        HandPose::clearHandPoseOverridesForSkeletonRelease();
        api::clearWeaponHandRecoilControllersForSkeletonRelease();

        _workingRootNode = nullptr;
        _skeletonInitDelayFrames = kSkeletonInitDelayFramesAfterRelease;

        _playerControlsHandler.reset();
        _smoothMovement.reset();

        // release the dependents before the skeleton they observe
        _pipboy.reset();
        _pipboyConfigMode.reset();
        _weaponPosition.reset();
        _skelly.reset();

        _inPowerArmor = false;
        _powerArmorChangeFrames = 0;
        // _dynamicCameraHeight is a script's setting for the player, not skeleton state: it survives a rebuild

        _boneSpheres.onSkeletonReleased();
    }

    /**
     * Calling three engine-level functions to update the scene graph state for the player's root node and its children,
     * specifically related to geometry bounds, skeletal bone transforms, and flattened tree data.
     * Without it some cull geometry, Pipboy interaction, and hand fingers position may not work.
     */
    void FRIK::updateWorldFinal()
    {
        F4CF_PERF_FUNCTION();

        const auto worldRootNode = f4vr::getWorldRootNode();
        f4vr::BSFadeNode_MergeWorldBounds(worldRootNode);
        f4vr::BSFlattenedBoneTree_UpdateBoneArray(f4vr::getRootNode());
        // just in case any transforms missed because they are not in the tree do a full flat bone array update
        f4vr::BSFadeNode_UpdateGeomArray(worldRootNode, 1);
    }

#ifdef FRIK_FLAT // C28
    /**
     * Flat (user headset 15 Nov: long coat panels splayed "as if the knees are bent all the way"): the animation keeps running on
     * the third-person body in first person (measured: knees 45 deg bent in the engine pose) and FRIK replaces the leg pose,
     * but helper bones hung under the pelvis that the animation turns with the thighs (L/RButtFat_skin, skirt bones of
     * skeleton mods) keep the animation's thigh pose. Each such helper takes the thigh's change instead:
     * helper' = thigh(FRIK) . thigh(anim)^-1 . helper(anim), rotations relative to the pelvis (translations kept).
     * Side from the name prefix (L / R); centre helpers are left alone.
     */
    void FRIK::flatTransferLegHelpers()
    {
        auto* tree = f4vr::getFlattenedBoneTree();
        if (!tree || !tree->transforms || static_cast<int>(_flatEngineLocal.size()) != tree->numTransforms) {
            return;
        }
        using namespace vrshim::xf;
        int thigh[2] = { -1, -1 };
        for (int i = 0; i < tree->numTransforms; ++i) {
            const char* nm = tree->transforms[i].name.c_str();
            if (!nm) continue;
            if (_stricmp(nm, "LLeg_Thigh") == 0) thigh[0] = i;
            else if (_stricmp(nm, "RLeg_Thigh") == 0) thigh[1] = i;
        }
        if (thigh[0] < 0 || thigh[1] < 0) {
            return;
        }
        M3 delta[2];
        for (int side = 0; side < 2; ++side) {
            const int t = thigh[side];
            const auto& tb = tree->transforms[t];
            delta[side] = Mul(FromNi(tb.refNode ? tb.refNode->local : tb.local).R, T(FromNi(_flatEngineLocal[t]).R));  // FRIK . anim^-1 (pelvis frame)
        }
        int n = 0;
        for (int i = 0; i < tree->numTransforms; ++i) {
            auto& b = tree->transforms[i];
            const char* nm = b.name.c_str();
            if (b.parPos < 0 || b.parPos >= tree->numTransforms || i == thigh[0] || i == thigh[1] || !nm) {
                continue;
            }
            const char* pn = tree->transforms[b.parPos].name.c_str();
            if (!pn || _stricmp(pn, "Pelvis") != 0) {
                continue;  // only node-less helpers hanging directly under the pelvis
            }
            const char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(nm[0])));
            const int  side = c0 == 'L' ? 0 : c0 == 'R' ? 1 : -1;
            if (side < 0) {
                continue;
            }
            Xf l = FromNi(_flatEngineLocal[i]);
            l.R = Mul(delta[side], l.R);
            b.local = ToNi(l);
            if (b.refNode) {  // an outfit attached a node for it: the node is what UpdateBoneArray copies; world from the pelvis now
                b.refNode->local = b.local;
                b.world = ToNi(Mul(FromNi(tree->transforms[b.parPos].world), l));
                b.refNode->world = b.world;
                f4vr::updateTransformsDown(b.refNode, false);
            }
            ++n;
        }
        _flatLegHelpers = n;
    }
#endif

#ifdef FRIK_FLAT // C37
    namespace
    {
        RE::NiTransform& flatBoneLocal(f4vr::BSFlattenedBoneTree::BoneTransforms& a_b) { return a_b.refNode ? a_b.refNode->local : a_b.local; }
        bool sameRT(const RE::NiTransform& a, const RE::NiTransform& b)
        {
            return std::memcmp(&a.rotate, &b.rotate, sizeof(a.rotate)) == 0 && std::memcmp(&a.translate, &b.translate, sizeof(a.translate)) == 0;
        }
        bool sameS(const RE::NiTransform& a, const RE::NiTransform& b) { return std::memcmp(&a.scale, &b.scale, sizeof(a.scale)) == 0; }
        bool restoreOn()
        {
            static const bool s_on = [] {
                const bool on = vrshim::bridge::iniInt("bRestoreEngineBody", 1) != 0;
                logger::info("FRIK-flat C37: leaving first person restores what FRIK left on bones the engine does not animate: {} ([FRIK] bRestoreEngineBody)",
                    on ? "on" : "OFF");
                return on;
            }();
            return s_on;
        }
    }

    /**
     * C37 (user 16 Nov: legs "stretch around" in first person and "even in an AAF animation I've seen them flop around"): the
     * flat body is animated by the engine every frame, and FRIK writes the locals of the bones it poses. A bone the engine does
     * not re-animate (a helper or root the animation graph has no track for, or the 3D root's x/y, set from the actor only when
     * it moves) keeps FRIK's last first-person write after first person ends: in AAF / SCENE (camera state 3), third person (8)
     * and every other state where FRIK steps back, the engine's animation would play on a body FRIK still partly holds. It also
     * fed C28 back on itself: C28 read "the animation's pose" at the frame start, which for such a bone is FRIK's own write of
     * the last frame, so helper' = thigh(FRIK) . thigh(anim)^-1 . helper accumulated every frame.
     * Per bone: base = the local when first person began, anim = the engine's latest pose (taken only when the local changed
     * since FRIK's last write, i.e. the engine wrote it), written = what FRIK left. C28 uses anim. On leaving first person, a
     * bone whose rotation/translation (or scale) the engine never rewrote during the whole first-person stint gets base back,
     * only while it still holds FRIK's write; the 3D root's x/y (FRIK: camera x/y) gets the actor's position back. Worlds follow
     * with the engine's next update. Off: fo4flatvr.ini [FRIK] bRestoreEngineBody=0 (C28 then reads the raw frame-start locals
     * as before, nothing restored).
     */
    void FRIK::flatTrackFrameStart()
    {
        auto* tree = f4vr::getFlattenedBoneTree();
        if (!tree || !tree->transforms || tree->numTransforms <= 0) {
            _flatEngineLocal.clear();
            return;
        }
        const int n = tree->numTransforms;
        if (!_flatTrackActive || _flatTrackTree != tree || static_cast<int>(_flatTrack.size()) != n) {
            _flatTrack.assign(n, {});
            for (int i = 0; i < n; ++i) {
                _flatTrack[i].base = _flatTrack[i].anim = flatBoneLocal(tree->transforms[i]);
            }
            _flatTrackTree = tree;
            _flatTrackActive = true;
            _flatBodyHasWritten = false;
        }
        const bool track = restoreOn();
        _flatEngineLocal.resize(n);
        for (int i = 0; i < n; ++i) {
            auto&       t = _flatTrack[i];
            const auto& cur = flatBoneLocal(tree->transforms[i]);
            if (!track || !t.hasWritten) {
                t.anim = cur;
            } else {
                if (!sameRT(cur, t.written)) {
                    t.rtAnimated = true;
                    t.anim.rotate = cur.rotate;
                    t.anim.translate = cur.translate;
                }
                if (!sameS(cur, t.written)) {
                    t.sAnimated = true;
                    t.anim.scale = cur.scale;
                }
            }
            _flatEngineLocal[i] = t.anim;
        }
    }

    void FRIK::flatTrackFrameEnd()
    {
        auto* tree = f4vr::getFlattenedBoneTree();
        if (!_flatTrackActive || !tree || tree != _flatTrackTree || static_cast<int>(_flatTrack.size()) != tree->numTransforms) {
            return;
        }
        for (int i = 0; i < tree->numTransforms; ++i) {
            _flatTrack[i].written = flatBoneLocal(tree->transforms[i]);
            _flatTrack[i].hasWritten = true;
        }
        if (const auto pl = RE::PlayerCharacter::GetSingleton(); pl && pl->loadedData && pl->loadedData->data3D) {
            _flatBodyWritten = pl->loadedData->data3D->local.translate;
            _flatBodyHasWritten = true;
        }
    }

    void FRIK::flatTrackRestore(const int a_camState)
    {
        if (!_flatTrackActive) {
            return;
        }
        _flatTrackActive = false;  // the next first-person frame takes the engine's pose afresh
        auto* tree = f4vr::getFlattenedBoneTree();
        if (!restoreOn() || !tree || tree != _flatTrackTree || static_cast<int>(_flatTrack.size()) != tree->numTransforms) {
            return;
        }
        int         nRT = 0, nS = 0, animated = 0;
        std::string names, animNames;
        for (int i = 0; i < tree->numTransforms; ++i) {
            auto& t = _flatTrack[i];
            auto& b = tree->transforms[i];
            animated += t.rtAnimated ? 1 : 0;
            if (t.rtAnimated && animNames.size() < 400) {
                animNames += std::format(" {}", b.name.c_str() ? b.name.c_str() : "?");
            }
            if (!t.hasWritten) {
                continue;
            }
            auto&      l = flatBoneLocal(b);
            const bool doRT = !t.rtAnimated && sameRT(l, t.written) && !sameRT(l, t.base);
            const bool doS = !t.sAnimated && sameS(l, t.written) && !sameS(l, t.base);
            if (!doRT && !doS) {
                continue;
            }
            if (doRT) {
                l.rotate = t.base.rotate;
                l.translate = t.base.translate;
                ++nRT;
            }
            if (doS) {
                l.scale = t.base.scale;
                ++nS;
            }
            if (b.refNode) {
                b.local = l;
            }
            if (names.size() < 400) {
                names += std::format(" {}{}", b.name.c_str() ? b.name.c_str() : "?", doRT ? (doS ? "(rt+s)" : "(rt)") : "(s)");
            }
        }
        float bodyMoved = -1.0f;
        if (const auto pl = RE::PlayerCharacter::GetSingleton(); _flatBodyHasWritten && pl && pl->loadedData && pl->loadedData->data3D) {
            auto& lt = pl->loadedData->data3D->local.translate;
            if (lt.x == _flatBodyWritten.x && lt.y == _flatBodyWritten.y) {  // not set from the actor since FRIK's last write
                bodyMoved = std::hypot(lt.x - pl->data.location.x, lt.y - pl->data.location.y);
                lt.x = pl->data.location.x;
                lt.y = pl->data.location.y;
            }
        }
        _flatBodyHasWritten = false;
        logger::info("FRIK-flat C37: camera state {}: {} of {} bones animated by the engine in first person:{} | restored rot/pos {} scale {}:{} | 3D root x/y "
                     "back to the actor: {}",
            a_camState, animated, tree->numTransforms, animNames.empty() ? " -" : animNames, nRT, nS, names.empty() ? " -" : names,
            bodyMoved < 0.0f ? std::string("not needed (set by the engine)") : std::format("{:.1f} u", bodyMoved));
    }
#endif

#ifdef FRIK_FLAT // C22
    /**
     * Flat: the third-person bone tree entries without an NiNode (twist/helper bones) are not refreshed from their parents by
     * the bone-array update after FRIK moved the body (measured 15 Nov 14:14: skin stretched into ribbons from the arms),
     * so each takes world = parent world . local here, in tree order (parents first).
     */
    void FRIK::flatPropagateNodelessBones()
    {
        auto* tree = f4vr::getFlattenedBoneTree();
        if (!tree || !tree->transforms || tree->numTransforms <= 0) {
            return;
        }
        using namespace vrshim::xf;
        float maxErr = 0.0f;
        int   maxIdx = -1, count = 0;
        for (int i = 0; i < tree->numTransforms; ++i) {
            auto& b = tree->transforms[i];
            if (b.refNode || b.parPos < 0 || b.parPos >= tree->numTransforms || b.parPos == i) {
                continue;
            }
            const Xf w = Mul(FromNi(tree->transforms[b.parPos].world), FromNi(b.local));
            const RE::NiTransform nw = ToNi(w);
            const float err = MatrixUtils::vec3Len(nw.translate - b.world.translate);
            if (err > maxErr) {
                maxErr = err, maxIdx = i;
            }
            ++count;
            b.world = nw;
        }
        _flatEndOfFrame.resize(tree->numTransforms);
        for (int i = 0; i < tree->numTransforms; ++i) {
            _flatEndOfFrame[i] = tree->transforms[i].world.translate;
        }
        _flatNodelessCount = count;
        _flatNodelessMaxErr = maxErr;
        _flatNodelessMaxName = maxIdx >= 0 ? tree->transforms[maxIdx].name.c_str() : "-";
        // diag: tree entry vs its node (what skinning may read) and the right arm chain as the tree holds it
        static auto s_last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last < std::chrono::milliseconds(2000)) {
            return;
        }
        s_last = now;
        float nodeMax = 0.0f;
        int   nodeIdx = -1, withNode = 0;
        for (int i = 0; i < tree->numTransforms; ++i) {
            const auto& b = tree->transforms[i];
            if (!b.refNode) {
                continue;
            }
            ++withNode;
            const float e = MatrixUtils::vec3Len(b.refNode->world.translate - b.world.translate);
            if (e > nodeMax) {
                nodeMax = e, nodeIdx = i;
            }
        }
        std::string chain;
        for (const char* nm : { "RArm_Collarbone", "RArm_UpperArm", "RArm_UpperTwist1", "RArm_UpperTwist2", "RArm_ForeArm1", "RArm_ForeArm2",
                 "RArm_ForeArm3", "RArm_Hand", "SPINE2", "Chest" }) {
            for (int i = 0; i < tree->numTransforms; ++i) {
                const auto& b = tree->transforms[i];
                if (b.name.c_str() && _stricmp(b.name.c_str(), nm) == 0) {
                    chain += std::format(" {}{}=({:.0f},{:.0f},{:.0f})", nm, b.refNode ? "" : "*", b.world.translate.x, b.world.translate.y,
                        b.world.translate.z);
                    break;
                }
            }
        }
        {
            auto idx = [&](const char* nm) {
                for (int i = 0; i < tree->numTransforms; ++i) {
                    if (tree->transforms[i].name.c_str() && _stricmp(tree->transforms[i].name.c_str(), nm) == 0) return i;
                }
                return -1;
            };
            const int a = idx("LLeg_Thigh"), b = idx("LLeg_Calf"), c = idx("LLeg_Foot");
            if (a >= 0 && b >= 0 && c >= 0) {
                const auto u = MatrixUtils::vec3Norm(tree->transforms[b].world.translate - tree->transforms[a].world.translate);
                const auto v = MatrixUtils::vec3Norm(tree->transforms[c].world.translate - tree->transforms[b].world.translate);
                logger::info("FRIK-flat legs (after FRIK): knee L {:.0f} deg; {} pelvis helpers follow the thighs", std::acos(std::clamp(MatrixUtils::vec3Dot(u, v), -1.0f, 1.0f)) * 57.29578f, _flatLegHelpers);
            }
        }
        logger::info("FRIK-flat tree: {} entries, {} with node, max tree-vs-node {:.1f} u ({}) | between frames (engine): max {:.1f} u ({}), "
                     "{} bones > 2 u, root moved {:.1f} u |{}",
            tree->numTransforms, withNode, nodeMax, nodeIdx >= 0 ? tree->transforms[nodeIdx].name.c_str() : "-", _flatBetweenMax, _flatBetweenName,
            _flatBetweenCount, _flatBetweenRoot, chain);
    }
#endif

    void FRIK::configureGameVars()
    {
        if (g_config.setScale) {
            logger::info("Setting VRScale from:({:.3f}) to:({:.3f})", f4vr::getIniSetting("fVrScale:VR")->GetFloat(), g_config.fVrScale);
            f4vr::getIniSetting("fVrScale:VR", true)->SetFloat(g_config.fVrScale);
        } else {
            logger::info("setScale is off; leaving the game's fVrScale:VR at {:.3f}", f4vr::getIniSetting("fVrScale:VR")->GetFloat());
        }

        f4vr::getIniSetting("fPipboyMaxScale:VRPipboy", true)->SetFloat(3.0000);
        f4vr::getIniSetting("fPipboyMinScale:VRPipboy", true)->SetFloat(0.0100f);
        f4vr::getIniSetting("fVrPowerArmorScaleMultiplier:VR", true)->SetFloat(1.0000);
#ifdef FRIK_FLAT // C25: world near plane. Flat's first-person camera clips at fNearDistance:Display (15 u = 21 cm, measured
        // 15 Nov: n=15.00); FRIK's shoulders and upper arms sit 10-20 u from the eyes, so the near plane cut the sleeves into
        // ribbons (a selfie view of the same pose was clean). fo4flatvr.ini [FRIK] fNearDistance (default 5 u = 7 cm), set
        // before the camera is built; 0 = leave the game's value.
        if (const auto* api = vrshim::bridge::api(); api && api->IniPath && vrshim::bridge::bodyEnabled()) {
            char buf[32]{};
            ::GetPrivateProfileStringA("FRIK", "fNearDistance", "5", buf, sizeof(buf), api->IniPath());
            const float nearU = std::strtof(buf, nullptr);
            if (auto* set = f4vr::getIniSetting("fNearDistance:Display"); set && nearU > 0.0f) {
                logger::info("FRIK-flat: fNearDistance:Display {:.2f} -> {:.2f} ([FRIK] fNearDistance)", set->GetFloat(), nearU);
                set->SetFloat(nearU);
            }
        }
#endif
    }

    /**
     * Apply again the config values FRIK reads only as it sets something up, when a load of the config changes them:
     * a change of the file, Mods Config FO4 VR's live preview, devbench's set. The rest are read every frame.
     */
    void FRIK::subscribeToConfigChanges()
    {
        g_config.subscribeForConfigChangedEvent("FRIK.vrScale",
            { { CONFIG_SECTION_MAIN, "setScale" }, { CONFIG_SECTION_MAIN, "fVrScale" } },
            [](const std::vector<config::ConfigKey>&) {
                configureGameVars();
            });

        g_config.subscribeForConfigChangedEvent("FRIK.pipboyModel",
            { { CONFIG_SECTION_MAIN, "HoloPipBoyEnabled" }, { CONFIG_SECTION_MAIN, "HoloPipBoyKeepWristModel" } },
            [this](const std::vector<config::ConfigKey>&) {
                if (_pipboy) {
                    _pipboy->reloadModel();
                }
            });

        g_config.subscribeForConfigChangedEvent("FRIK.pipboyScale", { { CONFIG_SECTION_MAIN, "PipboyScale" } }, [this](const std::vector<config::ConfigKey>&) {
            if (_pipboy) {
                _pipboy->applyModelScale();
            }
        });

        g_config.subscribeForConfigChangedEvent("FRIK.hideGeometry",
            { { CONFIG_SECTION_MAIN, "bHidePlayerHead" }, { CONFIG_SECTION_MAIN, "bHidePlayerSkin" } },
            [this](const std::vector<config::ConfigKey>&) {
                if (_skelly) {
                    _skelly->resetHideGeometry();
                }
            });
    }

    /**
     * If to add embedded flashlight to the game.
     */
    void FRIK::addEmbeddedFlashlightKeywordIfNeeded()
    {
        if (g_config.removeFlashlight) {
            F4SE::log::info("Flashlight disabled in config, skipping");
            return;
        }

        if (!g_frik.isFlashlightEnabled()) {
            F4SE::log::info("Flashlight disabled via API, skipping FRIK flashlight");
            return;
        }

        if (isDLLModLoaded("ImmersiveFlashlightVR")) {
            F4SE::log::info("Immersive Flashlight VR mod detected, skipping FRIK flashlight");
            return;
        }

        if (auto* armorObj = RE::TESForm::GetFormByID<RE::TESObjectARMO>(0x21B3B)) {
            if (const auto keywordObj = RE::TESForm::GetFormByID<RE::BGSKeyword>(0xB34A6)) {
                g_config.flashlightEnabled = true;
                if (!armorObj->HasKeyword(keywordObj)) {
                    logger::info("Init embedded flashlight, add keyword to: '{}', keyword: 0x{:x}", armorObj->GetFullName(), keywordObj->formID);
                    armorObj->AddKeyword(keywordObj);
                } else {
                    logger::warn("Init embedded flashlight, keyword already exists in '{}'", armorObj->GetFullName());
                }
            } else {
                logger::error("Failed to add init embedded flashlight, keyword not found");
            }
        } else {
            logger::error("Failed to init embedded flashlight, armor not found");
        }
    }

    /**
     * Send a message to the another mod in the game.
     */
    void FRIK::dispatchMessageToExternalMod(const std::string& receivingModName, const std::uint32_t messageType, void* data, const std::uint32_t dataLen) const
    {
        _messaging->Dispatch(messageType, data, dataLen, receivingModName.c_str());
    }

    /**
     * Broadcast kScopeEnter / kScopeExit when the looking-through-scope state flips, so clients stop inferring it.
     */
    void FRIK::broadcastScopeEvents()
    {
        const bool lookingThrough = isLookingThroughScope();
        if (lookingThrough == _lookingThroughScopeLastFrame) {
            return;
        }
        _lookingThroughScopeLastFrame = lookingThrough;
        logger::info("Broadcasting API lifecycle event: {}", lookingThrough ? "kScopeEnter" : "kScopeExit");
        broadcastMessage(static_cast<std::uint32_t>(lookingThrough ? api::FRIKApiV2::LifecycleEvent::kScopeEnter : api::FRIKApiV2::LifecycleEvent::kScopeExit), nullptr, 0);
    }

    /**
     * Broadcast a skeleton lifecycle event with its payload; the message is delivered synchronously so a stack struct is fine.
     */
    void FRIK::broadcastSkeletonLifecycle(const std::uint32_t messageType) const
    {
        api::core::SkeletonLifecycleData data{};
        data.structSize = sizeof(data);
        data.generation = _skeletonGeneration;
        data.rootNode = _workingRootNode;
        data.inPowerArmor = _inPowerArmor;
        broadcastMessage(messageType, &data, sizeof(data));
    }

    void FRIK::broadcastMessage(const std::uint32_t messageType, void* data, const std::uint32_t dataLen) const
    {
        _messaging->Dispatch(messageType, data, dataLen, nullptr);
    }

    void FRIK::onBetterScopesMessage(F4SE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }

        if (msg->type == 15) {
            // BetterScopesVR sends the bool as the pointer value itself, not as pointed-to data
            const bool lookingThroughScope = msg->data != nullptr;
            logger::info("BetterScopesVR looking through scopes: {}", lookingThroughScope);
            g_scopeAuthority.setLookingThroughScope(BETTER_SCOPES_VR_MOD_NAME, lookingThroughScope);
        }
    }

    /**
     * Dump game data if requested in the "aDumpDataOnce" names of the config.
     */
    void FRIK::checkDebugDump() const
    {
        ModBase::checkDebugDump();

        if (g_config.checkDebugDumpDataOnceFor("menus")) {
            _gameMenusHandler.debugDumpAllMenus();
        }
        if (g_config.checkDebugDumpDataOnceFor("weapon_muzzle")) {
            if (const auto muzzle = getMuzzleFlashNodes()) {
                f4vr::DebugDump::printNodes(muzzle->fireNode);
                f4vr::DebugDump::printNodes(muzzle->projectileNode);
            }
        }
    }
}
