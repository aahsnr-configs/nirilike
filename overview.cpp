#include "overview.hpp"
#include "globals.hpp"  // PHANDLE — used in HyprlandAPI::getConfigValue calls

// ── #define private public lets us call CHyprRenderer::renderWorkspace()
//    (declared private) and use CHyprOpenGLImpl internals.
//    Must come BEFORE every Hyprland header in this TU.
#define private public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#undef private

#include "NiriPassElement.hpp"

// ────────────────────────────────────────────────────────────────────────────
//  Helpers
// ────────────────────────────────────────────────────────────────────────────

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

// Animation update callback — just asks for a new frame.
static void niriDamageCallback(WP<Hyprutils::Animation::CBaseAnimatedVariable>) {
    if (g_pNiriOverview)
        g_pNiriOverview->damage();
}

// ────────────────────────────────────────────────────────────────────────────
//  Destructor
// ────────────────────────────────────────────────────────────────────────────

COverview::~COverview() {
    // FBs must be released while the EGL context is current.
    g_pHyprRenderer->makeEGLCurrent();
    m_strips.clear();
    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_UNKNOWN);
    if (const auto MON = pMonitor.lock())
        g_pHyprOpenGL->markBlurDirtyForMonitor(MON);
}

// ────────────────────────────────────────────────────────────────────────────
//  Constructor
// ────────────────────────────────────────────────────────────────────────────

