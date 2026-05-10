#include <gtest/gtest.h>

#include "RHI/rhi.hpp"
#include "pxr/imaging/hd/tokens.h"
#include "renderDelegate.h"
#include "spdlog/spdlog.h"

using namespace Ruzino;
using namespace pxr;

class PrimTest : public ::testing::Test {
   protected:
    void SetUp() override
    {
        RHI::init(false, true);
        spdlog::set_level(spdlog::level::warn);
        delegate_ = std::make_unique<Hd_RUZINO_RenderDelegate>();
    }

    void TearDown() override
    {
        delegate_.reset();
        RHI::shutdown();
    }

    std::unique_ptr<Hd_RUZINO_RenderDelegate> delegate_;
};

// ---- Camera ----

TEST_F(PrimTest, CreateCamera)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->camera, SdfPath("/cameras/main"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateFallbackCamera)
{
    auto* prim =
        delegate_->CreateFallbackSprim(HdPrimTypeTokens->camera);
    ASSERT_TRUE(prim != nullptr);
    delete prim;
}

// ---- Lights ----

TEST_F(PrimTest, CreateDistantLight)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->distantLight, SdfPath("/lights/sun"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateSphereLight)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->sphereLight, SdfPath("/lights/bulb"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateRectLight)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->rectLight, SdfPath("/lights/panel"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateDiskLight)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->diskLight, SdfPath("/lights/disk"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateCylinderLight)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->cylinderLight, SdfPath("/lights/tube"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateDomeLight)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->domeLight, SdfPath("/lights/env"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

// ---- Mesh (Rprim) ----

TEST_F(PrimTest, CreateMesh)
{
    auto* prim = delegate_->CreateRprim(
        HdPrimTypeTokens->mesh, SdfPath("/geometry/sphere"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroyRprim(prim);
}

// ---- Material ----

TEST_F(PrimTest, CreateMaterial)
{
    auto* prim = delegate_->CreateSprim(
        HdPrimTypeTokens->material, SdfPath("/materials/default"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroySprim(prim);
}

TEST_F(PrimTest, CreateFallbackMaterial)
{
    auto* prim =
        delegate_->CreateFallbackSprim(HdPrimTypeTokens->material);
    ASSERT_TRUE(prim != nullptr);
    delete prim;
}

// ---- Render Buffer (Bprim) ----

TEST_F(PrimTest, CreateRenderBuffer)
{
    auto* prim = delegate_->CreateBprim(
        HdPrimTypeTokens->renderBuffer, SdfPath("/aov/color"));
    ASSERT_TRUE(prim != nullptr);
    delegate_->DestroyBprim(prim);
}
