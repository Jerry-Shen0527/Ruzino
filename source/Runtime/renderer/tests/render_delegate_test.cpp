#include <gtest/gtest.h>

#include <algorithm>

#include "RHI/rhi.hpp"
#include "pxr/imaging/hd/tokens.h"
#include "renderBuffer.h"
#include "renderDelegate.h"
#include "spdlog/spdlog.h"

using namespace Ruzino;
using namespace pxr;

class RenderDelegateTest : public ::testing::Test {
   protected:
    void SetUp() override
    {
        RHI::init(false, true);
        spdlog::set_level(spdlog::level::warn);
    }

    void TearDown() override
    {
        RHI::shutdown();
    }
};

TEST_F(RenderDelegateTest, CreateDefault)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    EXPECT_TRUE(delegate.GetRenderParam() != nullptr);
}

TEST_F(RenderDelegateTest, CreateWithSettings)
{
    HdRenderSettingsMap settings;
    settings[TfToken("enableSceneColors")] = VtValue(false);

    auto delegate = Hd_RUZINO_RenderDelegate(settings);
    EXPECT_TRUE(delegate.GetRenderParam() != nullptr);
}

TEST_F(RenderDelegateTest, SupportedRprimTypes)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    const auto& types = delegate.GetSupportedRprimTypes();

    EXPECT_FALSE(types.empty());
    EXPECT_NE(
        std::find(types.begin(), types.end(), HdPrimTypeTokens->mesh),
        types.end());
}

TEST_F(RenderDelegateTest, SupportedSprimTypes)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    const auto& types = delegate.GetSupportedSprimTypes();

    EXPECT_FALSE(types.empty());
    EXPECT_NE(
        std::find(types.begin(), types.end(), HdPrimTypeTokens->camera),
        types.end());
    EXPECT_NE(
        std::find(types.begin(), types.end(), HdPrimTypeTokens->material),
        types.end());
    EXPECT_NE(
        std::find(types.begin(), types.end(), HdPrimTypeTokens->distantLight),
        types.end());
}

TEST_F(RenderDelegateTest, SupportedBprimTypes)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    const auto& types = delegate.GetSupportedBprimTypes();

    EXPECT_FALSE(types.empty());
    EXPECT_NE(
        std::find(
            types.begin(), types.end(), HdPrimTypeTokens->renderBuffer),
        types.end());
}

TEST_F(RenderDelegateTest, RenderSettingRoundTrip)
{
    auto delegate = Hd_RUZINO_RenderDelegate();

    auto key = TfToken("enableSceneColors");
    delegate.SetRenderSetting(key, VtValue(true));

    auto val = delegate.GetRenderSetting(key);
    ASSERT_TRUE(val.IsHolding<bool>());
    EXPECT_TRUE(val.Get<bool>());
}

TEST_F(RenderDelegateTest, GetDefaultAovDescriptorColor)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    auto desc = delegate.GetDefaultAovDescriptor(HdAovTokens->color);

    EXPECT_EQ(desc.format, HdFormatFloat32Vec4);
    EXPECT_TRUE(desc.clearValue.IsHolding<GfVec4f>());
}

TEST_F(RenderDelegateTest, GetDefaultAovDescriptorDepth)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    auto desc = delegate.GetDefaultAovDescriptor(HdAovTokens->depth);

    EXPECT_EQ(desc.format, HdFormatFloat32);
    EXPECT_TRUE(desc.clearValue.IsHolding<float>());
}

TEST_F(RenderDelegateTest, GetDefaultAovDescriptorNormal)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    auto desc = delegate.GetDefaultAovDescriptor(HdAovTokens->normal);

    EXPECT_EQ(desc.format, HdFormatFloat32Vec3);
}

TEST_F(RenderDelegateTest, CreateFallbackSprimCamera)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    auto* sprim = delegate.CreateFallbackSprim(HdPrimTypeTokens->camera);
    ASSERT_TRUE(sprim != nullptr);
    delete sprim;
}

TEST_F(RenderDelegateTest, CreateFallbackSprimLight)
{
    auto delegate = Hd_RUZINO_RenderDelegate();

    auto* distant =
        delegate.CreateFallbackSprim(HdPrimTypeTokens->distantLight);
    ASSERT_TRUE(distant != nullptr);
    delete distant;

    auto* sphere =
        delegate.CreateFallbackSprim(HdPrimTypeTokens->sphereLight);
    ASSERT_TRUE(sphere != nullptr);
    delete sphere;

    auto* rect = delegate.CreateFallbackSprim(HdPrimTypeTokens->rectLight);
    ASSERT_TRUE(rect != nullptr);
    delete rect;

    auto* disk = delegate.CreateFallbackSprim(HdPrimTypeTokens->diskLight);
    ASSERT_TRUE(disk != nullptr);
    delete disk;

    auto* cylinder =
        delegate.CreateFallbackSprim(HdPrimTypeTokens->cylinderLight);
    ASSERT_TRUE(cylinder != nullptr);
    delete cylinder;

    auto* dome = delegate.CreateFallbackSprim(HdPrimTypeTokens->domeLight);
    ASSERT_TRUE(dome != nullptr);
    delete dome;
}

TEST_F(RenderDelegateTest, CreateFallbackBprim)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    auto* bprim =
        delegate.CreateFallbackBprim(HdPrimTypeTokens->renderBuffer);
    ASSERT_TRUE(bprim != nullptr);
    delete bprim;
}

TEST_F(RenderDelegateTest, ResourceRegistry)
{
    auto delegate = Hd_RUZINO_RenderDelegate();
    auto registry = delegate.GetResourceRegistry();
    EXPECT_TRUE(registry != nullptr);
}
