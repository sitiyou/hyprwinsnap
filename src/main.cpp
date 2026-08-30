#include <src/plugins/PluginAPI.hpp>

// The private->public include trick below would corrupt any STL header that
// happens to be pulled in while it is active, so hoist the common offenders in
// advance (see wiki: Plugins/Development/Advanced).
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Access to the in-flight drag state (grabbed corner, size at drag start).
// Must be pulled in before LayoutManager.hpp, which includes DragController.hpp itself.
#define private public
#include <src/layout/supplementary/DragController.hpp>
#undef private

#include <src/debug/log/Logger.hpp>
#include <src/helpers/math/Math.hpp>
#include <src/helpers/time/Time.hpp>
#include <src/layout/LayoutManager.hpp>
#include <src/output/Monitor.hpp>
#include <src/desktop/DesktopTypes.hpp>
#include <src/desktop/view/View.hpp>
#include <src/desktop/view/Window.hpp>
#include <src/config/values/types/BoolValue.hpp>
#include <src/config/values/types/IntValue.hpp>
#include <src/config/values/types/StringValue.hpp>

using namespace Layout;

// ---------------------------------------------------------------------------
// plugin identity / handle
// ---------------------------------------------------------------------------

inline HANDLE PHANDLE = nullptr;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// ---------------------------------------------------------------------------
// config values (V2 API, all live under `plugin:hyprwinsnap:`)
// ---------------------------------------------------------------------------

inline SP<Config::Values::CBoolValue>   cfgEnabled;
inline SP<Config::Values::CIntValue>    cfgThreshold;
inline SP<Config::Values::CIntValue>    cfgHysteresis;
inline SP<Config::Values::CStringValue> cfgPresets;
inline SP<Config::Values::CBoolValue>   cfgDebug;

// ---------------------------------------------------------------------------
// preset size list
// ---------------------------------------------------------------------------

struct SPreset {
    double w = 0;
    double h = 0;
};

inline std::vector<SPreset> g_presetCache;
inline std::string          g_presetCacheKey;

static double parseDim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
        sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
        sv.remove_suffix(1);

    double          val = 0;
    const auto      res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return res.ec == std::errc() ? val : 0;
}

static std::vector<SPreset> parsePresets(std::string_view raw) {
    // hyprlang keeps quotes verbatim when the user quoted the value
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        raw = raw.substr(1, raw.size() - 2);

    std::vector<SPreset> presets;

    size_t start = 0;
    while (start <= raw.size()) {
        const auto sep = raw.find_first_of(",;", start);
        const auto item =
            raw.substr(start, sep == std::string_view::npos ? raw.size() - start : sep - start);
        start = sep == std::string_view::npos ? raw.size() + 1 : sep + 1;

        size_t b = 0, e = item.size();
        while (b < e && std::isspace(static_cast<unsigned char>(item[b])))
            b++;
        while (e > b && std::isspace(static_cast<unsigned char>(item[e - 1])))
            e--;

        if (b == e)
            continue;

        // tolerate quotes around an individual entry as well
        if (e - b >= 2 && item[b] == '"' && item[e - 1] == '"') {
            b++;
            e--;
        }

        const auto xSep = item.find_first_of("xX", b);
        if (xSep == std::string_view::npos || xSep == e - 1) {
            Log::logger->log(Log::WARN, "hyprwinsnap: ignoring malformed preset '{}' (expected WxH)",
                             item.substr(b, e - b));
            continue;
        }

        const double w = parseDim(item.substr(b, xSep - b));
        const double h = parseDim(item.substr(xSep + 1, e - xSep - 1));

        if (w <= 0 || h <= 0) {
            Log::logger->log(Log::WARN, "hyprwinsnap: ignoring malformed preset '{}' (expected positive WxH)",
                             item.substr(b, e - b));
            continue;
        }

        presets.push_back({w, h});
    }

    return presets;
}

// ---------------------------------------------------------------------------
// snapping state
// ---------------------------------------------------------------------------

