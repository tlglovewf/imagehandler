#pragma once

#include "domain/MaskSliceModels.h"

#include <functional>

class MaskStackMeshBuilder final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static std::shared_ptr<MaskStackGeometry> buildSolid(
        const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
        const Progress& progress = {});
    [[nodiscard]] static std::shared_ptr<MaskStackGeometry> buildExploded(
        const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
        const Progress& progress = {});
};
