#include "widgets/usdview/usdview_widget.hpp"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "GUI/window.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/sphere.h"
#include "stage/stage.hpp"
#include "widgets/usdtree/usd_fileviewer.h"

using namespace Ruzino;

int main()
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("%^[%T] %n: %v%$");

    auto stage = create_global_stage("../../Assets/test_scene.usd");
    stage->create_sphere(pxr::SdfPath("/sphere"));

    auto widget = std::make_unique<UsdFileViewer>(stage.get());
    auto render = std::make_unique<UsdviewEngine>(stage.get());

    auto window = std::make_unique<Window>();

    window->register_widget(std::move(widget));
    window->register_widget(std::move(render));
    window->register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });

    window->run();

    window.reset();
}
