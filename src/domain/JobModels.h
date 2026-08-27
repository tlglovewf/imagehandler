#pragma once

#include <QMetaType>
#include <QPoint>
#include <QString>
#include <QVector3D>
#include <limits>
#include <memory>
#include <vector>

enum class JobDocumentSource { ImportedFile, GeneratedPreview, ExportedFile };
class JobSpatialIndex;

struct JobModel {
    qint64 id = -1;
    QString label;
    QString path;
};

struct JobCoordinate {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] QVector3D relativeTo(const JobCoordinate& origin) const noexcept {
        return QVector3D(static_cast<float>(x - origin.x), static_cast<float>(y - origin.y),
                         static_cast<float>(z - origin.z));
    }

    friend bool operator==(const JobCoordinate&, const JobCoordinate&) = default;
};

struct JobPoint {
    qint64 placeId = -1;
    JobCoordinate position;
    qint64 commandIndex = -1;
};

struct JobBounds {
    JobCoordinate minimum;
    JobCoordinate maximum;
    bool valid = false;

    void include(const JobCoordinate& point) noexcept {
        if (!valid) {
            minimum = maximum = point;
            valid = true;
            return;
        }
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
    }

    [[nodiscard]] JobCoordinate center() const noexcept {
        return {(minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5,
                (minimum.z + maximum.z) * 0.5};
    }
    [[nodiscard]] JobCoordinate size() const noexcept {
        return {maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z};
    }
};

struct JobDocument {
    JobDocumentSource source = JobDocumentSource::ImportedFile;
    QString sourcePath;
    std::vector<JobModel> models;
    std::vector<JobPoint> points;
    JobBounds bounds;
    std::shared_ptr<JobSpatialIndex> spatialIndex;
    // Prefix sums in place.id order; entry 0 is zero and entry N+1 includes id N.
    std::vector<JobCoordinate> idPrefixSums;
    // Indexed by place.id. Empty for manually imported files.
    std::vector<QPoint> linkedCells;

    [[nodiscard]] bool isLinked() const noexcept {
        return !linkedCells.empty() && linkedCells.size() == points.size();
    }
};

using JobDocumentPtr = std::shared_ptr<JobDocument>;

Q_DECLARE_METATYPE(JobDocumentPtr)
