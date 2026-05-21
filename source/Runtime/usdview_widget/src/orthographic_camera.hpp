#pragma once

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>

#include "free_camera.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

class OrthographicCamera : public BaseCamera {
   public:
    OrthographicCamera() = default;

    explicit OrthographicCamera(const pxr::UsdGeomCamera& camera);

    void MousePosUpdate(double xpos, double ypos) override;
    void MouseButtonUpdate(int button, int action, int mods) override;
    void MouseScrollUpdate(double xoffset, double yoffset) override;
    void Animate(double deltaT) override;

    // Set the paper plane center (camera target)
    void SetCenter(const pxr::GfVec3d& center)
    {
        m_Center = center;
        UpdateCameraPosition();
    }

    // Set the orthographic view size (half-width of the view)
    void SetOrthoSize(double size)
    {
        m_OrthoSize = size;
    }

    double GetOrthoSize() const
    {
        return m_OrthoSize;
    }

    // Set camera height above paper plane
    void SetHeight(double height)
    {
        m_Height = height;
        UpdateCameraPosition();
    }

    // Set USD camera to orthographic projection
    void ConfigureUsdCamera();

   private:
    void UpdateCameraPosition();

    pxr::GfVec3d m_Center = pxr::GfVec3d(0.0);
    double m_Height = 10.0;    // Camera distance above paper
    double m_OrthoSize = 1.0;  // Half-width of orthographic view

    pxr::GfVec2d m_MousePos = pxr::GfVec2d(0.0);
    pxr::GfVec2d m_MousePosPrev = pxr::GfVec2d(0.0);

    typedef enum {
        Middle,
        Right,

        MouseButtonCount
    } MouseButtons;

    std::array<bool, MouseButtonCount> mouseButtonState = { false };
};

RUZINO_NAMESPACE_CLOSE_SCOPE
