#include "imaging/MosaicBuilder.h"
#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <vips/vips.h>

bool MosaicBuilder::parseName(const QString& path, MergeTile* tile) {
    if (!tile || QFileInfo(path).suffix().compare("bmp", Qt::CaseInsensitive) != 0) return false;
    static const QRegularExpression re(QStringLiteral(R"(^(?:(.*)_)?(\d+)_(\d+)$)"));
    const auto match = re.match(QFileInfo(path).completeBaseName());
    if (!match.hasMatch()) return false;
    bool rowOk = false, colOk = false;
    const qlonglong row = match.captured(2).toLongLong(&rowOk);
    const qlonglong col = match.captured(3).toLongLong(&colOk);
    if (!rowOk || !colOk || row > INT_MAX || col > INT_MAX) return false;
    tile->path = path;
    tile->prefix = match.captured(1);
    tile->row = static_cast<int>(row);
    tile->column = static_cast<int>(col);
    return true;
}

ImageDocumentPtr MosaicBuilder::build(const QStringList& paths, quint64 generation,
                                      const CancellationTokenPtr& token, const Progress& progress,
                                      QString* error) {
    if (paths.isEmpty()) { if (error) *error = QStringLiteral("当前列表中没有图片。"); return {}; }
    std::vector<MergeTile> tiles;
    QStringList problems;
    tiles.reserve(paths.size());
    for (qsizetype i = 0; i < paths.size(); ++i) {
        if (token->isCancelled()) return {};
        MergeTile tile;
        if (!parseName(paths[i], &tile)) problems << QStringLiteral("名称不符合规则：%1").arg(QFileInfo(paths[i]).fileName());
        else tiles.push_back(tile);
        if (progress) progress({QStringLiteral("验证文件名"), i + 1, paths.size()});
    }
    if (!problems.isEmpty()) { if (error) *error = problems.join('\n'); return {}; }

    const QString prefix = tiles.front().prefix;
    int minRow = INT_MAX, maxRow = 0, minCol = INT_MAX, maxCol = 0;
    QMap<QPair<int,int>, qsizetype> positions;
    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        const auto& t = tiles[i];
        if (t.prefix != prefix) problems << QStringLiteral("文件前缀不一致：%1").arg(QFileInfo(t.path).fileName());
        const auto key = qMakePair(t.row, t.column);
        if (positions.contains(key)) problems << QStringLiteral("坐标重复：%1,%2").arg(t.row).arg(t.column);
        else positions.insert(key, i);
        minRow = std::min(minRow, t.row); maxRow = std::max(maxRow, t.row);
        minCol = std::min(minCol, t.column); maxCol = std::max(maxCol, t.column);
    }
    const qint64 expected = qint64(maxRow - minRow + 1) * qint64(maxCol - minCol + 1);
    if (expected > 1000000) {
        problems << QStringLiteral("行列范围过大且明显存在大量缺块。");
    } else if (expected != static_cast<qint64>(tiles.size())) {
        for (int r = minRow; r <= maxRow; ++r)
            for (int c = minCol; c <= maxCol; ++c)
                if (!positions.contains({r,c})) problems << QStringLiteral("缺少坐标：%1,%2").arg(r).arg(c);
    }
    if (!problems.isEmpty()) { if (error) *error = problems.join('\n'); return {}; }

    std::vector<ImageDocumentPtr> docs(tiles.size());
    ImageMetadata reference;
    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        if (token->isCancelled()) return {};
        QString loadError;
        docs[i] = ImageLoader::load(tiles[i].path, generation, &loadError);
        if (!docs[i]) problems << QStringLiteral("无法读取 %1：%2").arg(QFileInfo(tiles[i].path).fileName(), loadError);
        else if (!ImageLoader::isProcessable(docs[i]->metadata)) problems << QStringLiteral("不支持合并的位深：%1").arg(QFileInfo(tiles[i].path).fileName());
        else if (i == 0) reference = docs[i]->metadata;
        else if (docs[i]->metadata.width != reference.width || docs[i]->metadata.height != reference.height ||
                 docs[i]->metadata.kind != reference.kind || docs[i]->metadata.bands != reference.bands ||
                 docs[i]->metadata.sourceBitsPerPixel != reference.sourceBitsPerPixel)
            problems << QStringLiteral("尺寸或位深不一致：%1").arg(QFileInfo(tiles[i].path).fileName());
        if (progress) progress({QStringLiteral("读取并验证图块"), i + 1, static_cast<qint64>(tiles.size())});
    }
    if (!problems.isEmpty()) { if (error) *error = problems.join('\n'); return {}; }

    const int rows = maxRow - minRow + 1, cols = maxCol - minCol + 1;
    std::vector<VipsImage*> ordered(static_cast<size_t>(rows * cols));
    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        const int index = (tiles[i].row - minRow) * cols + tiles[i].column - minCol;
        ordered[static_cast<size_t>(index)] = docs[i]->image.get();
    }
    VipsImage* mosaic = nullptr;
    if (vips_arrayjoin(ordered.data(), &mosaic, static_cast<int>(ordered.size()), "across", cols,
                       "shim", 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    if (progress) progress({QStringLiteral("构建虚拟拼图"), rows, rows});
    auto result = std::make_shared<ImageDocument>();
    result->displayName = QStringLiteral("合并结果 (%1×%2)").arg(rows).arg(cols);
    result->metadata = reference;
    result->metadata.width *= cols;
    result->metadata.height *= rows;
    result->image = VipsSupport::adopt(mosaic);
    result->generation = generation;
    result->virtualMosaic = true;
    return result;
}
