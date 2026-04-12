#include "free_camera.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

#include "widgets/api.h"
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"

RUZINO_NAMESPACE_OPEN_SCOPE
void BaseCamera::UpdateWorldToView()
{
    auto m_MatTranslatedWorldToView = pxr::GfMatrix4d(
        m_CameraRight[0],
        m_CameraUp[0],
        -m_CameraDir[0],
        0.0,
        m_CameraRight[1],
        m_CameraUp[1],
        -m_CameraDir[1],
        0.0,
        m_CameraRight[2],
        m_CameraUp[2],
        -m_CameraDir[2],
        0.0,
        0.0,
        0.0,
        0.0,
        1.0);
    m_MatWorldToView =
        (pxr::GfMatrix4d().SetIdentity().SetTranslate(-m_CameraPos) *
         m_MatTranslatedWorldToView);
}

void BaseCamera::UpdateUsdTransform()
{
    auto xform_op = GetTransformOp();
    if (!xform_op) {
        xform_op = AddTransformOp();
    }
    // Set the transform op using current time
    //pxr::UsdEditContext edit_ctx(
    //    GetPrim().GetStage(), GetPrim().GetStage()->GetRootLayer()   
    //);
    pxr::GfMatrix4d worldToCamera = m_MatWorldToView.GetInverse();
    xform_op.Set(worldToCamera, pxr::UsdTimeCode::Default());
}

BaseCamera::BaseCamera(const pxr::UsdGeomCamera& camera)
    : pxr::UsdGeomCamera(camera)
{
    pxr::UsdGeomXformOp transform_op = camera.GetTransformOp();
    if (transform_op) {
        pxr::GfMatrix4d transform_mat;
        transform_op.Get(&transform_mat, m_CurrentTime);

        m_CameraPos = transform_mat.ExtractTranslation();

        m_CameraDir = -transform_mat.ExtractRotation().TransformDir(
            pxr::GfVec3d(0.0, 0.0, 1.0));
        m_CameraUp = transform_mat.ExtractRotation().TransformDir(
            pxr::GfVec3d(0.0, 1.0, 0.0));
        m_CameraRight = transform_mat.ExtractRotation().TransformDir(
            pxr::GfVec3d(1.0, 0.0, 0.0));
        m_MatWorldToView = transform_mat.GetInverse();
    }

    // Load move speed or use default
    if (!GetPrim().GetAttribute(pxr::TfToken("move_speed")).Get(&m_MoveSpeed)) {
        m_MoveSpeed = 1.0;
    }
}

void BaseCamera::ReloadFromUsd()
{
    pxr::UsdGeomXformOp transform_op = GetTransformOp();
    if (transform_op) {
        pxr::GfMatrix4d transform_mat;
        transform_mat.SetIdentity();
        transform_op.Get(&transform_mat, m_CurrentTime);

        m_CameraPos = transform_mat.ExtractTranslation();

        m_CameraDir = -transform_mat.ExtractRotation().TransformDir(
            pxr::GfVec3d(0.0, 0.0, 1.0));
        m_CameraUp = transform_mat.ExtractRotation().TransformDir(
            pxr::GfVec3d(0.0, 1.0, 0.0));
        m_CameraRight = transform_mat.ExtractRotation().TransformDir(
            pxr::GfVec3d(1.0, 0.0, 0.0));
        m_MatWorldToView = transform_mat.GetInverse();
    }
}

void BaseCamera::BaseLookAt(
    pxr::GfVec3d cameraPos,
    pxr::GfVec3d cameraTarget,
    pxr::GfVec3d cameraUp)
{
    this->m_CameraPos = cameraPos;
    this->m_CameraDir = (cameraTarget - cameraPos).GetNormalized();
    this->m_CameraUp = cameraUp.GetNormalized();
    this->m_CameraRight =
        pxr::GfCross(this->m_CameraDir, this->m_CameraUp).GetNormalized();
    this->m_CameraUp =
        pxr::GfCross(this->m_CameraRight, this->m_CameraDir).GetNormalized();

    UpdateWorldToView();
    UpdateUsdTransform();
}

FirstPersonCamera::FirstPersonCamera(const pxr::UsdGeomCamera& camera)
    : BaseCamera(camera)
{
}

