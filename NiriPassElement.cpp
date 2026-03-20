#include "NiriPassElement.hpp"
// Monitor.hpp must be included here to give CMonitor a complete definition.
// PassElement.hpp only forward-declares it via DesktopTypes.hpp, which is
// not enough to access member fields like m_size.
#include <hyprland/src/helpers/Monitor.hpp>
#include "overview.hpp"

void CNiriPassElement::draw(const CRegion& /*damage*/) {
    if (g_pNiriOverview)
        g_pNiriOverview->fullRender();
}

bool CNiriPassElement::needsLiveBlur() {
    return false;
}

bool CNiriPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CNiriPassElement::boundingBox() {
    if (!g_pNiriOverview || !g_pNiriOverview->pMonitor)
        return std::nullopt;
    const auto MON = g_pNiriOverview->pMonitor.lock();
    if (!MON)
        return std::nullopt;
    return CBox(Vector2D{0, 0}, MON->m_size);
}

CRegion CNiriPassElement::opaqueRegion() {
    if (!g_pNiriOverview || !g_pNiriOverview->pMonitor)
        return CRegion{};
    const auto MON = g_pNiriOverview->pMonitor.lock();
    if (!MON)
        return CRegion{};
    return CBox(Vector2D{0, 0}, MON->m_size);
}
