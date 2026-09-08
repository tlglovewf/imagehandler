#pragma once

#include <QMetaType>
#include <QColor>
#include <QPoint>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector3D>
#include <limits>
#include <memory>
#include <vector>

enum class JobDocumentSource { ImportedFile, GeneratedPreview, ExportedFile };
class JobSpatialIndex;
struct MaskStackGeometry;

enum class JobModelStackState { NotDetected, Loaded, Invalid };

struct JobModelStack {
    QSize sourceSize;
    QSize gridSize;
    QSizeF physicalSize;
    double layerHeight = 0.0;
    int layerCount = 0;
    bool downsampled = false;
    std::shared_ptr<const MaskStackGeometry> solidGeometry;
    std::shared_ptr<const MaskStackGeometry> explodedGeometry;
};

using JobModelStackPtr = std::shared_ptr<const JobModelStack>;

struct JobModel {
    qint64 id = -1;
    QString label;
    QString path;
    JobModelStackState stackState = JobModelStackState::NotDetected;
    JobModelStackPtr stack;
    QString stackError;
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
    // Unique point instance used by picking and image/Job linkage. placeId is
    // the referenced model and may legitimately repeat at different positions.
    qint64 placementId = -1;
    QColor groupColor;
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
    JobBounds sceneBounds;
    std::shared_ptr<JobSpatialIndex> spatialIndex;
    // Prefix sums in placement order; entry 0 is zero and entry N+1 includes point N.
    std::vector<JobCoordinate> idPrefixSums;
    // Indexed by placementId. Empty for manually imported files.
    std::vector<QPoint> linkedCells;
    bool layeredPackageDetected = false;
    QStringList packageWarnings;

    [[nodiscard]] bool isLinked() const noexcept {
        return !linkedCells.empty() && linkedCells.size() == points.size();
    }

    [[nodiscard]] int loadedStackCount() const noexcept {
        return static_cast<int>(std::count_if(models.cbegin(), models.cend(),
            [](const JobModel& model) { return model.stackState == JobModelStackState::Loaded; }));
    }
};

using JobDocumentPtr = std::shared_ptr<JobDocument>;

Q_DECLARE_METATYPE(JobDocumentPtr)