void FirstPersonCamera::KeyboardUpdate(
    int key,
    int scancode,
    int action,
    int mods)
{
    if (keyboardMap.find(key) == keyboardMap.end()) {
        return;
    }

    auto cameraKey = keyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        keyboardState[cameraKey] = true;
    }
    else {
        keyboardState[cameraKey] = false;
    }
}

void FirstPersonCamera::MousePosUpdate(double xpos, double ypos)
{
    mousePos = { double(xpos), double(ypos) };
}

void FirstPersonCamera::MouseButtonUpdate(int button, int action, int mods)
{
    if (mouseButtonMap.find(button) == mouseButtonMap.end()) {
        return;
    }

    auto cameraButton = mouseButtonMap.at(button);
    if (action == GLFW_PRESS) {
        mouseButtonState[cameraButton] = true;
    }
    else {
        mouseButtonState[cameraButton] = false;
    }
}

void FirstPersonCamera::LookAt(
    pxr::GfVec3d cameraPos,
    pxr::GfVec3d cameraTarget,
    pxr::GfVec3d cameraUp)
{
    // Make the base method public.
    BaseLookAt(cameraPos, cameraTarget, cameraUp);
}

void FirstPersonCamera::LookTo(
    pxr::GfVec3d cameraPos,
    pxr::GfVec3d cameraDir,
    pxr::GfVec3d cameraUp)
{
    BaseLookAt(cameraPos, cameraPos + cameraDir, cameraUp);
}
std::pair<bool, pxr::GfVec3d> FirstPersonCamera::AnimateTranslation(
    double deltaT)
{
    bool cameraDirty = false;
    double moveStep = deltaT * m_MoveSpeed;
    pxr::GfVec3d cameraMoveVec(0.0);

    if (keyboardState[KeyboardControls::SpeedUp])
        moveStep *= 3.0;

    if (keyboardState[KeyboardControls::SlowDown])
        moveStep *= 0.1;

    if (keyboardState[KeyboardControls::MoveForward]) {
        cameraDirty = true;
        cameraMoveVec += m_CameraDir * moveStep;
    }

    if (keyboardState[KeyboardControls::MoveBackward]) {
        cameraDirty = true;
        cameraMoveVec += -m_CameraDir * moveStep;
    }

    if (keyboardState[KeyboardControls::MoveLeft]) {
        cameraDirty = true;
        cameraMoveVec += -m_CameraRight * moveStep;
    }

    if (keyboardState[KeyboardControls::MoveRight]) {
        cameraDirty = true;
        cameraMoveVec += m_CameraRight * moveStep;
    }

    if (keyboardState[KeyboardControls::MoveUp]) {
        cameraDirty = true;
        cameraMoveVec += m_CameraUp * moveStep;
    }

    if (keyboardState[KeyboardControls::MoveDown]) {
        cameraDirty = true;
        cameraMoveVec += -m_CameraUp * moveStep;
    }
    return std::make_pair(cameraDirty, cameraMoveVec);
}
void FirstPersonCamera::UpdateCamera(
    pxr::GfVec3d cameraMoveVec,
    pxr::GfRotation cameraRotation)
{
    m_CameraPos += cameraMoveVec;
    m_CameraDir = cameraRotation.TransformDir(m_CameraDir).GetNormalized();
    m_CameraUp = cameraRotation.TransformDir(m_CameraUp).GetNormalized();
    m_CameraRight = pxr::GfCross(m_CameraDir, m_CameraUp).GetNormalized();

    UpdateWorldToView();
    UpdateUsdTransform();
}

