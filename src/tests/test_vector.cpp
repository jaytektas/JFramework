#include <genesis/graphics/VectorGraphics.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <cassert>
#include <iostream>
#include <cmath>

using namespace Genesis;
using JVec2 = JVectorCanvas::JVec2;

static bool allTriangles(const JVectorCanvas& vg) { return vg.geometry().size() % 3 == 0; }

struct JBBox { float x0, y0, x1, y1; };
static JBBox bounds(const JVectorCanvas& vg) {
    JBBox b{1e9f, 1e9f, -1e9f, -1e9f};
    for (auto& v : vg.geometry()) {
        b.x0 = std::min(b.x0, v.position[0]); b.y0 = std::min(b.y0, v.position[1]);
        b.x1 = std::max(b.x1, v.position[0]); b.y1 = std::max(b.y1, v.position[1]);
    }
    return b;
}

void test_fill_rect_geometry() {
    JVectorCanvas vg(1.0f);
    vg.fillRect(10, 20, 100, 50, rgb(200, 100, 50));
    assert(!vg.empty());
    assert(allTriangles(vg));
    JBBox b = bounds(vg);
    // Within the rect plus the 1px AA fringe.
    assert(b.x0 >= 10 - 2 && b.y0 >= 20 - 2);
    assert(b.x1 <= 110 + 2 && b.y1 <= 70 + 2);
    std::cout << "test_fill_rect_geometry passed\n";
}

void test_circle_bounds() {
    JVectorCanvas vg(1.0f);
    vg.fillCircle(200, 200, 80, rgb(0, 180, 120));
    assert(allTriangles(vg) && !vg.empty());
    JBBox b = bounds(vg);
    assert(b.x0 >= 120 - 2 && b.x1 <= 280 + 2);
    assert(b.y0 >= 120 - 2 && b.y1 <= 280 + 2);
    // Should be a fair number of triangles for a smooth circle.
    assert(vg.geometry().size() > 30 * 3);
    std::cout << "test_circle_bounds passed\n";
}

void test_stroke_line() {
    JVectorCanvas vg(1.0f);
    vg.drawLine(0, 0, 100, 0, 4.0f, rgb(255, 255, 0));
    assert(allTriangles(vg) && !vg.empty());
    JBBox b = bounds(vg);
    // 4px wide line on y=0 → spans roughly y in [-2-aa, 2+aa].
    assert(b.y0 >= -4 && b.y1 <= 4);
    assert(b.x1 >= 99);
    std::cout << "test_stroke_line passed\n";
}

void test_polyline_path() {
    JVectorCanvas vg(1.0f);
    vg.beginPath();
    vg.moveTo(0, 100);
    vg.lineTo(50, 40);
    vg.lineTo(100, 80);
    vg.lineTo(150, 20);
    vg.stroke(3.0f, rgb(220, 220, 40), JLineCap::Round);
    assert(allTriangles(vg) && !vg.empty());
    std::cout << "test_polyline_path passed\n";
}

void test_linear_gradient_endpoints() {
    JVectorCanvas vg(0.0f);  // no fringe so we only get the exact corner vertices
    // rect corners coincide with the gradient axis endpoints.
    JPaint g = JPaint::linear(0, 0, rgb(255, 0, 0), 100, 0, rgb(0, 0, 255));
    vg.fillRect(0, 0, 100, 100, g);
    bool sawRed = false, sawBlue = false;
    for (auto& v : vg.geometry()) {
        if (std::fabs(v.position[0] - 0)   < 0.5f && v.color[0] > 200 && v.color[2] < 50) sawRed = true;
        if (std::fabs(v.position[0] - 100) < 0.5f && v.color[2] > 200 && v.color[0] < 50) sawBlue = true;
    }
    assert(sawRed && sawBlue);
    std::cout << "test_linear_gradient_endpoints passed\n";
}

void test_aa_fringe_present() {
    JVectorCanvas vg(1.0f);
    vg.fillCircle(50, 50, 40, rgb(255, 255, 255));
    bool sawTransparent = false, sawOpaque = false;
    for (auto& v : vg.geometry()) {
        if (v.color[3] == 0)   sawTransparent = true;   // fringe outer edge
        if (v.color[3] == 255) sawOpaque = true;        // interior
    }
    assert(sawTransparent && sawOpaque);
    std::cout << "test_aa_fringe_present passed\n";
}

void test_flush_to_buffer() {
    JVectorCanvas vg;
    vg.fillCircle(10, 10, 5, rgb(1, 2, 3));
    JPrimitiveBuffer buf;
    vg.flush(buf);
    int geomCmds = 0;
    for (auto& c : buf.getCommands())
        if (c.kind == JPrimitiveBuffer::JDrawCommand::JKind::Geometry) ++geomCmds;
    assert(geomCmds == 1);
    assert(buf.getCommands()[0].geom.verts.size() == vg.geometry().size());
    std::cout << "test_flush_to_buffer passed\n";
}

void test_pie_and_ring() {
    JVectorCanvas vg;
    vg.fillPie(100, 100, 60, -1.0f, 1.0f, rgb(120, 60, 200));
    assert(allTriangles(vg) && !vg.empty());
    size_t afterPie = vg.geometry().size();
    vg.fillRing(100, 100, 40, 60, 0.0f, 3.14159f, rgb(60, 200, 120));
    assert(allTriangles(vg) && vg.geometry().size() > afterPie);
    std::cout << "test_pie_and_ring passed\n";
}

int main() {
    test_fill_rect_geometry();
    test_circle_bounds();
    test_stroke_line();
    test_polyline_path();
    test_linear_gradient_endpoints();
    test_aa_fringe_present();
    test_flush_to_buffer();
    test_pie_and_ring();
    std::cout << "All VectorGraphics tests passed.\n";
    return 0;
}
