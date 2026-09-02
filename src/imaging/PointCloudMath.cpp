#include "imaging/PointCloudMath.h"

#include <algorithm>
#include <cmath>

namespace PointCloudMath {
double heightForNormalized(double normalizedGray, const PointCloudSettings& settings) {
    const double normalized = std::clamp(normalizedGray, 0.0, 1.0);
    return (settings.invertHeight ? 1.0 - normalized : normalized) * settings.maximumHeight;
}

double heightForRaw(double rawGray, double pixelMaximum, const PointCloudSettings& settings) {
    if (!std::isfinite(rawGray) || !std::isfinite(pixelMaximum) || pixelMaximum <= 0.0)
        return heightForNormalized(0.0, settings);
    return heightForNormalized(rawGray / pixelMaximum, settings);
}

bool maskLayerActive(double height, int zeroBasedLayer, double layerHeight) {
    if (!std::isfinite(height) || height < 0.0 || zeroBasedLayer < 0 ||
        !std::isfinite(layerHeight) || layerHeight <= 0.0) return false;
    return zeroBasedLayer == 0 || height > zeroBasedLayer * layerHeight;
}

QColor interpolateColor(double normalizedGray, const QColor& start, const QColor& end) {
    const double t = std::clamp(normalizedGray, 0.0, 1.0);
    const auto channel = [t](int first, int last) {
        return std::clamp(qRound(first + (last - first) * t), 0, 255);
    };
    return QColor(channel(start.red(), end.red()), channel(start.green(), end.green()),
                  channel(start.blue(), end.blue()), channel(start.alpha(), end.alpha()));
}

double axisMaximum(qint64 pixelCount, double pixelScale) {
    // Axis labels describe the full physical extent, not the coordinate of
    // the last pixel centre. A 220-pixel span at 0.25 units/pixel is 55.
    if (pixelCount <= 0 || !std::isfinite(pixelScale) || pixelScale <= 0.0) return 0.0;
    return static_cast<double>(pixelCount) * pixelScale;
}

double physicalDimension(qint64 pixelCount, double pixelScale) {
    if (pixelCount <= 0 || !std::isfinite(pixelScale) || pixelScale <= 0.0) return 0.0;
    return static_cast<double>(pixelCount) * pixelScale;
}

QString formatAxisMaximum(double value) {
    QString text = QString::number(value, 'f', 6);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return text;
}
}
