#include "imaging/JobPreviewMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kFieldOfViewRadians = 45.0F * kPi / 180.0F;
}

QPointF JobPreviewMath::vulkanNdcToScreen(const QVector2D& ndc, const QSizeF& viewport) {
    return {(ndc.x() + 1.0F) * viewport.width() * 0.5,
            (ndc.y() + 1.0F) * viewport.height() * 0.5};
}

QVector2D JobPreviewMath::screenToVulkanNdc(const QPointF& screen, const QSizeF& viewport) {
    const double width = std::max(1.0, viewport.width());
    const double height = std::max(1.0, viewport.height());
    return {static_cast<float>(screen.x() * 2.0 / width - 1.0),
            static_cast<float>(screen.y() * 2.0 / height - 1.0)};
}

bool JobPreviewMath::project(const QVector3D& point, const QMatrix4x4& viewProjection,
                             const QSizeF& viewport, QPointF* screen, QVector2D* ndc,
                             float* depth) {
    const QVector4D clip = viewProjection * QVector4D(point, 1.0F);
    if (clip.w() <= 0.00001F) return false;
    const QVector3D value = clip.toVector3DAffine();
    if (depth) *depth = value.z();
    if (ndc) *ndc = QVector2D(value.x(), value.y());
    if (screen) *screen = vulkanNdcToScreen(QVector2D(value.x(), value.y()), viewport);
    return value.z() >= 0.0F && value.z() <= 1.0F;
}

bool JobPreviewMath::screenRay(const QPointF& screen, const QSizeF& viewport,
                               const QMatrix4x4& viewProjection, QVector3D* origin,
                               QVector3D* direction) {
    if (!origin || !direction) return false;
    bool invertible = false;
    const QMatrix4x4 inverse = viewProjection.inverted(&invertible);
    if (!invertible) return false;
    const QVector2D ndc = screenToVulkanNdc(screen, viewport);
    QVector4D nearPoint = inverse * QVector4D(ndc.x(), ndc.y(), 0.0F, 1.0F);
    QVector4D farPoint = inverse * QVector4D(ndc.x(), ndc.y(), 1.0F, 1.0F);
    if (std::abs(nearPoint.w()) <= 0.00001F || std::abs(farPoint.w()) <= 0.00001F) return false;
    nearPoint /= nearPoint.w();
    farPoint /= farPoint.w();
    const QVector3D rayDirection = (farPoint - nearPoint).toVector3D();
    if (rayDirection.lengthSquared() <= 1.0e-12F) return false;
    *origin = nearPoint.toVector3D();
    *direction = rayDirection.normalized();
    return true;
}

bool JobPreviewMath::intersectHorizontalPlane(const QVector3D& rayOrigin,
                                              const QVector3D& rayDirection, float planeZ,
                                              QVector3D* intersection) {
    if (!intersection || std::abs(rayDirection.z()) <= 1.0e-6F) return false;
    const float distance = (planeZ - rayOrigin.z()) / rayDirection.z();
    if (distance < 0.0F) return false;
    *intersection = rayOrigin + rayDirection * distance;
    return true;
}

double JobPreviewMath::projectedPointSpacing(std::span<const QPointF> projectedPoints,
                                             size_t pointCount) {
    if (projectedPoints.empty() || pointCount == 0) return 0.0;

    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    size_t validCornerCount = 0;
    for (const QPointF& point : projectedPoints) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) continue;
        minimumX = std::min(minimumX, point.x());
        minimumY = std::min(minimumY, point.y());
        maximumX = std::max(maximumX, point.x());
        maximumY = std::max(maximumY, point.y());
        ++validCornerCount;
    }
    if (validCornerCount == 0) return 0.0;
    if (validCornerCount == 1 || pointCount == 1)
        return std::numeric_limits<double>::max();

    const double width = std::max(0.0, maximumX - minimumX);
    const double height = std::max(0.0, maximumY - minimumY);
    if (width > 0.0 && height > 0.0)
        return std::sqrt((width * height) / static_cast<double>(pointCount));
    const double extent = std::max(width, height);
    return extent > 0.0 ? extent / static_cast<double>(validCornerCount - 1) : 0.0;
}

