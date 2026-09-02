#pragma once

#include "io/ExportTypes.h"
#include "io/LayeredPackageTypes.h"

#include <functional>

class LayeredPackageExporter final {
public:
    using Progress = std::function<void(const TaskProgress&)>;

    [[nodiscard]] static int layerCount(const LayeredPackageOptions& options,
                                        QString* error = nullptr);
    [[nodiscard]] static ExportSizeEstimate estimateOutput(
        const LayeredPackageRequest& request);
    [[nodiscard]] static ExportPreflightResult inspect(
        const LayeredPackageRequest& request, const CancellationTokenPtr& token = {},
        const Progress& progress = {});
    [[nodiscard]] static bool exportPackage(
        const LayeredPackageRequest& request, const ExportExecutionOptions& execution,
        const CancellationTokenPtr& token, const Progress& progress, QString* error);
};