std::pair<bool, pxr::GfRotation> FirstPersonCamera::AnimateRoll(
    pxr::GfRotation initialRotation)
{
    bool cameraDirty = false;
    pxr::GfRotation cameraRotation = initialRotation;
    if (keyboardState[KeyboardControls::RollLeft] ||
        keyboardState[KeyboardControls::RollRight]) {
        double roll = double(keyboardState[KeyboardControls::RollLeft]) *
                          -m_RotateSpeed * 2.0 +
                      double(keyboardState[KeyboardControls::RollRight]) *
                          m_RotateSpeed * 2.0;

        cameraRotation = pxr::GfRotation(m_CameraDir, roll) * cameraRotation;
        cameraDirty = true;
    }
    return std::make_pair(cameraDirty, cameraRotation);
}
void FirstPersonCamera::Animate(double deltaT)
{
    // track mouse delta
    pxr::GfVec2d mouseMove = mousePos - mousePosPrev;
    mousePosPrev = mousePos;

    bool cameraDirty = false;
    pxr::GfRotation cameraRotation = pxr::GfRotation().SetIdentity();

    // Use middle button for rotation (Blender-style)
    if (mouseButtonState[MouseButtons::Middle] &&
        (mouseMove[0] != 0 || mouseMove[1] != 0)) {
        double yaw = m_RotateSpeed * mouseMove[0];
        double pitch = m_RotateSpeed * mouseMove[1];

        cameraRotation =
            pxr::GfRotation(pxr::GfVec3d(0.0, 0.0, 1.0), -yaw) * cameraRotation;
        cameraRotation =
            pxr::GfRotation(m_CameraRight, -pitch) * cameraRotation;

        cameraDirty = true;
    }

    // handle keyboard roll next
    auto rollResult = AnimateRoll(cameraRotation);
    cameraDirty |= rollResult.first;
    cameraRotation = rollResult.second;

    // handle translation
    auto translateResult = AnimateTranslation(deltaT);
    cameraDirty |= translateResult.first;
    const pxr::GfVec3d& cameraMoveVec = translateResult.second;

    if (cameraDirty) {
        UpdateCamera(cameraMoveVec, cameraRotation);
    }
}
void FirstPersonCamera::AnimateSmooth(double deltaT)
{
    const double c_DampeningRate = 7.5;
    double dampenWeight = exp(-c_DampeningRate * deltaT);

    pxr::GfVec2d mouseMove(0.0, 0.0);
    if (mouseButtonState[MouseButtons::Middle]) {
        if (!isMoving) {
            isMoving = true;
            mousePosPrev = mousePos;
        }

        mousePosDamp[0] =
            pxr::GfLerp(dampenWeight, mousePos[0], mousePosPrev[0]);
        mousePosDamp[1] =
            pxr::GfLerp(dampenWeight, mousePos[1], mousePosPrev[1]);

        // track mouse delta
        mouseMove = mousePosDamp - mousePosPrev;
        mousePosPrev = mousePosDamp;
    }
    else {
        isMoving = false;
    }

    bool cameraDirty = false;
    pxr::GfRotation cameraRotation;

    // handle mouse rotation first
    // this will affect the movement vectors in the world matrix, which we use
    // below
    if (mouseMove[0] != 0 || mouseMove[1] != 0) {
        double yaw = m_RotateSpeed * mouseMove[0];
        double pitch = m_RotateSpeed * mouseMove[1];

        cameraRotation =
            pxr::GfRotation(pxr::GfVec3d(0.0, 1.0, 0.0), -yaw) * cameraRotation;
        cameraRotation =
            pxr::GfRotation(m_CameraRight, -pitch) * cameraRotation;

        cameraDirty = true;
    }

    // handle keyboard roll next
    auto rollResult = AnimateRoll(cameraRotation);
    cameraDirty |= rollResult.first;
    cameraRotation = rollResult.second;

    // handle translation
    auto translateResult = AnimateTranslation(deltaT);
    cameraDirty |= translateResult.first;
    const pxr::GfVec3d& cameraMoveVec = translateResult.second;

    if (cameraDirty) {
        UpdateCamera(cameraMoveVec, cameraRotation);
    }
}

void FirstPersonCamera::MouseScrollUpdate(double xoffset, double yoffset)
{
    GetPrim().GetAttribute(pxr::TfToken("move_speed")).Get(&m_MoveSpeed);
    m_MoveSpeed =
        std::clamp(m_MoveSpeed * (yoffset > 0 ? 1.05 : 1.0 / 1.05), 0.1, 500.0);
    using namespace pxr;
    GetPrim()
        .CreateAttribute(TfToken("move_speed"), SdfValueTypeNames->Double)
        .Set(m_MoveSpeed);
}

void ThirdPersonCamera::KeyboardUpdate(
    int key,
    int scancode,
    int action,
    int mods)
{
    if (keyboardMap.find(key) == keyboardMap.end()) {
        return;
    }

    auto cameraKey = keyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        keyboardState[cameraKey] = true;
    }
    else {
        keyboardState[cameraKey] = false;
    }
}