// engaged preset state; hysteresis makes the snap "sticky"
struct SSnapping {
    bool    engaged = false;
    SPreset preset;

    // drag target the current snap session runs on (identity only, not owned)
    Layout::ITarget* dragTarget = nullptr;
};
inline SSnapping g_snapping;

static const SPreset* nearestInRange(const CBox& box, double range, eMouseBindMode mode, const Vector2D& beginSize) {
    const SPreset* best     = nullptr;
    double         bestDist = range;
    for (const auto& p : g_presetCache) {
        const double dw = std::abs(box.w - p.w);
        const double dh = std::abs(box.h - p.h);
        if (dw > range || dh > range)
            continue;

        // `keepaspectratio` rules force MBIND_RESIZE_FORCE_RATIO; only consider
        // presets that keep that ratio so we don't fight the ratio lock.
        if (mode == MBIND_RESIZE_FORCE_RATIO) {
            if (beginSize.x < 1 || beginSize.y < 1)
                continue;
            if (std::abs(p.h / p.w - beginSize.y / beginSize.x) > 1e-3)
                continue;
        }

        const double dist = std::max(dw, dh);
        if (!best || dist < bestDist) {
            best     = &p;
            bestDist = dist;
        }
    }
    return best;
}

static void applySizeSnap(CBox& box, eRectCorner corner, eMouseBindMode mode, const Vector2D& beginSize) {
    const std::string raw = cfgPresets->value();
    if (raw != g_presetCacheKey) {
        g_presetCacheKey = raw;
        g_presetCache    = parsePresets(raw);
    }

    const double thresh = cfgThreshold->value();
    if (thresh <= 0 || g_presetCache.empty()) {
        g_snapping.engaged = false;
        return;
    }

    // hysteresis >= threshold: once engaged, keep snapping until the window
    // drifts past the (bigger) release distance -> no boundary flicker
    const double hyst = std::max(thresh, static_cast<double>(std::max(INT64_C(0), cfgHysteresis->value())));

    const CBox old = box;

    const SPreset* cand = nearestInRange(box, thresh, mode, beginSize);
    if (cand) {
        g_snapping.engaged = true;
        g_snapping.preset  = *cand;
    } else if (g_snapping.engaged &&
               (std::abs(box.w - g_snapping.preset.w) > hyst || std::abs(box.h - g_snapping.preset.h) > hyst))
        g_snapping.engaged = false;

    if (!g_snapping.engaged)
        return;

    box.w = g_snapping.preset.w;
    box.h = g_snapping.preset.h;

    // anchor the edges that are not being dragged, mirroring the core snap
    if (edgeLeft(corner))
        box.x = old.x + old.w - box.w;
    else if (edgeRight(corner))
        box.x = old.x;

    if (edgeTop(corner))
        box.y = old.y + old.h - box.h;
    else if (edgeBottom(corner))
        box.y = old.y;

    if (cfgDebug->value())
        Log::logger->log(Log::DEBUG, "hyprwinsnap: snapped {}x{} -> {}x{}", old.w, old.h, box.w, box.h);
}

// ---------------------------------------------------------------------------
// function hook
// ---------------------------------------------------------------------------

inline CFunctionHook* g_pSetPosHook = nullptr;

// matches Layout::ITarget::setPositionGlobal(Hyprutils::Math::CBox const&, unsigned char)
using origSetPositionGlobal = void (*)(Layout::ITarget*, const CBox&, uint8_t);