COverview::COverview(PHLWORKSPACE startedOn_, bool /*swipe*/) : m_startedOn(startedOn_) {
    const auto PMONITOR = Desktop::focusState()->monitor();
    pMonitor = PMONITOR;

    // ── Read config ───────────────────────────────────────────────────────
    static auto* const* PGAP    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:nirilike:gap_size")->getDataStaticPtr();
    static auto* const* PMARGIN = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:nirilike:side_margin")->getDataStaticPtr();
    static auto* const* PBGCOL  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:nirilike:bg_col")->getDataStaticPtr();
    static auto* const* PBDRCOL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:nirilike:border_col")->getDataStaticPtr();
    static auto* const* PBDRSZ  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:nirilike:border_size")->getDataStaticPtr();

    m_gapSize    = (int)**PGAP;
    m_sideMargin = (int)**PMARGIN;
    m_bgColor    = **PBGCOL;
    m_borderCol  = **PBDRCOL;
    m_borderSize = (int)**PBDRSZ;

    // ── Enumerate workspaces on this monitor, sorted by ID ────────────────
    std::vector<PHLWORKSPACE> wsOnMon;
    for (auto& ws : g_pCompositor->getWorkspacesCopy()) {
        if (!ws || ws->inert() || ws->m_isSpecialWorkspace)
            continue;
        if (ws->m_monitor.lock() == PMONITOR)
            wsOnMon.push_back(ws);
    }

    std::sort(wsOnMon.begin(), wsOnMon.end(), [](const PHLWORKSPACE& a, const PHLWORKSPACE& b) {
        return a->m_id < b->m_id;
    });

    if (wsOnMon.empty())
        wsOnMon.push_back(startedOn_);

    // Find index of the workspace we opened on.
    m_activeIdx  = 0;
    m_closeToIdx = 0;
    for (int i = 0; i < (int)wsOnMon.size(); ++i) {
        if (wsOnMon[i] == startedOn_) {
            m_activeIdx  = i;
            m_closeToIdx = i;
            break;
        }
    }

    const int N = (int)wsOnMon.size();

    // ── Compute layout in LOGICAL pixels ─────────────────────────────────
    //   Strip width fills the monitor minus two side margins.
    //   Strip height: divide available vertical space evenly; clamp to 60 px.
    const float monW   = PMONITOR->m_size.x;
    const float monH   = PMONITOR->m_size.y;
    const float gap    = (float)m_gapSize;
    const float sideM  = (float)m_sideMargin;
    const float vertM  = (float)m_vertMargin;
    const float stripW = monW - 2.0f * sideM;
    const float avail  = monH - gap * (float)(N - 1) - 2.0f * vertM;

    m_stripH = (avail / (float)N > 60.0f) ? avail / (float)N : 60.0f;

    const float totalH = (float)N * m_stripH + (float)(N - 1) * gap + 2.0f * vertM;

    // Scroll so that the active strip is vertically centred on screen.
    if (totalH <= monH) {
        m_scrollOffset = (monH - totalH) / 2.0f;
    } else {
        const float activeCY = vertM + (float)m_activeIdx * (m_stripH + gap) + m_stripH * 0.5f;
        m_scrollOffset = monH * 0.5f - activeCY;
        const float lo = monH - totalH;
        if (m_scrollOffset < lo) m_scrollOffset = lo;
        if (m_scrollOffset > 0.0f) m_scrollOffset = 0.0f;
    }

    // Build strip metadata.
    m_strips.resize(N);
    for (int i = 0; i < N; ++i) {
        m_strips[i].workspaceID = wsOnMon[i]->m_id;
        m_strips[i].pWorkspace  = wsOnMon[i];
        m_strips[i].overviewBox = {sideM, vertM + (float)i * (m_stripH + gap), stripW, m_stripH};
    }

    // ── Render each workspace into its own offscreen framebuffer ──────────
    g_pHyprRenderer->makeEGLCurrent();

    PHLWORKSPACE openSpecial = PMONITOR->m_activeSpecialWorkspace;
    if (openSpecial)
        PMONITOR->m_activeSpecialWorkspace.reset();

    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    m_startedOn->m_visible                    = false;

    // m_pixelSize members are already double (Vector2D), so pass them directly
    // to CBox (which takes double) — no conversion in the braced-initializer.
    const CBox renderBox{0.0, 0.0, PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y};

    for (int i = 0; i < N; ++i) {
        auto& strip = m_strips[i];
        // alloc() takes int; cast explicitly rather than relying on implicit double→int.
        strip.fb.alloc((int)PMONITOR->m_pixelSize.x, (int)PMONITOR->m_pixelSize.y,
                       PMONITOR->m_output->state->state().drmFormat);

        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &strip.fb);
        g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 1.0});

        if (strip.pWorkspace) {
            PMONITOR->m_activeWorkspace = strip.pWorkspace;

            if (strip.pWorkspace == m_startedOn)
                PMONITOR->m_activeSpecialWorkspace = openSpecial;

            g_pDesktopAnimationManager->startAnimation(
                strip.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
            strip.pWorkspace->m_visible = true;

            g_pHyprRenderer->renderWorkspace(PMONITOR, strip.pWorkspace,
                                              Time::steadyNow(), renderBox);

            strip.pWorkspace->m_visible = false;
            g_pDesktopAnimationManager->startAnimation(
                strip.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);

            if (strip.pWorkspace == m_startedOn)
                PMONITOR->m_activeSpecialWorkspace.reset();
        } else {
            // Empty / placeholder workspace — clear is enough.
            g_pHyprRenderer->renderWorkspace(PMONITOR, strip.pWorkspace,
                                              Time::steadyNow(), renderBox);
        }

        g_pHyprOpenGL->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
    }

    // Restore monitor state.
    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
    PMONITOR->m_activeSpecialWorkspace        = openSpecial;
    PMONITOR->m_activeWorkspace               = m_startedOn;
    m_startedOn->m_visible                    = true;
    g_pDesktopAnimationManager->startAnimation(
        m_startedOn, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);

    // ── Set up opening animation (0 → 1) ─────────────────────────────────
    g_pAnimationManager->createAnimation(
        0.0f, m_progress,
        g_pConfigManager->getAnimationPropertyConfig("windowsMove"),
        AVARDAMAGE_NONE);

    m_progress->setUpdateCallback(niriDamageCallback);

    // Animate open; on completion re-render all FBs at full resolution.
    *m_progress = 1.0f;
    m_progress->setCallbackOnEnd([this](auto) { redrawAll(); });

    // ── Cursor and input ──────────────────────────────────────────────────
    Cursor::overrideController->setOverride("left_ptr", Cursor::CURSOR_OVERRIDE_UNKNOWN);
    m_lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position;

    // onMove — track cursor, update hover highlight.
    auto onMove = [this](Event::SCallbackInfo& info) {
        if (m_closing) return;
        info.cancelled      = true;
        m_lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor.lock()->m_position;
        updateHover();
    };

    // onSelect — record which workspace was clicked and close.
    auto onSelect = [this](Event::SCallbackInfo& info) {
        if (m_closing) return;
        info.cancelled = true;
        selectHoveredWorkspace();
        close();
    };

    m_mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen(
        [onMove](Vector2D, Event::SCallbackInfo& info) { onMove(info); });

    m_touchMoveHook = Event::bus()->m_events.input.touch.motion.listen(
        [onMove](ITouch::SMotionEvent, Event::SCallbackInfo& info) { onMove(info); });

    m_mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(
        [onSelect](IPointer::SButtonEvent, Event::SCallbackInfo& info) { onSelect(info); });

    m_touchDownHook = Event::bus()->m_events.input.touch.down.listen(
        [onSelect](ITouch::SDownEvent, Event::SCallbackInfo& info) { onSelect(info); });
}