void ThirdPersonCamera::MousePosUpdate(double xpos, double ypos)
{
    m_MousePos = pxr::GfVec2d(double(xpos), double(ypos));
}

void ThirdPersonCamera::MouseButtonUpdate(int button, int action, int mods)
{
    const bool pressed = (action == GLFW_PRESS);
    const bool shift_pressed = (mods & GLFW_MOD_SHIFT) != 0;

    switch (button) {
        case GLFW_MOUSE_BUTTON_MIDDLE:
            // Shift + Middle: pan, Middle alone: orbit
            if (shift_pressed) {
                mouseButtonState[MouseButtons::Middle] = pressed;
                mouseButtonState[MouseButtons::Left] = false;  // Clear orbit
            }
            else {
                mouseButtonState[MouseButtons::Left] = pressed;  // Orbit
                mouseButtonState[MouseButtons::Middle] = false;  // Clear pan
            }
            break;
        case GLFW_MOUSE_BUTTON_RIGHT:
            mouseButtonState[MouseButtons::Right] = pressed;
            break;
        default: break;
    }
}

void ThirdPersonCamera::MouseScrollUpdate(double xoffset, double yoffset)
{
    const double scrollFactor = 1.15f;
    double oldDistance = m_Distance;
    m_Distance = std::clamp(
        m_Distance * (yoffset < 0 ? scrollFactor : 1.0f / scrollFactor),
        m_MinDistance,
        m_MaxDistance);

    // If distance changed, update position and USD
    if (oldDistance != m_Distance) {
        // Update camera position based on new distance
        double x = m_Distance * cos(m_Pitch) * cos(m_Yaw);
        double y = m_Distance * cos(m_Pitch) * sin(m_Yaw);
        double z = m_Distance * sin(m_Pitch);
        pxr::GfVec3d offset(x, y, z);

        m_CameraPos = m_TargetPos + offset;
        m_CameraDir = -offset.GetNormalized();

        pxr::GfVec3d world_up(0, 0, 1);
        if (cos(m_Pitch) < 0) {
            world_up = pxr::GfVec3d(0, 0, -1);
        }
        if (std::abs(std::cos(m_Pitch)) < 0.02) {
            world_up = pxr::GfVec3d(-sin(m_Yaw), cos(m_Yaw), 0);
        }
        m_CameraRight = pxr::GfCross(m_CameraDir, world_up).GetNormalized();
        m_CameraUp = pxr::GfCross(m_CameraRight, m_CameraDir).GetNormalized();

        UpdateWorldToView();
        UpdateUsdTransform();
    }
}

void ThirdPersonCamera::JoystickUpdate(int axis, double value)
{
    switch (axis) {
        case GLFW_GAMEPAD_AXIS_RIGHT_X: m_DeltaYaw = value; break;
        case GLFW_GAMEPAD_AXIS_RIGHT_Y: m_DeltaPitch = value; break;
        default: break;
    }
}

void ThirdPersonCamera::JoystickButtonUpdate(int button, bool pressed)
{
    switch (button) {
        case GLFW_GAMEPAD_BUTTON_B:
            if (pressed)
                m_DeltaDistance -= 1;
            break;
        case GLFW_GAMEPAD_BUTTON_A:
            if (pressed)
                m_DeltaDistance += 1;
            break;
        default: break;
    }
}

void ThirdPersonCamera::SetRotation(double yaw, double pitch)
{
    m_Yaw = yaw;
    m_Pitch = pitch;
}

void ThirdPersonCamera::SetCameraStateFromMatrix(
    const pxr::GfVec3d& position,
    const pxr::GfVec3d& direction,
    const pxr::GfVec3d& up)
{
    // Compute the target position that would be "distance" units in front
    // This allows us to use BaseLookAt while preserving the exact orientation
    pxr::GfVec3d implicit_target = position + direction * m_Distance;

    // Apply gimbal lock protection (same as Animate method)
    pxr::GfVec3d protected_up = up;

    // Check if we're near the poles (direction nearly parallel to Z-axis)
    double dir_z_component = std::abs(direction[2]);
    if (dir_z_component > 0.98) {  // Near vertical (< ~11 degrees from pole)
        // Use a horizontal up vector to avoid gimbal lock
        // Calculate it perpendicular to the view direction in XY plane
        double dir_xy_length = std::sqrt(
            direction[0] * direction[0] + direction[1] * direction[1]);
        if (dir_xy_length > 0.01) {
            // Use perpendicular in XY plane
            protected_up =
                pxr::GfVec3d(-direction[1], direction[0], 0.0).GetNormalized();
        }
        else {
            // Almost exactly at pole, use arbitrary horizontal vector
            protected_up = pxr::GfVec3d(1.0, 0.0, 0.0);
        }
    }

    // Use BaseLookAt to set all internal vectors properly
    BaseLookAt(position, implicit_target, protected_up);
}

