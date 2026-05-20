#pragma once

namespace Renderer {
namespace Input {

    // Installs all available Android input hooks. Each hook feeds the shared
    // renderer input queue, and duplicate events are filtered automatically.
    bool Init();

} // namespace Input
} // namespace Renderer
