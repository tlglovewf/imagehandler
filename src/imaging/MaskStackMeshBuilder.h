#pragma once

#include "domain/MaskSliceModels.h"

#include <cstddef>
#include <functional>

class MaskStackMeshBuilder final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static std::shared_ptr<MaskStackGeometry> buildSolid(
        const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
        const Progress& progress = {});
    [[nodiscard]] static std::shared_ptr<MaskStackGeometry> buildExploded(
        const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
        const Progress& progress = {},
        std::size_t maximumQuads = 2'000'000);
};