JobGuideGeometry JobPreviewMath::guideGeometry(const JobDocument& document,
                                               const JobCoordinate& floatingOrigin,
                                               const JobCoordinate& coordinateOrigin) {
    JobGuideGeometry geometry;
    if (!document.bounds.valid || document.points.empty()) return geometry;
    JobBounds displayBounds = document.bounds;
    displayBounds.include(coordinateOrigin);
    geometry.coordinateOrigin = coordinateOrigin.relativeTo(floatingOrigin);
    geometry.minimum = displayBounds.minimum.relativeTo(floatingOrigin);
    geometry.maximum = displayBounds.maximum.relativeTo(floatingOrigin);
    geometry.xStart = {geometry.minimum.x(), geometry.coordinateOrigin.y(), geometry.coordinateOrigin.z()};
    geometry.xEnd = {geometry.maximum.x(), geometry.coordinateOrigin.y(), geometry.coordinateOrigin.z()};
    geometry.yStart = {geometry.coordinateOrigin.x(), geometry.minimum.y(), geometry.coordinateOrigin.z()};
    geometry.yEnd = {geometry.coordinateOrigin.x(), geometry.maximum.y(), geometry.coordinateOrigin.z()};
    const JobCoordinate size = document.bounds.size();
    geometry.xDimension = std::abs(size.x);
    geometry.yDimension = std::abs(size.y);
    geometry.valid = true;
    return geometry;
}

QMatrix4x4 JobPreviewMath::coordinateDisplayTransform(
    const JobCoordinate& floatingOrigin, const JobCoordinate& coordinateOrigin,
    bool reverseCoordinates) {
    QMatrix4x4 transform;
    if (reverseCoordinates) transform.scale(-1.0F, -1.0F, 1.0F);
    transform.translate(static_cast<float>(floatingOrigin.x - coordinateOrigin.x),
                        static_cast<float>(floatingOrigin.y - coordinateOrigin.y));
    return transform;
}

JobCoordinate JobPreviewMath::remapCoordinateOrigin(
    const JobCoordinate& coordinateOrigin, const JobCoordinate& sourceAnchor,
    const JobCoordinate& targetAnchor) {
    return {coordinateOrigin.x + targetAnchor.x - sourceAnchor.x,
            coordinateOrigin.y + targetAnchor.y - sourceAnchor.y,
            coordinateOrigin.z + targetAnchor.z - sourceAnchor.z};
}

void JobTerrainCamera::setSceneBounds(const QVector3D& minimum, const QVector3D& maximum) {
    sceneMinimum_ = minimum;
    sceneMaximum_ = maximum;
    sceneRadius_ = std::max(0.5F, (maximum - minimum).length() * 0.5F);
    setPose(fittedPose());
}

JobCameraPose JobTerrainCamera::fittedPose() const {
    JobCameraPose result;
    result.pivot = (sceneMinimum_ + sceneMaximum_) * 0.5F;
    result.distance = std::max(2.0F, sceneRadius_ / std::sin(kFieldOfViewRadians * 0.5F) * 1.1F);
    result.azimuth = -45.0F;
    result.elevation = 48.0F;
    return result;
}

JobCameraPose JobTerrainCamera::focusPose(const QVector3D& point, bool preserveDistance) const {
    JobCameraPose result = pose_;
    result.pivot = point;
    if (!preserveDistance)
        result.distance = std::max(sceneRadius_ * 0.03F, minimumDistance() * 10.0F);
    return result;
}

void JobTerrainCamera::setPose(const JobCameraPose& pose) {
    pose_ = pose;
    pose_.elevation = continuousOrbitEnabled_
        ? std::remainder(pose_.elevation, 360.0F)
        : std::clamp(pose_.elevation, minimumElevation_, maximumElevation_);
    pose_.distance = std::clamp(pose_.distance, minimumDistance(), maximumDistance());
}

void JobTerrainCamera::setElevationLimits(float minimum, float maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) return;
    minimumElevation_ = std::clamp(minimum, -89.0F, 89.0F);
    maximumElevation_ = std::clamp(maximum, -89.0F, 89.0F);
    if (minimumElevation_ >= maximumElevation_) {
        minimumElevation_ = 5.0F;
        maximumElevation_ = 89.0F;
    }
    if (!continuousOrbitEnabled_)
        pose_.elevation = std::clamp(pose_.elevation, minimumElevation_, maximumElevation_);
}

void JobTerrainCamera::setContinuousOrbitEnabled(bool enabled) {
    continuousOrbitEnabled_ = enabled;
    pose_.elevation = enabled
        ? std::remainder(pose_.elevation, 360.0F)
        : std::clamp(pose_.elevation, minimumElevation_, maximumElevation_);
}

float JobTerrainCamera::minimumDistance() const noexcept {
    return std::max(0.0001F, sceneRadius_ * 0.001F);
}

