#include "io/SplitExporter.h"
#include "io/BmpStreamWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <climits>
#include <new>

std::vector<QPoint> SplitExporter::traversal(int rows, int columns, TraversalMode mode) {
    std::vector<QPoint> order;
    if (rows <= 0 || columns <= 0) return order;
    const qint64 count = qint64(rows) * columns;
    if (count <= 0 || quint64(count) > order.max_size()) return order;
    try {
        order.reserve(static_cast<size_t>(count));
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (mode == TraversalMode::SnakeZ) {
        for (int r = 0; r < rows; ++r) {
            if (r % 2 == 0) for (int c = 0; c < columns; ++c) order.emplace_back(c, r);
            else for (int c = columns - 1; c >= 0; --c) order.emplace_back(c, r);
        }
        return order;
    }
    int left=0,right=columns-1,top=0,bottom=rows-1;
    while(left<=right&&top<=bottom) {
        for(int c=left;c<=right;++c) order.emplace_back(c,top); ++top;
        for(int r=top;r<=bottom;++r) order.emplace_back(right,r); --right;
        if(top<=bottom) { for(int c=right;c>=left;--c) order.emplace_back(c,bottom); --bottom; }
        if(left<=right) { for(int r=bottom;r>=top;--r) order.emplace_back(left,r); ++left; }
    }
    return order;
}

qint64 SplitExporter::traversalIndex(int rows, int columns, TraversalMode mode,
                                     int row, int column) {
    if (rows <= 0 || columns <= 0 || row < 0 || row >= rows ||
        column < 0 || column >= columns) {
        return -1;
    }
    if (mode == TraversalMode::SnakeZ) {
        return qint64(row) * columns +
               (row % 2 == 0 ? column : columns - 1 - column);
    }

    const int layer = std::min({row, column, rows - 1 - row, columns - 1 - column});
    const int top = layer;
    const int left = layer;
    const int bottom = rows - 1 - layer;
    const int right = columns - 1 - layer;
    const qint64 innerRows = rows - 2LL * layer;
    const qint64 innerColumns = columns - 2LL * layer;
    const qint64 prefix = qint64(rows) * columns - innerRows * innerColumns;
    const qint64 width = right - left + 1LL;
    const qint64 height = bottom - top + 1LL;

    if (row == top) return prefix + column - left;
    if (column == right) return prefix + width + row - top - 1;
    if (row == bottom) {
        return prefix + width + height - 1 + right - column - 1;
    }
    return prefix + width + height - 1 + width - 1 + bottom - row - 1;
}

QPoint SplitExporter::nextTraversalCell(int rows, int columns, TraversalMode mode,
                                        int row, int column) {
    if (traversalIndex(rows, columns, mode, row, column) < 0) return {-1, -1};
    if (mode == TraversalMode::SnakeZ) {
        if (row % 2 == 0) {
            if (column + 1 < columns) return {column + 1, row};
        } else if (column > 0) {
            return {column - 1, row};
        }
        return row + 1 < rows ? QPoint(column, row + 1) : QPoint(-1, -1);
    }

    const int layer = std::min({row, column, rows - 1 - row, columns - 1 - column});
    const int top = layer;
    const int left = layer;
    const int bottom = rows - 1 - layer;
    const int right = columns - 1 - layer;
    if (row == top) {
        if (column < right) return {column + 1, row};
        if (row < bottom) return {column, row + 1};
        return {-1, -1};
    }
    if (column == right) {
        if (row < bottom) return {column, row + 1};
        if (column > left) return {column - 1, row};
        return {-1, -1};
    }
    if (row == bottom) {
        if (column > left) return {column - 1, row};
        if (row > top + 1) return {column, row - 1};
    } else if (column == left && row > top + 1) {
        return {column, row - 1};
    }
    if (top + 1 <= bottom - 1 && left + 1 <= right - 1) {
        return {left + 1, top + 1};
    }
    return {-1, -1};
}

bool SplitExporter::exportTiles(const QString& outputDirectory, const ImageDocumentPtr& document,
                                const SegmentationResult& segmentation, TraversalMode mode,
                                bool overwrite, const CancellationTokenPtr& token,
                                const Progress& progress, QString* error) {
    if (!document || !document->image || segmentation.settings.cellWidth <= 0 ||
        segmentation.settings.cellHeight <= 0) {
        if (error) *error = QStringLiteral("分割文档或参数无效。");
        return false;
    }
    const qint64 cellCount = qint64(segmentation.rows) * segmentation.columns;
    if (cellCount <= 0) {
        if (error) *error = QStringLiteral("分割数量无效。");
        return false;
    }
    const qint64 expectedColumns = (document->metadata.width + segmentation.settings.cellWidth - 1) /
                                   segmentation.settings.cellWidth;
    const qint64 expectedRows = (document->metadata.height + segmentation.settings.cellHeight - 1) /
                                segmentation.settings.cellHeight;
    if (segmentation.columns != expectedColumns || segmentation.rows != expectedRows) {
        if (error) *error = QStringLiteral("分割网格与当前图像尺寸不匹配。");
        return false;
    }
    QDir output(outputDirectory);
    if (!output.exists() && !output.mkpath(".")) { if(error)*error=QStringLiteral("无法创建输出目录。"); return false; }
    const auto order = traversal(segmentation.rows, segmentation.columns, mode);
    if (order.empty()) { if(error)*error=QStringLiteral("没有可导出的完整单元。"); return false; }
    const QString tempName = QStringLiteral(".image_handler_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(tempName)) { if(error)*error=QStringLiteral("无法创建临时导出目录。"); return false; }
    QDir temp(output.filePath(tempName));
    const auto cleanup = [&] { return temp.removeRecursively(); };
    const qint64 totalWork = qint64(order.size()) * 1001;
    qint64 lastReportedPermille = -1;
    const auto reportProgress = [&](const QString& phase, qint64 completed) {
        if (!progress) return;
        const qint64 permille = completed * 1000 / std::max<qint64>(1, totalWork);
        if (permille == lastReportedPermille && completed < totalWork) return;
        lastReportedPermille = permille;
        progress({phase, completed, totalWork});
    };

    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) { cleanup(); return false; }
        const QString name = QString::number(i + 1) + QStringLiteral(".bmp");
        QString writeError;
        const QPoint cell = order[static_cast<size_t>(i)];
        const ImageRegion region = segmentationCellRegion(
            segmentation, document->metadata.width, document->metadata.height, cell.y(), cell.x());
        if (!region.isValid() || region.x > INT_MAX || region.y > INT_MAX ||
            region.width > INT_MAX || region.height > INT_MAX) {
            cleanup();
            if (error) *error = QStringLiteral("分割区域超出 BMP 导出器支持的尺寸范围。");
            return false;
        }
        const bool ok = BmpStreamWriter::write(temp.filePath(name), document,
            static_cast<int>(region.x), static_cast<int>(region.y),
            static_cast<int>(region.width), static_cast<int>(region.height), segmentation.settings,
            [&](int row, int rowTotal) {
                if (token->isCancelled()) return false;
                const qint64 base = qint64(i) * 1000;
                reportProgress(QStringLiteral("导出 %1").arg(name),
                               base + qint64(row) * 1000 / std::max(1, rowTotal));
                return true;
            }, &writeError);
        if (!ok) { cleanup(); if(!token->isCancelled()&&error)*error=writeError; return false; }
    }

    if (token->isCancelled()) { cleanup(); return false; }
    QDir backup(temp.filePath("backup")); temp.mkdir("backup");
    QStringList committed, backedUp;
    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) { if(error)*error=QStringLiteral("提交阶段已取消。"); goto rollback; }
        const QString name = QString::number(i + 1) + QStringLiteral(".bmp");
        const QString target = output.filePath(name);
        const QFileInfo targetInfo(target);
        if (targetInfo.exists()) {
            if (!targetInfo.isFile() || targetInfo.isSymLink()) {
                if(error)*error=QStringLiteral("目标路径不是可覆盖的普通文件：%1").arg(name);
                goto rollback;
            }
            if (!overwrite) { if(error)*error=QStringLiteral("目标文件已存在：%1").arg(name); goto rollback; }
            if (!QFile::rename(target, backup.filePath(name))) { if(error)*error=QStringLiteral("无法备份：%1").arg(name); goto rollback; }
            backedUp << name;
        }
        if (!QFile::rename(temp.filePath(name), target)) { if(error)*error=QStringLiteral("无法提交：%1").arg(name); goto rollback; }
        committed << name;
        reportProgress(QStringLiteral("提交文件"), qint64(order.size()) * 1000 + i + 1);
    }
    cleanup(); return true;

rollback:
    {
        QStringList rollbackErrors;
        for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
            const QString target = output.filePath(*it);
            if (QFile::exists(target) && !QFile::remove(target))
                rollbackErrors << QStringLiteral("无法移除已提交文件 %1").arg(*it);
        }
        for (auto it = backedUp.crbegin(); it != backedUp.crend(); ++it) {
            const QString target = output.filePath(*it);
            const QString saved = backup.filePath(*it);
            if (QFile::exists(target)) {
                rollbackErrors << QStringLiteral("目标仍被占用，未恢复 %1").arg(*it);
            } else if (!QFile::rename(saved, target)) {
                rollbackErrors << QStringLiteral("无法恢复原文件 %1").arg(*it);
            }
        }
        if (rollbackErrors.isEmpty()) {
            if (!cleanup() && error)
                *error += QStringLiteral("\n回滚已完成，但临时目录未能删除：%1").arg(temp.absolutePath());
        } else if (error) {
            *error += QStringLiteral("\n回滚未完全成功：%1。备份已保留在：%2")
                          .arg(rollbackErrors.join(QStringLiteral("；")), temp.absolutePath());
        }
        return false;
    }
}