void ThirdPersonCamera::SetView(const pxr::GfFrustum& view)
{
    m_ProjectionMatrix = view.ComputeProjectionMatrix();
    m_InverseProjectionMatrix = m_ProjectionMatrix.GetInverse();
    auto viewport = view.GetWindow();
    m_ViewportSize = viewport.GetSize();
}
bool ThirdPersonCamera::AnimateOrbit(double deltaT)
{
    bool hasInteraction = false;

    if (mouseButtonState[MouseButtons::Left]) {
        pxr::GfVec2d mouseMove = m_MousePos - m_MousePosPrev;
        if (mouseMove[0] != 0.0 || mouseMove[1] != 0.0) {
            double rotateSpeed = m_RotateSpeed * 0.2;

            m_Yaw -= rotateSpeed * mouseMove[0];
            m_Pitch += rotateSpeed * mouseMove[1];
            hasInteraction = true;
        }
    }

    const double ORBIT_SENSITIVITY = 1.5f;
    const double ZOOM_SENSITIVITY = 40.f;

    if (m_DeltaDistance != 0.0) {
        m_Distance += ZOOM_SENSITIVITY * deltaT * m_DeltaDistance;
        hasInteraction = true;
    }
    if (m_DeltaYaw != 0.0) {
        m_Yaw += ORBIT_SENSITIVITY * deltaT * m_DeltaYaw;
        hasInteraction = true;
    }
    if (m_DeltaPitch != 0.0) {
        m_Pitch += ORBIT_SENSITIVITY * deltaT * m_DeltaPitch;
        hasInteraction = true;
    }

    m_Distance = std::clamp(m_Distance, m_MinDistance, m_MaxDistance);

    // Limit pitch to avoid gimbal lock at exact top/bottom (±90°)
    // Leave small margin to prevent camera flipping
    const double PITCH_LIMIT =
        M_PI / 2.0 * 0.98;  // ~88.2°, leaves margin before gimbal lock
    m_Pitch = std::clamp(m_Pitch, -PITCH_LIMIT, PITCH_LIMIT);

    // Normalize yaw to keep it sane (pitch is already clamped)
    if (m_Yaw > M_PI)
        m_Yaw -= 2 * M_PI;
    if (m_Yaw < -M_PI)
        m_Yaw += 2 * M_PI;

    m_DeltaDistance = 0;
    m_DeltaYaw = 0;
    m_DeltaPitch = 0;

    return hasInteraction;
}

bool ThirdPersonCamera::AnimateTranslation(const pxr::GfMatrix3d& viewMatrix)
{
    // If the view parameters have never been set, we can't translate
    if (m_ViewportSize[0] <= 0.0 || m_ViewportSize[1] <= 0.0)
        return false;

    if (m_MousePos == m_MousePosPrev)
        return false;

    if (mouseButtonState[MouseButtons::Middle]) {
        pxr::GfVec2d mouseDelta(
            m_MousePos[0] - m_MousePosPrev[0],
            m_MousePos[1] - m_MousePosPrev[1]);

        // Scale factor based on distance
        double pixelToWorld = m_Distance * 0.002;

        // Pan the target position inversely to mouse movement
        m_TargetPos -= mouseDelta[0] * pixelToWorld * m_CameraRight;
        m_TargetPos += mouseDelta[1] * pixelToWorld * m_CameraUp;
        return true;
    }
    return false;
}

