#include "ViewLayout.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace
{

// Layout values land on whole pixels by construction, so these compare
// exactly on purpose; only ClientToImage divides and needs a tolerance.
ViewLayout Fit(uint32_t imageWidth, uint32_t imageHeight, float clientWidth, float clientHeight)
{
    return ComputeViewLayout(imageWidth, imageHeight, clientWidth, clientHeight, ZoomMode::Fit,
                             1.0f, 0.0f, 0.0f);
}

}  // namespace

TEST_CASE("Fit never upscales", "[viewlayout]")
{
    const ViewLayout layout = Fit(100, 50, 800, 600);
    REQUIRE(layout.scale == 1.0f);
    REQUIRE(layout.displayWidth == 100.0f);
    REQUIRE(layout.displayHeight == 50.0f);
    // Centered, with no panning available.
    REQUIRE(layout.destX == 350.0f);
    REQUIRE(layout.destY == 275.0f);
    REQUIRE(layout.maxPanX == 0.0f);
    REQUIRE(layout.maxPanY == 0.0f);
}

TEST_CASE("Fit shrinks on the tighter axis and keeps the aspect ratio", "[viewlayout]")
{
    const ViewLayout layout = Fit(2000, 1000, 800, 600);
    REQUIRE(layout.scale == 0.4f);  // width-limited: 800 / 2000
    REQUIRE(layout.displayWidth == 800.0f);
    REQUIRE(layout.displayHeight == 400.0f);
    REQUIRE(layout.destX == 0.0f);
    REQUIRE(layout.destY == 100.0f);  // centered vertically
}

TEST_CASE("An exactly fitting image does not gain scrollbars", "[viewlayout]")
{
    // Rounding is why: 0.1f * 8000 is 800.00006 as a raw product.
    const ViewLayout layout =
        ComputeViewLayout(8000, 6000, 800.0f, 600.0f, ZoomMode::Custom, 0.1f, 0.0f, 0.0f);
    REQUIRE(layout.displayWidth == 800.0f);
    REQUIRE(layout.displayHeight == 600.0f);
    REQUIRE(layout.maxPanX == 0.0f);
    REQUIRE(layout.maxPanY == 0.0f);
}

TEST_CASE("Actual size pans within bounds", "[viewlayout]")
{
    const ViewLayout layout = ComputeViewLayout(2000, 1500, 800.0f, 600.0f, ZoomMode::ActualSize,
                                                1.0f, 5000.0f, -40.0f);
    REQUIRE(layout.scale == 1.0f);
    REQUIRE(layout.maxPanX == 1200.0f);
    REQUIRE(layout.maxPanY == 900.0f);
    REQUIRE(layout.panX == 1200.0f);  // clamped to maxPan
    REQUIRE(layout.panY == 0.0f);     // clamped to zero
    REQUIRE(layout.destX == -1200.0f);
    REQUIRE(layout.destY == 0.0f);
}

TEST_CASE("An empty image yields the default layout", "[viewlayout]")
{
    const ViewLayout layout = Fit(0, 0, 800, 600);
    REQUIRE(layout.scale == 0.0f);
    REQUIRE(layout.displayWidth == 0.0f);
    REQUIRE(layout.displayHeight == 0.0f);
}

TEST_CASE("ClientToImage inverts the layout transform", "[viewlayout]")
{
    const ViewLayout layout =
        ComputeViewLayout(1000, 800, 400.0f, 300.0f, ZoomMode::Custom, 2.0f, 100.0f, 50.0f);
    const D2D1_POINT_2F image = layout.ClientToImage(layout.destX + 200.0f, layout.destY + 100.0f);
    REQUIRE_THAT(image.x, Catch::Matchers::WithinAbs(100.0, 1e-4));
    REQUIRE_THAT(image.y, Catch::Matchers::WithinAbs(50.0, 1e-4));
}

TEST_CASE("FixedZoomScale ignores the zoom factor at actual size", "[viewlayout]")
{
    REQUIRE(FixedZoomScale(ZoomMode::ActualSize, 3.0f) == 1.0f);
    REQUIRE(FixedZoomScale(ZoomMode::Custom, 3.0f) == 3.0f);
}
