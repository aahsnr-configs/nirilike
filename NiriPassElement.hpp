#pragma once

#include <hyprland/src/render/pass/PassElement.hpp>

class CNiriPassElement : public IPassElement {
  public:
    CNiriPassElement()          = default;
    virtual ~CNiriPassElement() = default;

    virtual void                draw(const CRegion& damage);
    virtual bool                needsLiveBlur();
    virtual bool                needsPrecomputeBlur();
    virtual std::optional<CBox> boundingBox();
    virtual CRegion             opaqueRegion();

    virtual const char* passName() {
        return "CNiriPassElement";
    }
};
