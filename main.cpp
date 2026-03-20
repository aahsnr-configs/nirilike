#define WLR_USE_UNSTABLE

#include <unistd.h>

// pixman headers live at /usr/include/pixman-1/ on Arch Linux — the subdirectory
// is not in the default compiler search path.  Using the full path here makes
// the include work unconditionally without relying on -I/usr/include/pixman-1
// being threaded through from pkg-config.  pixman_region32_t is used directly
// in the addDamage hook typedef and implementation below.
#include <pixman-1/pixman.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Monitor.hpp>

#include "globals.hpp"
#include "overview.hpp"
#include "globals.hpp"
#include "overview.hpp"

// ────────────────────────────────────────────────────────────────────────────
//  Hook state
// ────────────────────────────────────────────────────────────────────────────

inline CFunctionHook* g_pRenderWorkspaceHook = nullptr;
inline CFunctionHook* g_pAddDamageHookA      = nullptr; // CBox overload
inline CFunctionHook* g_pAddDamageHookB      = nullptr; // pixman_region32_t* overload

// ABI signatures match what Hyprland actually calls at runtime.
// renderWorkspace is private so the vtable call uses timespec*, not steady_tp.
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, timespec*, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);

static bool g_unloading = false;

// ────────────────────────────────────────────────────────────────────────────
//  Required export — version handshake
// ────────────────────────────────────────────────────────────────────────────

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Prevents re-entrant overview creation during our own render calls.
static bool renderingOverview = false;

// ────────────────────────────────────────────────────────────────────────────
//  Hook implementations
// ────────────────────────────────────────────────────────────────────────────

static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace,
                               timespec* now, const CBox& geometry) {
    // If there is no active overview, or it belongs to a different monitor,
    // or we are already inside our rendering path — fall through to Hyprland.
    if (!g_pNiriOverview || renderingOverview ||
        g_pNiriOverview->blockOverviewRendering ||
        g_pNiriOverview->pMonitor != pMonitor) {
        ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(
            thisptr, pMonitor, pWorkspace, now, geometry);
    } else {
        g_pNiriOverview->render();
    }
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = (CMonitor*)thisptr;
    if (!g_pNiriOverview ||
        g_pNiriOverview->pMonitor != PMONITOR->m_self ||
        g_pNiriOverview->blockDamageReporting) {
        ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
        return;
    }
    g_pNiriOverview->onDamageReported();
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = (CMonitor*)thisptr;
    if (!g_pNiriOverview ||
        g_pNiriOverview->pMonitor != PMONITOR->m_self ||
        g_pNiriOverview->blockDamageReporting) {
        ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
        return;
    }
    g_pNiriOverview->onDamageReported();
}

// ────────────────────────────────────────────────────────────────────────────
//  Dispatcher:  nirilike  args: toggle (default) | open | close
// ────────────────────────────────────────────────────────────────────────────

static SDispatchResult onNiriDispatcher(std::string arg) {
    if (arg == "close" || arg == "off") {
        if (g_pNiriOverview)
            g_pNiriOverview->close();
        return {};
    }

    if (arg == "open") {
        if (!g_pNiriOverview) {
            renderingOverview = true;
            g_pNiriOverview   = std::make_unique<COverview>(
                Desktop::focusState()->monitor()->m_activeWorkspace);
            renderingOverview = false;
        }
        return {};
    }

    // "toggle" or empty string (e.g. hyprctl dispatch nirilike) — both toggle.
    if (g_pNiriOverview) {
        g_pNiriOverview->close();
    } else {
        renderingOverview = true;
        g_pNiriOverview   = std::make_unique<COverview>(
            Desktop::focusState()->monitor()->m_activeWorkspace);
        renderingOverview = false;
    }
    return {};
}

