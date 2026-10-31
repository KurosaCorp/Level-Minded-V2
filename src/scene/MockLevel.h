#pragma once

#include <cstdint>

namespace lm {

class SceneTree;

// Fills `tree` with a small sample level (nested groups of brushes, meshes, lights and entities
// with world-space bounds) for exercising the hierarchy panel and viewport before real level
// loading exists.
void BuildMockLevel(SceneTree& tree);

// Adds a "Stress" group with `itemCount` items in blocks of 100 (one subgroup per block), laid out
// on a grid next to the mock building. For profiling.
void BuildStressLevel(SceneTree& tree, uint32_t itemCount);

} // namespace lm