void ThirdPersonCamera::Animate(double deltaT)
{
    bool hasInteraction = AnimateOrbit(deltaT);

    // Z-up spherical coordinates: offset from target to camera
    // pitch = 90° => camera above, pitch = -90° => camera below
    double x = m_Distance * cos(m_Pitch) * cos(m_Yaw);
    double y = m_Distance * cos(m_Pitch) * sin(m_Yaw);
    double z = m_Distance * sin(m_Pitch);
    pxr::GfVec3d offset(x, y, z);

    m_CameraPos = m_TargetPos + offset;
    m_CameraDir = -offset.GetNormalized();

    // Build right and up vectors in Z-up world space
    pxr::GfVec3d world_up(0, 0, 1);

    if (cos(m_Pitch) < 0) {
        world_up = pxr::GfVec3d(0, 0, -1);
    }

    // Avoid gimbal lock when looking straight up/down (Pitch near +/- 90, 270)
    if (std::abs(std::cos(m_Pitch)) < 0.02) {
        world_up = pxr::GfVec3d(-sin(m_Yaw), cos(m_Yaw), 0);
    }

    m_CameraRight = pxr::GfCross(m_CameraDir, world_up).GetNormalized();
    m_CameraUp = pxr::GfCross(m_CameraRight, m_CameraDir).GetNormalized();

    // Apply pan translation
    hasInteraction |= AnimateTranslation(pxr::GfMatrix3d(1.0));

    // Recalculate position after target may have moved
    x = m_Distance * cos(m_Pitch) * cos(m_Yaw);
    y = m_Distance * cos(m_Pitch) * sin(m_Yaw);
    z = m_Distance * sin(m_Pitch);
    offset = pxr::GfVec3d(x, y, z);
    m_CameraPos = m_TargetPos + offset;
    m_CameraDir = -offset.GetNormalized();

    // Recalculate right and up
    world_up = pxr::GfVec3d(0, 0, 1);
    if (cos(m_Pitch) < 0) {
        world_up = pxr::GfVec3d(0, 0, -1);
    }
    if (std::abs(std::cos(m_Pitch)) < 0.02) {
        world_up = pxr::GfVec3d(-sin(m_Yaw), cos(m_Yaw), 0);
    }
    m_CameraRight = pxr::GfCross(m_CameraDir, world_up).GetNormalized();
    m_CameraUp = pxr::GfCross(m_CameraRight, m_CameraDir).GetNormalized();

    UpdateWorldToView();

    // Track interaction for event emission
    m_HadInteractionLastFrame = hasInteraction;

    // Only write to USD if there was actual user interaction
    if (hasInteraction) {
        UpdateUsdTransform();
        SaveState();  // Also save spherical coordinates to USD attributes
    }

    m_MousePosPrev = m_MousePos;
}
void ThirdPersonCamera::LookAt(
    pxr::GfVec3d cameraPos,
    pxr::GfVec3d cameraTarget)
{
    pxr::GfVec3d cameraToTarget = cameraTarget - cameraPos;
    pxr::GfVec3d offset = -cameraToTarget;

    double azimuth, elevation, dirLength;
    CartesianToSpherical(offset, azimuth, elevation, dirLength);

    SetTargetPosition(cameraTarget);
    SetDistance(dirLength);
    SetRotation(azimuth, elevation);

    double x = m_Distance * cos(m_Pitch) * cos(m_Yaw);
    double y = m_Distance * cos(m_Pitch) * sin(m_Yaw);
    double z = m_Distance * sin(m_Pitch);
    pxr::GfVec3d new_offset(x, y, z);

    m_CameraPos = m_TargetPos + new_offset;
    m_CameraDir = -new_offset.GetNormalized();

    pxr::GfVec3d world_up(0, 0, 1);
    if (cos(m_Pitch) < 0) {
        world_up = pxr::GfVec3d(0, 0, -1);
    }
    if (std::abs(std::cos(m_Pitch)) < 0.02) {
        world_up = pxr::GfVec3d(-sin(m_Yaw), cos(m_Yaw), 0);
    }
    m_CameraRight = pxr::GfCross(m_CameraDir, world_up).GetNormalized();
    m_CameraUp = pxr::GfCross(m_CameraRight, m_CameraDir).GetNormalized();

    UpdateWorldToView();
    UpdateUsdTransform();
}