float JobTerrainCamera::maximumDistance() const noexcept {
    return std::max(100.0F, sceneRadius_ * 1000.0F);
}

void JobTerrainCamera::orbit(const QPointF& pixelDelta) {
    pose_.azimuth -= static_cast<float>(pixelDelta.x()) * 0.25F;
    const float elevation = pose_.elevation + static_cast<float>(pixelDelta.y()) * 0.25F;
    pose_.elevation = continuousOrbitEnabled_
        ? std::remainder(elevation, 360.0F)
        : std::clamp(elevation, minimumElevation_, maximumElevation_);
    pose_.azimuth = std::remainder(pose_.azimuth, 360.0F);
}

void JobTerrainCamera::translate(const QVector3D& delta) {
    const QVector3D next = pose_.pivot + delta;
    if (std::isfinite(next.x()) && std::isfinite(next.y()) && std::isfinite(next.z()))
        pose_.pivot = next;
}

bool JobTerrainCamera::groundIntersection(const QPointF& screen, const QSizeF& viewport,
                                          const QMatrix4x4& viewProjection,
                                          QVector3D* intersection) const {
    QVector3D origin, direction, hit;
    if (!intersection ||
        !JobPreviewMath::screenRay(screen, viewport, viewProjection, &origin, &direction) ||
        !JobPreviewMath::intersectHorizontalPlane(origin, direction, pose_.pivot.z(), &hit))
        return false;
    // Near the horizon a valid forward intersection can still be arbitrarily far
    // away. Do not use such points as navigation anchors.
    const float distance = (hit - eye()).length();
    if (!std::isfinite(distance) || distance > pose_.distance * 10.0F) return false;
    *intersection = hit;
    return true;
}

void JobTerrainCamera::panOnGround(const QPointF& previous, const QPointF& current,
                                  const QSizeF& viewport, const QMatrix4x4& viewProjection) {
    const QPointF pixels = current - previous;
    const float worldPerPixel = std::max(0.000001F,
        2.0F * pose_.distance * std::tan(kFieldOfViewRadians * 0.5F) /
            static_cast<float>(std::max(1.0, viewport.height())));
    QVector3D before, after;
    if (groundIntersection(previous, viewport, viewProjection, &before) &&
        groundIntersection(current, viewport, viewProjection, &after)) {
        QVector3D delta = before - after;
        delta.setZ(0.0F);
        const double limit = std::hypot(pixels.x(), pixels.y()) * worldPerPixel * 10.0;
        if (std::isfinite(delta.length()) && delta.length() <= limit) {
            translate(delta);
            return;
        }
    }
    translate(right() * float(-pixels.x()) * worldPerPixel +
              groundForward() * float(pixels.y()) * worldPerPixel);
}

void JobTerrainCamera::zoomBy(float factor) {
    if (!std::isfinite(factor) || factor <= 0.0F) return;
    pose_.distance = std::clamp(pose_.distance * factor, minimumDistance(), maximumDistance());
}

QVector3D JobTerrainCamera::eye() const {
    const float azimuth = pose_.azimuth * kPi / 180.0F;
    const float elevation = pose_.elevation * kPi / 180.0F;
    const QVector3D direction(std::cos(elevation) * std::cos(azimuth),
                              std::cos(elevation) * std::sin(azimuth),
                              std::sin(elevation));
    return pose_.pivot + direction * pose_.distance;
}

QVector3D JobTerrainCamera::groundForward() const {
    const float azimuth = pose_.azimuth * kPi / 180.0F;
    const float elevation = pose_.elevation * kPi / 180.0F;
    const float direction = continuousOrbitEnabled_ && std::cos(elevation) < 0.0F ? 1.0F : -1.0F;
    return QVector3D(direction * std::cos(azimuth), direction * std::sin(azimuth), 0.0F);
}

QVector3D JobTerrainCamera::right() const {
    const float azimuth = pose_.azimuth * kPi / 180.0F;
    return {-std::sin(azimuth), std::cos(azimuth), 0.0F};
}

QMatrix4x4 JobTerrainCamera::viewMatrix() const {
    QMatrix4x4 view;
    QVector3D up(0.0F, 0.0F, 1.0F);
    if (continuousOrbitEnabled_) {
        const float azimuth = pose_.azimuth * kPi / 180.0F;
        const float elevation = pose_.elevation * kPi / 180.0F;
        up = {-std::sin(elevation) * std::cos(azimuth),
              -std::sin(elevation) * std::sin(azimuth), std::cos(elevation)};
    }
    view.lookAt(eye(), pose_.pivot, up);
    return view;
}