// ────────────────────────────────────────────────────────────────────────────
//  updateHover
// ────────────────────────────────────────────────────────────────────────────

void COverview::updateHover() {
    const int prev = m_hoveredIdx;
    m_hoveredIdx   = -1;

    for (int i = 0; i < (int)m_strips.size(); ++i) {
        CBox box = m_strips[i].overviewBox;
        box.y += m_scrollOffset;

        if (m_lastMousePosLocal.x >= box.x && m_lastMousePosLocal.x < box.x + box.w &&
            m_lastMousePosLocal.y >= box.y && m_lastMousePosLocal.y < box.y + box.h) {
            m_hoveredIdx = i;
            break;
        }
    }

    if (prev != m_hoveredIdx)
        damage();
}

// ────────────────────────────────────────────────────────────────────────────
//  selectHoveredWorkspace
// ────────────────────────────────────────────────────────────────────────────

void COverview::selectHoveredWorkspace() {
    if (m_closing) return;
    m_closeToIdx = (m_hoveredIdx >= 0 && m_hoveredIdx < (int)m_strips.size())
                   ? m_hoveredIdx : m_activeIdx;
}

// ────────────────────────────────────────────────────────────────────────────
//  redrawIdx / redrawAll — refresh offscreen FBs
// ────────────────────────────────────────────────────────────────────────────

void COverview::redrawIdx(int idx) {
    if (!pMonitor || idx < 0 || idx >= (int)m_strips.size()) return;

    const auto PMONITOR    = pMonitor.lock();
    blockOverviewRendering = true;

    g_pHyprRenderer->makeEGLCurrent();

    // m_pixelSize members are already double (Vector2D), so pass them directly
    // to CBox (which takes double) — no conversion in the braced-initializer.
    const CBox renderBox{0.0, 0.0, PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y};
    auto& strip = m_strips[idx];

    if (strip.fb.m_size != renderBox.size()) {
        strip.fb.release();
        // alloc() takes int; cast explicitly rather than relying on implicit double→int.
        strip.fb.alloc((int)PMONITOR->m_pixelSize.x, (int)PMONITOR->m_pixelSize.y,
                       PMONITOR->m_output->state->state().drmFormat);
    }

    PHLWORKSPACE openSpecial = PMONITOR->m_activeSpecialWorkspace;
    if (openSpecial)
        PMONITOR->m_activeSpecialWorkspace.reset();

    m_startedOn->m_visible = false;

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &strip.fb);
    g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 1.0});

    if (strip.pWorkspace) {
        PMONITOR->m_activeWorkspace = strip.pWorkspace;

        if (strip.pWorkspace == m_startedOn)
            PMONITOR->m_activeSpecialWorkspace = openSpecial;

        g_pDesktopAnimationManager->startAnimation(
            strip.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
        strip.pWorkspace->m_visible = true;

        g_pHyprRenderer->renderWorkspace(PMONITOR, strip.pWorkspace,
                                          Time::steadyNow(), renderBox);

        strip.pWorkspace->m_visible = false;
        g_pDesktopAnimationManager->startAnimation(
            strip.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);

        if (strip.pWorkspace == m_startedOn)
            PMONITOR->m_activeSpecialWorkspace.reset();
    } else {
        g_pHyprRenderer->renderWorkspace(PMONITOR, strip.pWorkspace,
                                          Time::steadyNow(), renderBox);
    }

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    PMONITOR->m_activeSpecialWorkspace = openSpecial;
    PMONITOR->m_activeWorkspace        = m_startedOn;
    m_startedOn->m_visible             = true;
    g_pDesktopAnimationManager->startAnimation(
        m_startedOn, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);

    blockOverviewRendering = false;
}