void ThirdPersonCamera::LookTo(
    pxr::GfVec3d cameraPos,
    pxr::GfVec3d cameraDir,
    std::optional<double> targetDistance)
{
    double azimuth, elevation, dirLength;
    CartesianToSpherical(-cameraDir, azimuth, elevation, dirLength);
    cameraDir /= dirLength;

    double const distance = targetDistance.value_or(GetDistance());
    SetTargetPosition(cameraPos + cameraDir * distance);
    SetDistance(distance);
    SetRotation(azimuth, elevation);

    double x = m_Distance * cos(m_Pitch) * cos(m_Yaw);
    double y = m_Distance * cos(m_Pitch) * sin(m_Yaw);
    double z = m_Distance * sin(m_Pitch);
    pxr::GfVec3d offset(x, y, z);

    m_CameraPos = m_TargetPos + offset;
    m_CameraDir = -offset.GetNormalized();

    pxr::GfVec3d world_up(0, 0, 1);
    if (cos(m_Pitch) < 0) {
        world_up = pxr::GfVec3d(0, 0, -1);
    }
    if (std::abs(std::cos(m_Pitch)) < 0.02) {
        world_up = pxr::GfVec3d(-sin(m_Yaw), cos(m_Yaw), 0);
    }
    m_CameraRight = pxr::GfCross(m_CameraDir, world_up).GetNormalized();
    m_CameraUp = pxr::GfCross(m_CameraRight, m_CameraDir).GetNormalized();

    UpdateWorldToView();
    UpdateUsdTransform();
}

void ThirdPersonCamera::CartesianToSpherical(
    const pxr::GfVec3d& cartesian,
    double& azimuth,
    double& elevation,
    double& length)
{
    length = cartesian.GetLength();
    if (length < 1e-6) {
        azimuth = 0.0;
        elevation = 0.0;
        return;
    }
    // Z-up coordinate system
    elevation = std::asin(cartesian[2] / length);
    azimuth = std::atan2(cartesian[1], cartesian[0]);
}

void ThirdPersonCamera::SaveState()
{
    using namespace pxr;
    auto prim = GetPrim();
    if (!prim)
        return;

    prim.CreateAttribute(
            TfToken("third_person:target"), SdfValueTypeNames->Double3)
        .Set(m_TargetPos);
    prim.CreateAttribute(
            TfToken("third_person:distance"), SdfValueTypeNames->Double)
        .Set(m_Distance);
    prim.CreateAttribute(TfToken("third_person:yaw"), SdfValueTypeNames->Double)
        .Set(m_Yaw);
    prim.CreateAttribute(
            TfToken("third_person:pitch"), SdfValueTypeNames->Double)
        .Set(m_Pitch);
}

void ThirdPersonCamera::LoadState()
{
    using namespace pxr;
    auto prim = GetPrim();
    if (!prim)
        return;

    auto targetAttr = prim.GetAttribute(TfToken("third_person:target"));
    auto distanceAttr = prim.GetAttribute(TfToken("third_person:distance"));
    auto yawAttr = prim.GetAttribute(TfToken("third_person:yaw"));
    auto pitchAttr = prim.GetAttribute(TfToken("third_person:pitch"));

    if (targetAttr && distanceAttr && yawAttr && pitchAttr) {
        targetAttr.Get(&m_TargetPos);
        distanceAttr.Get(&m_Distance);
        yawAttr.Get(&m_Yaw);
        pitchAttr.Get(&m_Pitch);

        // Ensure distance is valid
        if (m_Distance < 0.1) {
            m_Distance = 10.0;
        }

        // Update camera position from loaded state
        double x = m_Distance * cos(m_Pitch) * cos(m_Yaw);
        double y = m_Distance * cos(m_Pitch) * sin(m_Yaw);
        double z = m_Distance * sin(m_Pitch);
        pxr::GfVec3d offset(x, y, z);

        m_CameraPos = m_TargetPos + offset;
        m_CameraDir = -offset.GetNormalized();

        pxr::GfVec3d world_up(0, 0, 1);
        if (cos(m_Pitch) < 0) {
            world_up = pxr::GfVec3d(0, 0, -1);
        }
        if (std::abs(std::cos(m_Pitch)) < 0.02) {
            world_up = pxr::GfVec3d(-sin(m_Yaw), cos(m_Yaw), 0);
        }
        m_CameraRight = pxr::GfCross(m_CameraDir, world_up).GetNormalized();
        m_CameraUp = pxr::GfCross(m_CameraRight, m_CameraDir).GetNormalized();

        UpdateWorldToView();
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE