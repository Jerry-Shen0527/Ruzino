#include <gtest/gtest.h>

#include "RHI/rhi.hpp"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/tokens.h"
#include "renderDelegate.h"
#include "spdlog/spdlog.h"

using namespace Ruzino;
using namespace pxr;

class RenderBufferTest : public ::testing::Test {
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

TEST_F(RenderBufferTest, CreateViaDelegate)
{
    auto* bprim = delegate_->CreateBprim(
        HdPrimTypeTokens->renderBuffer, SdfPath("/aov/color"));
    ASSERT_TRUE(bprim != nullptr);

    auto* rb = static_cast<HdRenderBuffer*>(bprim);
    EXPECT_EQ(rb->GetWidth(), 0u);
    EXPECT_EQ(rb->GetHeight(), 0u);
    EXPECT_EQ(rb->GetDepth(), 1u);
    EXPECT_FALSE(rb->IsConverged());
    EXPECT_FALSE(rb->IsMapped());

    delegate_->DestroyBprim(bprim);
}

TEST_F(RenderBufferTest, ConvergenceState)
{
    auto* bprim = delegate_->CreateBprim(
        HdPrimTypeTokens->renderBuffer, SdfPath("/aov/conv"));
    ASSERT_TRUE(bprim != nullptr);

    auto* rb = static_cast<HdRenderBuffer*>(bprim);
    EXPECT_FALSE(rb->IsConverged());

    delegate_->DestroyBprim(bprim);
}

TEST_F(RenderBufferTest, CreateFallback)
{
    auto* bprim =
        delegate_->CreateFallbackBprim(HdPrimTypeTokens->renderBuffer);
    ASSERT_TRUE(bprim != nullptr);
    delete bprim;
}

TEST_F(RenderBufferTest, AovDescriptorColor)
{
    auto desc = delegate_->GetDefaultAovDescriptor(HdAovTokens->color);
    EXPECT_EQ(desc.format, HdFormatFloat32Vec4);
    EXPECT_TRUE(desc.clearValue.IsHolding<GfVec4f>());
}

TEST_F(RenderBufferTest, AovDescriptorDepth)
{
    auto desc = delegate_->GetDefaultAovDescriptor(HdAovTokens->depth);
    EXPECT_EQ(desc.format, HdFormatFloat32);
    EXPECT_TRUE(desc.clearValue.IsHolding<float>());
}

TEST_F(RenderBufferTest, AovDescriptorNormal)
{
    auto desc = delegate_->GetDefaultAovDescriptor(HdAovTokens->normal);
    EXPECT_EQ(desc.format, HdFormatFloat32Vec3);
}

TEST_F(RenderBufferTest, AovDescriptorPrimvar)
{
    TfToken primvarToken("primvars:myCustom");
    auto desc = delegate_->GetDefaultAovDescriptor(primvarToken);
    EXPECT_EQ(desc.format, HdFormatFloat32Vec3);
}

TEST_F(RenderBufferTest, AovDescriptorUnknown)
{
    auto desc = delegate_->GetDefaultAovDescriptor(TfToken("unknownAov"));
    EXPECT_EQ(desc.format, HdFormatInvalid);
}
