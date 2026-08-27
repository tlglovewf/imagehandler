#pragma once

#include "domain/JobModels.h"
#include "domain/Models.h"

#include <QMatrix4x4>
#include <memory>
#include <vector>

class JobSpatialIndex {
public:
    static std::shared_ptr<JobSpatialIndex> build(const std::vector<JobPoint>& points,
                                                  const CancellationTokenPtr& token);
    static bool prepare(JobDocument* document, const CancellationTokenPtr& token);

    void queryRay(const QVector3D& rayOrigin, const QVector3D& rayDirection,
                  float angularRadius, const JobCoordinate& coordinateOrigin,
                  std::vector<quint32>* candidates, size_t maximumCandidates = 8192) const;
    void queryVisible(const QMatrix4x4& viewProjection, const JobCoordinate& coordinateOrigin,
                      std::vector<quint32>* candidates,
                      size_t maximumCandidates = 200000) const;

private:
    struct Node {
        JobCoordinate minimum;
        JobCoordinate maximum;
        quint32 begin = 0;
        quint32 count = 0;
        qint32 left = -1;
        qint32 right = -1;
        [[nodiscard]] bool leaf() const noexcept { return left < 0; }
    };

    qint32 buildNode(const std::vector<JobPoint>& points, quint32 begin, quint32 count,
                     const CancellationTokenPtr& token);
    [[nodiscard]] float nodeRayPriority(const Node& node, const QVector3D& origin,
                                        const QVector3D& direction, float angularRadius,
                                        const JobCoordinate& coordinateOrigin) const;
    bool nodeVisible(const Node& node, const QMatrix4x4& matrix,
                     const JobCoordinate& coordinateOrigin) const;

    std::vector<quint32> indices_;
    std::vector<Node> nodes_;
};
