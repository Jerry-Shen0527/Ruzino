#include "orthographic_camera.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usdGeom/camera.h>

#include "GLFW/glfw3.h"
#include "spdlog/spdlog.h"

RUZINO_NAMESPACE_OPEN_SCOPE

OrthographicCamera::OrthographicCamera(const pxr::UsdGeomCamera& camera)
    : BaseCamera(camera)
{
    // Camera looks straight down: direction = -Z, up = +Y
    m_CameraDir = pxr::GfVec3d(0.0, 0.0, -1.0);
    m_CameraUp = pxr::GfVec3d(0.0, 1.0, 0.0);
    m_CameraRight = pxr::GfVec3d(1.0, 0.0, 0.0);
    UpdateCameraPosition();
}

void OrthographicCamera::UpdateCameraPosition()
{
    m_CameraPos = m_Center + pxr::GfVec3d(0.0, 0.0, m_Height);
    BaseLookAt(m_CameraPos, m_Center, pxr::GfVec3d(0.0, 1.0, 0.0));
    UpdateUsdTransform();
}

void OrthographicCamera::ConfigureUsdCamera()
{
    if (!GetPrim().IsValid()) return;

    auto prim = GetPrim();
    auto projAttr = prim.CreateAttribute(
        pxr::TfToken("projection"), pxr::SdfValueTypeNames->Token);
    projAttr.Set(pxr::TfToken("orthographic"));

    auto orthoSizeAttr = prim.CreateAttribute(
        pxr::TfToken("orthographicSize"), pxr::SdfValueTypeNames->Float);
    orthoSizeAttr.Set(static_cast<float>(m_OrthoSize * 2.0));

    auto clippingRangeAttr = prim.CreateAttribute(
        pxr::TfToken("clippingRange"), pxr::SdfValueTypeNames->Float2);
    clippingRangeAttr.Set(pxr::GfVec2f(0.1f, static_cast<float>(m_Height * 2.0f)));
}

void OrthographicCamera::MousePosUpdate(double xpos, double ypos)
{
    m_MousePosPrev = m_MousePos;
    m_MousePos = pxr::GfVec2d(xpos, ypos);

    // Middle mouse: pan in XY plane
    if (mouseButtonState[Middle]) {
        pxr::GfVec2d delta = m_MousePos - m_MousePosPrev;
        double panSpeed = m_OrthoSize * 0.003;
        m_Center += pxr::GfVec3d(
            -delta[0] * panSpeed,
            delta[1] * panSpeed,
            0.0);
        UpdateCameraPosition();
        m_HadInteractionLastFrame = true;
    }
}

void OrthographicCamera::MouseButtonUpdate(
    int button, int action, int mods)
{
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        mouseButtonState[Middle] = (action == GLFW_PRESS);
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        mouseButtonState[Right] = (action == GLFW_PRESS);
    }
}

void OrthographicCamera::MouseScrollUpdate(double xoffset, double yoffset)
{
    // Zoom: adjust orthographic view size
    double factor = 1.0 - yoffset * 0.1;
    m_OrthoSize *= factor;
    m_OrthoSize = std::max(m_OrthoSize, 0.01);
    m_OrthoSize = std::min(m_OrthoSize, 100.0);

    ConfigureUsdCamera();
    m_HadInteractionLastFrame = true;
}

void OrthographicCamera::Animate(double deltaT)
{
    m_HadInteractionLastFrame = false;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
