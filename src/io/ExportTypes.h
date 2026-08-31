#pragma once

#include "domain/Models.h"

#include <QMetaType>
#include <QStringList>

enum class ExportMode { SplitTiles, PrintPackage };

struct PrintExportOptions {
    int canvasX = 1;
    int canvasY = 1;
    int repeatTimes = 10;
    bool reverseY = false;
};

struct ExportRequest {
    ExportMode mode = ExportMode::SplitTiles;
    QString outputDirectory;
    ImageDocumentPtr document;
    SegmentationResult segmentation;
    TraversalMode traversalMode = TraversalMode::SnakeZ;
    PrintExportOptions printOptions;
    bool writeReport = true;
};

struct ExportSizeEstimate {
    bool valid = false;
    QString error;
    qint64 bmpFileCount = 0;
    qint64 estimatedBmpBytes = 0;
    qint64 estimatedTotalBytes = 0;
    qint64 requiredBytes = 0;
};

struct ExportPreflightResult {
    bool valid = false;
    QString blockingError;
    QStringList warnings;
    qint64 bmpFileCount = 0;
    qint64 estimatedBmpBytes = 0;
    qint64 estimatedTotalBytes = 0;
    qint64 requiredBytes = 0;
    qint64 availableBytes = -1;
    bool spaceKnown = false;
    qint64 conflictCount = 0;
    QStringList conflictSamples;
    QString firstOutput;
    QString lastOutput;
    QString targetFingerprint;
};

struct ExportExecutionOptions {
    bool overwrite = false;
    bool writeReport = false;
    QString confirmedTargetFingerprint;
};

Q_DECLARE_METATYPE(ExportPreflightResult)
