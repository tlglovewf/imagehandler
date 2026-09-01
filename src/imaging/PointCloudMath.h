#pragma once

#include "domain/PointCloudModels.h"

namespace PointCloudMath {
[[nodiscard]] double heightForNormalized(double normalizedGray,
                                         const PointCloudSettings& settings);
[[nodiscard]] double heightForRaw(double rawGray, double pixelMaximum,
                                  const PointCloudSettings& settings);
[[nodiscard]] QColor interpolateColor(double normalizedGray, const QColor& start,
                                      const QColor& end);
[[nodiscard]] double axisMaximum(qint64 pixelCount, double pixelScale);
[[nodiscard]] double physicalDimension(qint64 pixelCount, double pixelScale);
[[nodiscard]] QString formatAxisMaximum(double value);
}