// ────────────────────────────────────────────────────────────────────────────
//  Helpers
// ────────────────────────────────────────────────────────────────────────────

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(PHANDLE,
        "[nirilike] Initialisation failed: " + reason,
        CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

// ────────────────────────────────────────────────────────────────────────────
//  PLUGIN_INIT
// ────────────────────────────────────────────────────────────────────────────

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Version handshake — bail out if headers don't match the running build.
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        failNotif("header/runtime version mismatch");
        throw std::runtime_error("[nirilike] Version mismatch");
    }

    // ── Hook renderWorkspace ─────────────────────────────────────────────
    // findFunctionsByName is a SUBSTRING match: "renderWorkspace" also
    // matches renderWorkspaceWindows and renderWorkspaceWindowsFullscreen.
    // Those short helpers may lack enough bytes for the 14-byte x86_64
    // trampoline, causing hook() to silently return false.
    // Filter on the demangled name to get exactly CHyprRenderer::renderWorkspace.
    auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
    if (FNS.empty()) {
        failNotif("symbol not found: renderWorkspace");
        throw std::runtime_error("[nirilike] renderWorkspace not found");
    }
    {
        const SFunctionMatch* match = nullptr;
        for (const auto& fn : FNS) {
            // Accept the first entry whose demangled name is exactly
            // CHyprRenderer::renderWorkspace (not the ...Windows variants).
            if (fn.demangled.find("CHyprRenderer::renderWorkspace(") != std::string::npos) {
                match = &fn;
                break;
            }
        }
        if (!match) {
            std::string sigs;
            for (const auto& fn : FNS)
                sigs += "  " + fn.demangled + "\n";
            failNotif("renderWorkspace: no matching overload found.\nCandidates:\n" + sigs);
            throw std::runtime_error("[nirilike] renderWorkspace overload not found");
        }
        g_pRenderWorkspaceHook = HyprlandAPI::createFunctionHook(
            PHANDLE, match->address, (void*)hkRenderWorkspace);
    }

    // ── Hook addDamage(const pixman_region32_t*) ─────────────────────────
    // Search by the demangled class + argument type so the overload
    // resolution is stable across fork variants.
    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "addDamage");
    if (FNS.empty()) {
        failNotif("symbol not found: CMonitor::addDamage");
        throw std::runtime_error("[nirilike] addDamage not found");
    }
    {
        const SFunctionMatch* matchB = nullptr; // pixman overload
        const SFunctionMatch* matchA = nullptr; // CBox overload
        for (const auto& fn : FNS) {
            const auto& d = fn.demangled;
            if (d.find("CMonitor::addDamage") == std::string::npos)
                continue;
            if (d.find("pixman") != std::string::npos && !matchB)
                matchB = &fn;
            else if (d.find("CBox") != std::string::npos && !matchA)
                matchA = &fn;
        }

        if (!matchB) {
            std::string sigs;
            for (const auto& fn : FNS)
                sigs += "  " + fn.demangled + "\n";
            failNotif("addDamage(pixman): no matching overload.\nCandidates:\n" + sigs);
            throw std::runtime_error("[nirilike] addDamage(pixman) not found");
        }
        if (!matchA) {
            std::string sigs;
            for (const auto& fn : FNS)
                sigs += "  " + fn.demangled + "\n";
            failNotif("addDamage(CBox): no matching overload.\nCandidates:\n" + sigs);
            throw std::runtime_error("[nirilike] addDamage(CBox) not found");
        }

        g_pAddDamageHookB = HyprlandAPI::createFunctionHook(
            PHANDLE, matchB->address, (void*)hkAddDamageB);
        g_pAddDamageHookA = HyprlandAPI::createFunctionHook(
            PHANDLE, matchA->address, (void*)hkAddDamageA);
    }

    // ── Activate hooks — report each failure individually ─────────────────
    if (!g_pRenderWorkspaceHook->hook()) {
        failNotif("hook() failed: renderWorkspace — function may be too short for trampoline");
        throw std::runtime_error("[nirilike] hook() failed: renderWorkspace");
    }
    if (!g_pAddDamageHookA->hook()) {
        failNotif("hook() failed: addDamage(CBox) — function may be too short for trampoline");
        throw std::runtime_error("[nirilike] hook() failed: addDamage(CBox)");
    }
    if (!g_pAddDamageHookB->hook()) {
        failNotif("hook() failed: addDamage(pixman) — function may be too short for trampoline");
        throw std::runtime_error("[nirilike] hook() failed: addDamage(pixman)");
    }

    // ── Pre-render callback — refresh dirty FBs before each frame ────────
    static auto preRenderListener = Event::bus()->m_events.render.pre.listen(
        [](PHLMONITOR) {
            if (g_pNiriOverview)
                g_pNiriOverview->onPreRender();
        });

    // ── Register dispatcher ───────────────────────────────────────────────
    HyprlandAPI::addDispatcherV2(PHANDLE, "nirilike", ::onNiriDispatcher);

    // ── Register config values ────────────────────────────────────────────
    // All values live under plugin:nirilike:* as required.
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:nirilike:gap_size",
                                Hyprlang::INT{8});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:nirilike:side_margin",
                                Hyprlang::INT{60});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:nirilike:bg_col",
                                Hyprlang::INT{(long long)0xFF141414});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:nirilike:border_col",
                                Hyprlang::INT{(long long)0xFF6699FF});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:nirilike:border_size",
                                Hyprlang::INT{3});

    HyprlandAPI::reloadConfig();

    return {"nirilike",
            "Niri-style vertical strip workspace overview",
            "nirilike",
            "1.0"};
}

// ────────────────────────────────────────────────────────────────────────────
//  PLUGIN_EXIT
// ────────────────────────────────────────────────────────────────────────────

APICALL EXPORT void PLUGIN_EXIT() {
    // Remove any live pass elements from the render pipeline.
    g_pHyprRenderer->m_renderPass.removeAllOfType("CNiriPassElement");

    // Immediately destroy any open overview without the deferred path
    // (doLater won't run after the plugin has been unloaded).
    g_pNiriOverview.reset();

    g_unloading = true;
}
