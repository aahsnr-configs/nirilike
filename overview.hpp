#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <vector>

class CMonitor;

// ───────────────────────────────────────────────────────────────────────
//  COverview  —  Niri-style vertical-strip workspace overview for Hyprland
//
//  All workspaces on the focused monitor are shown as scaled-down horizontal
//  strips stacked vertically.  The active workspace gets a coloured border;
//  the hovered workspace gets a lighter border.  Clicking a strip switches
//  to that workspace and closes the overview.
//
//  Animation (PHLANIMVAR<float> m_progress):
//      0 = fully closed  →  1 = fully open
//  Opening:  active workspace shrinks from full-screen to its strip slot;
//            other strips fly in from the top / bottom edges.
//  Closing:  reverse — selected workspace expands to full-screen.
// ───────────────────────────────────────────────────────────────────────

class COverview {
  public:
    // swipe parameter reserved for future trackpad-gesture support.
    explicit COverview(PHLWORKSPACE startedOn, bool swipe = false);
    ~COverview();

    // Called from the hooked renderWorkspace() — injects our pass element.
    void render();

    // Called from CNiriPassElement::draw() — the real compositor.
    void fullRender();

    // Damage / pre-render helpers — called by hooks in main.cpp.
    void damage();
    void onDamageReported();
    void onPreRender();

    // Dispatcher helpers.
    void close();
    void selectHoveredWorkspace();

    // Public flags read by the hooks in main.cpp.
    bool          blockOverviewRendering = false;
    bool          blockDamageReporting   = false;
    PHLMONITORREF pMonitor;
    bool          m_isSwiping = false;

  private:
    // Re-render one workspace's offscreen framebuffer.
    void redrawIdx(int idx);
    // Re-render every workspace's offscreen framebuffer.
    void redrawAll();
    // Update m_hoveredIdx from the current mouse position.
    void updateHover();

    // ── Per-workspace snapshot ──────────────────────────────────────────
    struct SWorkspaceStrip {
        CFramebuffer fb;
        WORKSPACEID  workspaceID = WORKSPACE_INVALID;
        PHLWORKSPACE pWorkspace;
        // Destination rect in LOGICAL pixels when fully open (before scroll).
        CBox         overviewBox;
    };

    std::vector<SWorkspaceStrip> m_strips;

    // Index of the workspace that was active when the overview opened.
    int m_activeIdx  = 0;
    // Index currently under the cursor, -1 = none.
    int m_hoveredIdx = -1;
    // Index to navigate to on close; -1 = stay on m_activeIdx.
    int m_closeToIdx = -1;

    // The workspace that was active on open (updated if user switches externally).
    PHLWORKSPACE m_startedOn;

    bool m_closing    = false;
    bool m_damageDirty = false;

    // ── Config values (read once in constructor) ────────────────────────
    int        m_gapSize    = 8;    // vertical gap between strips, logical px
    int        m_sideMargin = 60;   // horizontal margin each side, logical px
    int        m_vertMargin = 20;   // top/bottom margin, logical px
    CHyprColor m_bgColor    = CHyprColor{0.08, 0.08, 0.08, 1.0};
    CHyprColor m_borderCol  = CHyprColor{0.4,  0.7,  1.0,  1.0};
    CHyprColor m_hoverCol   = CHyprColor{0.55, 0.55, 0.55, 0.85};
    int        m_borderSize = 3;    // border thickness, logical px

    // ── Layout geometry (computed once in constructor) ──────────────────
    float m_scrollOffset = 0.0f; // vertical offset applied to all strips, logical px
    float m_stripH       = 0.0f; // height of one strip, logical px

    // Current cursor position relative to the monitor top-left, logical px.
    Vector2D m_lastMousePosLocal;

    // Animation variable: 0 = closed, 1 = open.
    PHLANIMVAR<float> m_progress;

    // Input listeners (destroying them unregisters the callbacks).
    CHyprSignalListener m_mouseMoveHook;
    CHyprSignalListener m_mouseButtonHook;
    CHyprSignalListener m_touchMoveHook;
    CHyprSignalListener m_touchDownHook;

    friend class CNiriPassElement;
};

// One overview at a time.
inline std::unique_ptr<COverview> g_pNiriOverview;
