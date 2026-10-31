#pragma once

namespace lm {

class SceneTree;

// Fills `tree` with a small sample level (nested groups of brushes, meshes, lights and entities
// with world-space bounds) for exercising the hierarchy panel and viewport before real level
// loading exists.
void BuildMockLevel(SceneTree& tree);

} // namespace lm
