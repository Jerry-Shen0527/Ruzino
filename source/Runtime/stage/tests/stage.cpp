#include <gtest/gtest.h>

#include <stage/stage.hpp>

#include "pxr/usd/usd/prim.h"

using namespace Ruzino;

TEST(Stage, CreateStage)
{
    Stage stage("test_stage_create.usdc");
    auto prim = stage.add_prim(pxr::SdfPath("/root"));
    ASSERT_TRUE(prim);

    auto content = stage.stage_content();
    ASSERT_FALSE(content.empty());
}