void hkSetPositionGlobal(Layout::ITarget* thisptr, const CBox& box, uint8_t flags) {
    const bool canSnap =
        g_layoutManager && cfgEnabled && cfgEnabled->value() && cfgThreshold && cfgHysteresis && cfgPresets && cfgDebug;

    if (canSnap) {
        const auto& DC   = g_layoutManager->dragController();
        const auto  mode = DC->mode();
        const bool  resizing =
            mode == MBIND_RESIZE || mode == MBIND_RESIZE_FORCE_RATIO || mode == MBIND_RESIZE_BLOCK_RATIO;

        if (resizing) {
            const auto dragTarget = DC->target();
            if (dragTarget.get() == thisptr && dragTarget->floating() && DC->m_grabbedCorner != CORNER_NONE) {
                const auto window = dragTarget->window();
                if (Desktop::View::validMapped(window)) {
                    // hysteresis stickiness is per drag session: a resize drag on a
                    // different window must not inherit the previous drag's engagement
                    if (g_snapping.dragTarget != dragTarget.get()) {
                        g_snapping.dragTarget = dragTarget.get();
                        g_snapping.engaged    = false;
                    }

                    CBox modBox = box;
                    applySizeSnap(modBox, DC->m_grabbedCorner, mode, DC->m_beginDragSizeXY);

                    (*(origSetPositionGlobal)g_pSetPosHook->m_original)(thisptr, modBox, flags);
                    return;
                }
            }
        }
    }

    (*(origSetPositionGlobal)g_pSetPosHook->m_original)(thisptr, box, flags);
}

// ---------------------------------------------------------------------------
// plugin init / exit
// ---------------------------------------------------------------------------

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();

    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinsnap] Mismatched headers! Cannot proceed.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprwinsnap] Version mismatch");
    }

    cfgEnabled = makeShared<Config::Values::CBoolValue>("plugin:hyprwinsnap:enabled", "Master switch for size snapping",
                                                        true);
    cfgThreshold = makeShared<Config::Values::CIntValue>(
        "plugin:hyprwinsnap:threshold", "Max per-axis size distance in logical px to engage a snap", 24,
        Config::Values::SIntValueOptions{.min = 0});
    cfgHysteresis = makeShared<Config::Values::CIntValue>(
        "plugin:hyprwinsnap:hysteresis", "Release distance in logical px (>= threshold); how 'sticky' the snap is", 32,
        Config::Values::SIntValueOptions{.min = 0});
    cfgPresets = makeShared<Config::Values::CStringValue>("plugin:hyprwinsnap:presets",
                                                          "Comma/semicolon separated preset sizes in WxH",
                                                          "800x600,1024x768,1280x720");
    cfgDebug = makeShared<Config::Values::CBoolValue>("plugin:hyprwinsnap:debug", "Log every snap to the Hyprland log",
                                                      false);

    const auto registered = HyprlandAPI::addConfigValueV2(PHANDLE, cfgEnabled) &&
                            HyprlandAPI::addConfigValueV2(PHANDLE, cfgThreshold) &&
                            HyprlandAPI::addConfigValueV2(PHANDLE, cfgHysteresis) &&
                            HyprlandAPI::addConfigValueV2(PHANDLE, cfgPresets) &&
                            HyprlandAPI::addConfigValueV2(PHANDLE, cfgDebug);
    if (!registered)
        Log::logger->log(Log::ERR, "hyprwinsnap: failed to register config values");

    static const auto methods = HyprlandAPI::findFunctionsByName(PHANDLE, "setPositionGlobal");
    for (const auto& m : methods) {
        // pick the non-virtual CBox overload (by-value-arg matches are filtered
        // out through the STargetBox demangled signatures)
        if (!m.demangled.contains("Layout::ITarget::setPositionGlobal") || !m.demangled.contains("CBox"))
            continue;

        g_pSetPosHook =
            HyprlandAPI::createFunctionHook(handle, m.address, reinterpret_cast<void*>(&hkSetPositionGlobal));
        if (g_pSetPosHook && g_pSetPosHook->hook()) {
            Log::logger->log(Log::INFO, "hyprwinsnap: hooked ITarget::setPositionGlobal");
            break;
        }

        Log::logger->log(Log::ERR, "hyprwinsnap: failed to hook setPositionGlobal");
        break;
    }

    return {"hyprwinsnap", "Snap floating window sizes to configurable presets while resizing", "quy", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // hooks are cleaned up by Hyprland
}