void COverview::redrawAll() {
    for (int i = 0; i < (int)m_strips.size(); ++i)
        redrawIdx(i);
}

// ────────────────────────────────────────────────────────────────────────────
//  damage — force a full repaint of the monitor
// ────────────────────────────────────────────────────────────────────────────

void COverview::damage() {
    const auto MON = pMonitor.lock();
    if (!MON) return;
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(MON);
    blockDamageReporting = false;
}

// ────────────────────────────────────────────────────────────────────────────
//  onDamageReported — a workspace window changed; queue a FB refresh
// ────────────────────────────────────────────────────────────────────────────

void COverview::onDamageReported() {
    m_damageDirty = true;

    // Also damage the on-screen area of the active strip precisely.
    if (m_activeIdx >= 0 && m_activeIdx < (int)m_strips.size()) {
        const auto MON = pMonitor.lock();
        if (MON) {
            CBox box = m_strips[m_activeIdx].overviewBox;
            box.y += m_scrollOffset;
            box.scale(MON->m_scale).round();
            box.translate(MON->m_position);

            damage();
            blockDamageReporting = true;
            g_pHyprRenderer->damageBox(box);
            blockDamageReporting = false;
        }
    }

    g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

// ────────────────────────────────────────────────────────────────────────────
//  close — begin the closing animation, optionally switching workspace
// ────────────────────────────────────────────────────────────────────────────

void COverview::close() {
    if (m_closing) return;
    m_closing = true;

    const int closeIdx = (m_closeToIdx >= 0 && m_closeToIdx < (int)m_strips.size())
                         ? m_closeToIdx : m_activeIdx;

    const auto PMON = pMonitor.lock();

    // Switch workspace if the user selected a different one.
    if (closeIdx != m_activeIdx && PMON) {
        auto& strip  = m_strips[closeIdx];
        auto  OLDWS  = PMON->m_activeWorkspace;

        PMON->setSpecialWorkspace(0);

        if (!strip.pWorkspace || strip.pWorkspace->inert())
            g_pKeybindManager->changeworkspace(std::to_string((int64_t)strip.workspaceID));
        else
            g_pKeybindManager->changeworkspace(strip.pWorkspace->getConfigName());

        g_pDesktopAnimationManager->startAnimation(
            PMON->m_activeWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
        g_pDesktopAnimationManager->startAnimation(
            OLDWS, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);

        m_startedOn = PMON->m_activeWorkspace;
        m_activeIdx = closeIdx;
        m_closeToIdx = closeIdx;
    }

    // Re-render all FBs at full quality before the closing zoom.
    redrawAll();

    // Wire end callback BEFORE setting the target so it is always in place.
    m_progress->setCallbackOnEnd([](auto) {
        g_pEventLoopManager->doLater([] { g_pNiriOverview.reset(); });
    });

    // Animate back to 0.
    *m_progress = 0.0f;
}

// ────────────────────────────────────────────────────────────────────────────
//  onPreRender — refresh the active strip's FB if dirty
// ────────────────────────────────────────────────────────────────────────────

void COverview::onPreRender() {
    if (!m_damageDirty) return;
    m_damageDirty = false;
    // During closing only redraw the selected strip; otherwise redraw the active one.
    const int idx = m_closing
        ? ((m_closeToIdx >= 0 && m_closeToIdx < (int)m_strips.size()) ? m_closeToIdx : m_activeIdx)
        : m_activeIdx;
    redrawIdx(idx);
}

// ────────────────────────────────────────────────────────────────────────────
//  fullRender — called every frame from CNiriPassElement::draw()
// ────────────────────────────────────────────────────────────────────────────

void COverview::fullRender() {
    const auto PMONITOR = pMonitor.lock();
    if (!PMONITOR) return;

    // If the user switched workspaces externally, track the change.
    if (!m_closing && PMONITOR->m_activeWorkspace != m_startedOn) {
        for (int i = 0; i < (int)m_strips.size(); ++i) {
            if (m_strips[i].pWorkspace == PMONITOR->m_activeWorkspace) {
                m_activeIdx  = i;
                m_closeToIdx = i;
                break;
            }
        }
        m_startedOn = PMONITOR->m_activeWorkspace;
    }

    // t: 0 = fully closed geometry, 1 = fully open overview geometry.
    const float t    = m_progress->value();
    const float monW = PMONITOR->m_size.x;
    const float monH = PMONITOR->m_size.y;

    // The workspace strip that will expand/shrink on open/close.
    const int closeIdx = m_closing
        ? ((m_closeToIdx >= 0 && m_closeToIdx < (int)m_strips.size()) ? m_closeToIdx : m_activeIdx)
        : m_activeIdx;

    // Fill with background colour.
    g_pHyprOpenGL->clear(m_bgColor.stripA());

    // Full-screen damage region used for all draw calls.
    static CRegion fullDmg{0, 0, INT16_MAX, INT16_MAX};

    for (int i = 0; i < (int)m_strips.size(); ++i) {
        auto& strip = m_strips[i];

        // ── Fully-open position (logical px, including vertical scroll) ──
        CBox openBox = strip.overviewBox;
        openBox.y += m_scrollOffset;

        // ── Fully-closed position (logical px) ──
        //   closeIdx strip occupies the full screen.
        //   Strips above it go off the top; strips below go off the bottom.
        CBox closeBox;
        if (i == closeIdx) {
            closeBox = {0.0, 0.0, monW, monH};
        } else if (i < closeIdx) {
            closeBox = {0.0, -monH, monW, monH};
        } else {
            closeBox = {0.0,  monH, monW, monH};
        }

        // Lerp between closed and open positions in logical coords.
        CBox rb;
        rb.x = lerpf((float)closeBox.x, (float)openBox.x, t);
        rb.y = lerpf((float)closeBox.y, (float)openBox.y, t);
        rb.w = lerpf((float)closeBox.w, (float)openBox.w, t);
        rb.h = lerpf((float)closeBox.h, (float)openBox.h, t);

        // Convert to physical (pixel) coordinates.
        rb.scale(PMONITOR->m_scale).round();

        // Cull fully off-screen strips.
        const float scrW = monW * PMONITOR->m_scale;
        const float scrH = monH * PMONITOR->m_scale;
        if (rb.w <= 0.0 || rb.h <= 0.0)          continue;
        if (rb.x + rb.w <= 0.0 || rb.x >= scrW)  continue;
        if (rb.y + rb.h <= 0.0 || rb.y >= scrH)  continue;

        // ── Highlight (border) ───────────────────────────────────────────
        //   Active strip: accent border.  Hovered strip: neutral border.
        //   Fades in once the overview is more than half-open.
        const bool isActive  = (i == m_activeIdx);
        const bool isHovered = (i == m_hoveredIdx && i != m_activeIdx);

        if ((isActive || isHovered) && t > 0.5f) {
            const float fade   = std::clamp((t - 0.5f) * 2.0f, 0.0f, 1.0f);
            CHyprColor  bdrCol = isActive ? m_borderCol : m_hoverCol;
            bdrCol.a          *= fade;

            const float bdrPx = (float)m_borderSize * PMONITOR->m_scale;

            // Draw a slightly larger rect in the border colour behind the texture.
            CBox bdrBox = {rb.x - bdrPx, rb.y - bdrPx,
                           rb.w + 2.0f * bdrPx, rb.h + 2.0f * bdrPx};

            CHyprOpenGLImpl::SRectRenderData rdata;
            rdata.damage = &fullDmg;
            rdata.round  = 0;

            g_pHyprOpenGL->renderRect(bdrBox, bdrCol, rdata);
        }

        // ── Workspace thumbnail ──────────────────────────────────────────
        const int   cornerR  = std::max(1, (int)(4.0f * PMONITOR->m_scale));

        CHyprOpenGLImpl::STextureRenderData texData;
        texData.damage = &fullDmg;
        texData.a      = 1.0f;
        texData.round  = cornerR;

        g_pHyprOpenGL->renderTexture(strip.fb.getTexture(), rb, texData);
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  render — inject our pass element (called from hooked renderWorkspace)
// ────────────────────────────────────────────────────────────────────────────

void COverview::render() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<CNiriPassElement>());
}
