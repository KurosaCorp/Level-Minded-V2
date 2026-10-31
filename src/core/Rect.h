#pragma once

namespace lm {

// Axis-aligned rectangle in pixels (top-left origin).
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
    bool IsEmpty() const { return width < 1.0f || height < 1.0f; }
    float Aspect() const { return height > 0.0f ? width / height : 1.0f; }
};

} // namespace lm
