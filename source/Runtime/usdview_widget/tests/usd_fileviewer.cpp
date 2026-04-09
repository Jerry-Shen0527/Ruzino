

#include "widgets/usdtree/usd_fileviewer.h"

#include <gtest/gtest.h>

#include "GUI/window.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/sphere.h"
#include "stage/stage.hpp"
#include "widgets/usdview/usdview_widget.hpp"

#ifdef __linux__
#include <cstdlib>
#endif

using namespace Ruzino;

TEST(USDWIDGET, create_widget)
{
#ifdef __linux__
    const char* display = std::getenv("DISPLAY");
    if (!display || std::string(display).empty()) {
        GTEST_SKIP() << "Skipping: no DISPLAY available (headless environment)";
    }
#endif
    auto stage = create_global_stage();

    stage->create_sphere(pxr::SdfPath("/sphere"));

    auto widget = std::make_unique<UsdFileViewer>(stage.get());
    auto window = std::make_unique<Window>();
    window->register_widget(std::move(widget));
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