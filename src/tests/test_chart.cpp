#include <j/graphics/Chart.h>
#include <j/graphics/RenderPrimitive.h>
#include <cassert>
#include <iostream>
#include <cmath>

using namespace jf;

static int geomCommands(const JPrimitiveBuffer& buf) {
    int n = 0;
    for (auto& c : buf.getCommands())
        if (c.kind == JPrimitiveBuffer::JDrawCommand::JKind::Geometry) ++n;
    return n;
}

void test_basic_render() {
    JChart c;
    c.setRect(0, 0, 400, 300);
    c.setTitle("RPM");
    int s = c.addSeries("rpm", rgb(80, 200, 255), 2.0f, /*area=*/true);
    for (int i = 0; i <= 100; ++i) c.addPoint(s, i * 0.1, std::sin(i * 0.1) * 50 + 50);
    JPrimitiveBuffer buf;
    c.render(buf);
    // Grid + series (+ area) produce geometry commands; no text atlas in tests.
    assert(geomCommands(buf) >= 1);
    // All emitted geometry must be valid triangle lists.
    for (auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::Geometry)
            assert(cmd.geom.verts.size() % 3 == 0 && !cmd.geom.verts.empty());
    std::cout << "test_basic_render passed\n";
}

void test_xwindow_scroll() {
    JChart c;
    c.setRect(0, 0, 200, 100);
    int s = c.addSeries("v", rgb(0, 200, 120));
    c.setXWindow(2.0);
    for (int i = 0; i <= 100; ++i) c.addPoint(s, i * 0.1, i);
    auto& pts = c.series(s).pts;
    assert(!pts.empty());
    // Oldest retained sample must be within the 2.0 window of the newest.
    assert(pts.back().x - pts.front().x <= 2.0 + 1e-3);
    std::cout << "test_xwindow_scroll passed\n";
}

void test_multi_series() {
    JChart c;
    c.setRect(0, 0, 500, 250);
    int a = c.addSeries("a", rgb(255, 80, 80));
    int b = c.addSeries("b", rgb(80, 80, 255));
    for (int i = 0; i < 50; ++i) { c.addPoint(a, i, std::sin(i * 0.2)); c.addPoint(b, i, std::cos(i * 0.2)); }
    assert(c.seriesCount() == 2);
    JPrimitiveBuffer buf;
    c.render(buf);
    assert(geomCommands(buf) >= 1);
    std::cout << "test_multi_series passed\n";
}

void test_fixed_range_and_empty() {
    JChart c;
    c.setRect(10, 10, 300, 150);
    c.setXRange(0, 10);
    c.setYRange(-1, 1);
    JPrimitiveBuffer buf;
    c.render(buf);   // no series — must not crash; grid still drawn
    assert(geomCommands(buf) >= 1);
    std::cout << "test_fixed_range_and_empty passed\n";
}

void test_series_types() {
    JChart c;
    c.setRect(0, 0, 500, 300);
    int line = c.addSeries("line", rgb(0, 200, 255));
    c.series(line).markers = true;
    int bar = c.addSeries("bar", rgb(200, 120, 0));
    c.series(bar).type = JSeriesType::Bar;
    int sc = c.addSeries("scatter", rgb(120, 220, 120));
    c.series(sc).type = JSeriesType::Scatter;
    for (int i = 0; i < 20; ++i) {
        c.addPoint(line, i, std::sin(i * 0.3) * 10 + 20);
        c.addPoint(bar, i, (i % 5) + 1);
        c.addPoint(sc, i, std::cos(i * 0.3) * 8 + 15);
    }
    JPrimitiveBuffer buf;
    c.render(buf);
    assert(geomCommands(buf) >= 1);
    for (auto& cmd : buf.getCommands())
        if (cmd.kind == JPrimitiveBuffer::JDrawCommand::JKind::Geometry)
            assert(cmd.geom.verts.size() % 3 == 0);
    std::cout << "test_series_types passed\n";
}

void test_secondary_axis() {
    JChart c;
    c.setRect(0, 0, 500, 300);
    int a = c.addSeries("primary", rgb(0, 200, 255));
    int b = c.addSeries("secondary", rgb(255, 160, 60));
    c.series(b).secondary = true;
    for (int i = 0; i < 30; ++i) { c.addPoint(a, i, i * 10.0); c.addPoint(b, i, std::sin(i * 0.2)); }
    JPrimitiveBuffer buf;
    c.render(buf);
    assert(geomCommands(buf) >= 1);
    std::cout << "test_secondary_axis passed\n";
}

void test_log_scale() {
    JChart c;
    c.setRect(0, 0, 400, 250);
    c.setLogY(true);
    int s = c.addSeries("log", rgb(200, 200, 60));
    for (int i = 1; i <= 50; ++i) c.addPoint(s, i, std::pow(1.3, i));
    JPrimitiveBuffer buf;
    c.render(buf);
    assert(geomCommands(buf) >= 1);
    std::cout << "test_log_scale passed\n";
}

void test_hover_and_zoom() {
    JChart c;
    c.setRect(0, 0, 600, 300);
    c.setXRange(0, 100);
    int s = c.addSeries("v", rgb(0, 200, 255));
    for (int i = 0; i <= 100; ++i) c.addPoint(s, i, std::sin(i * 0.1));
    JPrimitiveBuffer buf;
    c.render(buf);
    // pixelToDataX is monotonic increasing.
    assert(c.pixelToDataX(200) < c.pixelToDataX(500));
    // Same pixel span maps to a smaller data span after zooming in.
    double span0 = c.pixelToDataX(500) - c.pixelToDataX(200);
    c.zoomX(0.5, 50.0);
    JPrimitiveBuffer buf2; c.render(buf2);
    double span1 = c.pixelToDataX(500) - c.pixelToDataX(200);
    assert(span1 < span0);
    // Hover renders without crashing and adds geometry (crosshair).
    c.setHover(300, 150);
    JPrimitiveBuffer buf3; c.render(buf3);
    assert(geomCommands(buf3) >= 1);
    std::cout << "test_hover_and_zoom passed\n";
}

int main() {
    test_basic_render();
    test_xwindow_scroll();
    test_multi_series();
    test_fixed_range_and_empty();
    test_series_types();
    test_secondary_axis();
    test_log_scale();
    test_hover_and_zoom();
    std::cout << "All JChart tests passed.\n";
    return 0;
}
