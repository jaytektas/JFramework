#include <genesis/graphics/RenderPrimitive.h>
#include <cassert>
#include <iostream>

using namespace jf;

void test_primitive_packing() {
    JPrimitiveBuffer buffer;

    uint8_t red[4] = {255, 0, 0, 255};
    buffer.pushRectangle(10, 20, 100, 50, red, 5.0f, 2.0f);

    const auto& cmds = buffer.getCommands();
    assert(cmds.size() == 1);
    assert(cmds[0].kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect);

    const auto& inst = cmds[0].rect;
    assert(inst.rectBounds[0] == 10.0f);
    assert(inst.rectBounds[1] == 20.0f);
    assert(inst.rectBounds[0] + inst.rectBounds[2] == 110.0f); // x + w
    assert(inst.rectBounds[1] + inst.rectBounds[3] == 70.0f);  // y + h
    assert(inst.rectBounds[2] == 100.0f);
    assert(inst.borderRadius == 5.0f);
    assert(inst.borderWidth == 2.0f);
    assert(inst.primitiveType == static_cast<uint32_t>(JPrimitiveType::Rectangle));

    std::cout << "test_primitive_packing passed" << std::endl;
}

void test_memory_alignment() {
    assert(alignof(GpuPrimitiveInstance) == 16);
    assert(sizeof(GpuPrimitiveInstance) % 16 == 0);
    std::cout << "test_memory_alignment passed" << std::endl;
}

void test_draw_order() {
    // Commands must appear in push order — rect, text, rect.
    JPrimitiveBuffer buf;

    uint8_t col[4] = {255,255,255,255};
    buf.pushRectangle(0, 0, 10, 10, col);

    JPrimitiveBuffer::JTextCall tc;
    std::copy(col, col+4, tc.color);
    buf.pushTextCall(std::move(tc));

    buf.pushRectangle(5, 5, 10, 10, col);

    const auto& cmds = buf.getCommands();
    assert(cmds.size() == 3);
    assert(cmds[0].kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect);
    assert(cmds[1].kind == JPrimitiveBuffer::JDrawCommand::JKind::Text);
    assert(cmds[2].kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect);

    std::cout << "test_draw_order passed" << std::endl;
}

int main() {
    test_primitive_packing();
    test_memory_alignment();
    test_draw_order();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
