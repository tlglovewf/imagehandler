#include "io/SplitExporter.h"
#include "io/BmpStreamWriter.h"

#include <QDir>
#include <QFile>
#include <QUuid>
#include <algorithm>

std::vector<QPoint> SplitExporter::traversal(int rows, int columns, TraversalMode mode) {
    std::vector<QPoint> order;
    if (rows <= 0 || columns <= 0) return order;
    order.reserve(static_cast<size_t>(rows * columns));
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

bool SplitExporter::exportTiles(const QString& outputDirectory, const ImageDocumentPtr& document,
                                const SegmentationResult& segmentation, TraversalMode mode,
                                bool overwrite, const CancellationTokenPtr& token,
                                const Progress& progress, QString* error) {
    QDir output(outputDirectory);
    if (!output.exists() && !output.mkpath(".")) { if(error)*error=QStringLiteral("无法创建输出目录。"); return false; }
    const auto order = traversal(segmentation.rows, segmentation.columns, mode);
    if (order.empty()) { if(error)*error=QStringLiteral("没有可导出的完整单元。"); return false; }
    const QString tempName = QStringLiteral(".image_handler_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(tempName)) { if(error)*error=QStringLiteral("无法创建临时导出目录。"); return false; }
    QDir temp(output.filePath(tempName));
    const auto cleanup = [&] { temp.removeRecursively(); };

    for (qsizetype i = 0; i < static_cast<qsizetype>(order.size()); ++i) {
        if (token->isCancelled()) { cleanup(); return false; }
        const QString name = QString::number(i + 1) + QStringLiteral(".bmp");
        QString writeError;
        const QPoint cell = order[static_cast<size_t>(i)];
        const bool ok = BmpStreamWriter::write(temp.filePath(name), document,
            cell.x() * segmentation.settings.cellWidth, cell.y() * segmentation.settings.cellHeight,
            segmentation.settings.cellWidth, segmentation.settings.cellHeight, segmentation.settings,
            [&](int row, int rowTotal) {
                if (token->isCancelled()) return false;
                const qint64 base = qint64(i) * 1000;
                if (progress) progress({QStringLiteral("导出 %1").arg(name), base + qint64(row) * 1000 / std::max(1,rowTotal), qint64(order.size()) * 1000});
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
        if (QFile::exists(target)) {
            if (!overwrite) { if(error)*error=QStringLiteral("目标文件已存在：%1").arg(name); goto rollback; }
            if (!QFile::rename(target, backup.filePath(name))) { if(error)*error=QStringLiteral("无法备份：%1").arg(name); goto rollback; }
            backedUp << name;
        }
        if (!QFile::rename(temp.filePath(name), target)) { if(error)*error=QStringLiteral("无法提交：%1").arg(name); goto rollback; }
        committed << name;
        if (progress) progress({QStringLiteral("提交文件"), i + 1, static_cast<qint64>(order.size())});
    }
    cleanup(); return true;

rollback:
    for (const QString& name : committed) QFile::remove(output.filePath(name));
    for (const QString& name : backedUp) QFile::rename(backup.filePath(name), output.filePath(name));
    cleanup(); return false;
}
