#pragma once

#include "domain/JobModels.h"

#include <QMatrix4x4>
#include <QPointF>
#include <QSizeF>
#include <QVector2D>
#include <QVector3D>

#include <span>

struct JobGuideGeometry {
    QVector3D coordinateOrigin;
    QVector3D minimum;
    QVector3D maximum;
    QVector3D xStart;
    QVector3D xEnd;
    QVector3D yStart;
    QVector3D yEnd;
    double xDimension = 0.0;
    double yDimension = 0.0;
    bool valid = false;
};

struct JobCameraPose {
    QVector3D pivot;
    float distance = 10.0F;
    float azimuth = -45.0F;
    float elevation = 48.0F;
};

namespace JobPreviewMath {

[[nodiscard]] QPointF vulkanNdcToScreen(const QVector2D& ndc, const QSizeF& viewport);
[[nodiscard]] QVector2D screenToVulkanNdc(const QPointF& screen, const QSizeF& viewport);
[[nodiscard]] bool project(const QVector3D& point, const QMatrix4x4& viewProjection,
                           const QSizeF& viewport, QPointF* screen,
                           QVector2D* ndc = nullptr, float* depth = nullptr);
[[nodiscard]] bool screenRay(const QPointF& screen, const QSizeF& viewport,
                             const QMatrix4x4& viewProjection, QVector3D* origin,
                             QVector3D* direction);
[[nodiscard]] bool intersectHorizontalPlane(const QVector3D& rayOrigin,
                                            const QVector3D& rayDirection, float planeZ,
                                            QVector3D* intersection);
[[nodiscard]] double projectedPointSpacing(std::span<const QPointF> projectedPoints,
                                           size_t pointCount);
[[nodiscard]] JobGuideGeometry guideGeometry(const JobDocument& document,
                                             const JobCoordinate& floatingOrigin,
                                             const JobCoordinate& coordinateOrigin = {});

} // namespace JobPreviewMath

class JobTerrainCamera final {
public:
    void setSceneBounds(const QVector3D& minimum, const QVector3D& maximum);
    [[nodiscard]] JobCameraPose fittedPose() const;
    [[nodiscard]] JobCameraPose focusPose(const QVector3D& point,
                                          bool preserveDistance = false) const;

    void setPose(const JobCameraPose& pose);
    [[nodiscard]] const JobCameraPose& pose() const noexcept { return pose_; }
    [[nodiscard]] float sceneRadius() const noexcept { return sceneRadius_; }
    [[nodiscard]] float minimumDistance() const noexcept;
    [[nodiscard]] float maximumDistance() const noexcept;
    void setElevationLimits(float minimum, float maximum);
    void setContinuousOrbitEnabled(bool enabled);
    [[nodiscard]] bool continuousOrbitEnabled() const noexcept { return continuousOrbitEnabled_; }
    [[nodiscard]] float minimumElevation() const noexcept { return minimumElevation_; }
    [[nodiscard]] float maximumElevation() const noexcept { return maximumElevation_; }

    void orbit(const QPointF& pixelDelta);
    void translate(const QVector3D& delta);
    [[nodiscard]] bool groundIntersection(const QPointF& screen, const QSizeF& viewport,
                                           const QMatrix4x4& viewProjection,
                                           QVector3D* intersection) const;
    void panOnGround(const QPointF& previous, const QPointF& current,
                     const QSizeF& viewport, const QMatrix4x4& viewProjection);
    void zoomBy(float factor);

    [[nodiscard]] QVector3D eye() const;
    [[nodiscard]] QVector3D groundForward() const;
    [[nodiscard]] QVector3D right() const;
    [[nodiscard]] QMatrix4x4 viewMatrix() const;

private:
    JobCameraPose pose_;
    QVector3D sceneMinimum_{-0.5F, -0.5F, -0.5F};
    QVector3D sceneMaximum_{0.5F, 0.5F, 0.5F};
    float sceneRadius_ = 1.0F;
    float minimumElevation_ = 5.0F;
    float maximumElevation_ = 89.0F;
    bool continuousOrbitEnabled_ = false;
};
