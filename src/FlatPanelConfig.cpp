// FRIK-flat (20 Nov): FRIK.toml through fo4flatvr's VR settings panel. See FlatPanelConfig.h.
#include "FlatPanelConfig.h"

#include "Config.h"
#include "vrshim/Bridge.h"
#include "fo4flatvr/FrikBridgeApi.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace frik::flatpanel
{
    namespace
    {
        // the owner of the panel's session overrides (Mods Config FO4 VR's are "ModsConfigFO4VR", devbench / API ones their own)
        constexpr auto OWNER = "fo4flatvr VR panel";

        enum class Op : int
        {
            kPreview,
            kDiscard,
            kSave
        };
        struct Req
        {
            Op          op;
            std::string sec, key, val;
        };

        std::mutex               g_lock;
        std::vector<Req>         g_queue;  // under g_lock
        std::string              g_last;   // under g_lock: last result line
        std::atomic<std::uint32_t> g_previews{ 0 }, g_discards{ 0 }, g_saves{ 0 }, g_refused{ 0 }, g_drains{ 0 };

        void copyOut(const std::string& a_s, char* a_out, std::uint32_t a_cap)
        {
            if (!a_out || a_cap == 0) return;
            const std::size_t n = (std::min)(a_s.size(), static_cast<std::size_t>(a_cap - 1));
            std::memcpy(a_out, a_s.data(), n);
            a_out[n] = 0;
        }

        std::uint8_t apiGet(const char* a_sec, const char* a_key, char* a_out, std::uint32_t a_cap)
        {
            if (!a_sec || !a_key) {
                copyOut("", a_out, a_cap);
                return 0;
            }
            try {
                static constexpr const char* kNone = "\x01<none>";
                const std::string v = g_config.getConfigValue(a_sec, a_key, kNone);
                if (v == kNone) {
                    copyOut("", a_out, a_cap);
                    return 0;
                }
                copyOut(v, a_out, a_cap);
                return 1;
            } catch (...) {
                copyOut("", a_out, a_cap);
                return 0;
            }
        }

        std::uint8_t enqueue(const Op a_op, const char* a_sec, const char* a_key, const char* a_val)
        {
            if (!a_sec || !a_key || !*a_sec || !*a_key) return 0;
            std::scoped_lock l(g_lock);
            if (g_queue.size() > 4096) return 0;
            g_queue.push_back({ a_op, a_sec, a_key, a_val ? a_val : "" });
            return 1;
        }
        std::uint8_t apiPreview(const char* a_sec, const char* a_key, const char* a_val) { return enqueue(Op::kPreview, a_sec, a_key, a_val); }
        std::uint8_t apiDiscard(const char* a_sec, const char* a_key) { return enqueue(Op::kDiscard, a_sec, a_key, nullptr); }
        std::uint8_t apiSave(const char* a_sec, const char* a_key, const char* a_val) { return enqueue(Op::kSave, a_sec, a_key, a_val); }

        void apiStatus(char* a_out, const std::uint32_t a_cap)
        {
            std::string line;
            {
                std::scoped_lock l(g_lock);
                line = std::format("frikcfg file='{}' queued={} drains={} previews={} discards={} saves={} refused={} last='{}'",
                    g_config.getConfigFilePath(), g_queue.size(), g_drains.load(), g_previews.load(), g_discards.load(), g_saves.load(),
                    g_refused.load(), g_last);
            }
            copyOut(line, a_out, a_cap);
        }

        void apiConfigFile(char* a_out, const std::uint32_t a_cap) { copyOut(g_config.getConfigFilePath(), a_out, a_cap); }

        const FO4FlatVR_FrikConfigApi kApi{ sizeof(FO4FlatVR_FrikConfigApi), 0, &apiGet, &apiPreview, &apiDiscard, &apiSave, &apiStatus,
            &apiConfigFile };

        void setLast(std::string a_s)
        {
            std::scoped_lock l(g_lock);
            g_last = std::move(a_s);
        }
    }

    void registerWithBridge()
    {
        const bool ok = vrshim::bridge::setConfigApi(&kApi);
        logger::info("FRIK-flat: FRIK.toml pages of fo4flatvr's VR settings panel: {} (config '{}')",
            ok ? "config table registered (bridge v6)" : "not available (fo4flatvr bridge older than v6)", g_config.getConfigFilePath());
    }

    void drain()
    {
        std::vector<Req> q;
        {
            std::scoped_lock l(g_lock);
            if (g_queue.empty()) return;
            q.swap(g_queue);
        }
        ++g_drains;
        try {
            // the last request per key decides (a save also ends the preview): coalesce a slider drag into one value
            std::map<std::pair<std::string, std::string>, Req> last;
            for (auto& r : q) {
                last[{ r.sec, r.key }] = std::move(r);
            }
            std::vector<config::ConfigEntry> saves;
            std::vector<config::ConfigKey>   clears;
            std::vector<config::ConfigEntry> previews;
            for (auto& [k, r] : last) {
                switch (r.op) {
                case Op::kSave:
                    saves.push_back({ r.sec, r.key, config::ConfigValue(r.val) });
                    clears.push_back({ r.sec, r.key });
                    break;
                case Op::kDiscard:
                    clears.push_back({ r.sec, r.key });
                    break;
                case Op::kPreview:
                    previews.push_back({ r.sec, r.key, config::ConfigValue(r.val) });
                    break;
                }
            }
            std::string res;
            if (!saves.empty()) {
                g_config.saveConfigValues(std::span<const config::ConfigEntry>(saves.data(), saves.size()));  // FRIK's own save
                g_saves += static_cast<std::uint32_t>(saves.size());
                res += std::format("saved {} to {}", saves.size(), g_config.getConfigFilePath());
            }
            if (!clears.empty()) {
                // reload=true reads the file: saved values apply at once (before FRIK's file watch reloads them too)
                const auto n = g_config.clearConfigOverrides(OWNER, clears, true);
                g_discards += static_cast<std::uint32_t>(clears.size() - saves.size());
                res += std::format("{}cleared {} preview(s)", res.empty() ? "" : "; ", n);
                if (n == 0 && !saves.empty()) {
                    g_config.reload();  // nothing was previewed: the saved values still apply now
                }
            }
            if (!previews.empty()) {
                // one at a time (a refused value is logged with why and keeps the old one), one reload at the end
                int ok = 0, bad = 0;
                std::string refused;
                for (const auto& e : previews) {
                    if (g_config.setConfigOverrides(OWNER, { e }, false)) {
                        ++ok;
                    } else {
                        ++bad;
                        refused += std::format(" {}.{}={}", e.section, e.key, e.value.toString());
                    }
                }
                if (ok) {
                    g_config.reload();
                }
                g_previews += ok;
                g_refused += bad;
                res += std::format("{}previewed {}{}", res.empty() ? "" : "; ", ok, bad ? std::format(", refused {}:{}", bad, refused) : "");
            }
            logger::sample(250, "FRIK-flat panel: {}", res);
            setLast(res);
        } catch (const std::exception& e) {
            logger::warn("FRIK-flat panel: config request failed: {}", e.what());
            setLast(std::format("error: {}", e.what()));
        } catch (...) {
            logger::warn("FRIK-flat panel: config request failed");
            setLast("error");
        }
    }
}
