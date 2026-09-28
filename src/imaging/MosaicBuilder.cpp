#include "imaging/MosaicBuilder.h"
#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <limits>
#include <vips/vips.h>

namespace {
bool isMergeImage(const QString& path) {
    static const QSet<QString> extensions{QStringLiteral("bmp"), QStringLiteral("png"),
                                          QStringLiteral("tif"), QStringLiteral("tiff")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

VipsImagePtr normalizeTile(VipsImage* input, bool colorOutput, int sourceBits, QString* error) {
    VipsImage* prepared = nullptr;
    if (colorOutput) {
        VipsImage* srgb = nullptr;
        if (vips_colourspace(input, &srgb, VIPS_INTERPRETATION_sRGB, nullptr) != 0) {
            if (error) *error = VipsSupport::lastError();
            return {};
        }
        if (srgb->Bands > 3) {
            if (vips_extract_band(srgb, &prepared, 0, "n", 3, nullptr) != 0) {
                g_object_unref(srgb);
                if (error) *error = VipsSupport::lastError();
                return {};
            }
            g_object_unref(srgb);
        } else {
            prepared = srgb;
        }
    } else if (input->Bands == 1) {
        prepared = input;
        g_object_ref(prepared);
    } else if (vips_extract_band(input, &prepared, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }

    const VipsBandFormat targetFormat = !colorOutput && sourceBits == 16
        ? VIPS_FORMAT_USHORT : VIPS_FORMAT_UCHAR;
    if (prepared->BandFmt != targetFormat) {
        VipsImage* casted = nullptr;
        if (vips_cast(prepared, &casted, targetFormat, nullptr) != 0) {
            g_object_unref(prepared);
            if (error) *error = VipsSupport::lastError();
            return {};
        }
        g_object_unref(prepared);
        prepared = casted;
    }
    return VipsSupport::adopt(prepared);
}
}

bool MosaicBuilder::parseName(const QString& path, MergeTile* tile) {
    if (!tile || !isMergeImage(path)) return false;
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

MosaicBuildResult MosaicBuilder::build(const QStringList& paths, quint64 generation,
                                       const MosaicBuildOptions& options,
                                       const CancellationTokenPtr& token, const Progress& progress,
                                       QString* error) {
    MosaicBuildResult buildResult;
    buildResult.sourceCount = static_cast<int>(paths.size());
    if (paths.isEmpty()) { if (error) *error = QStringLiteral("当前列表中没有图片。"); return buildResult; }
    const qint64 phaseSize = paths.size();
    const qint64 totalProgress = phaseSize * 5;
    std::vector<MergeTile> tiles;
    QStringList problems;
    tiles.reserve(paths.size());
    for (qsizetype i = 0; i < paths.size(); ++i) {
        if (token->isCancelled()) return buildResult;
        MergeTile tile;
        if (!parseName(paths[i], &tile)) problems << QStringLiteral("名称不符合规则：%1").arg(QFileInfo(paths[i]).fileName());
        else tiles.push_back(tile);
        if (progress) progress({QStringLiteral("验证文件名与坐标"), i + 1, totalProgress});
    }
    if (!problems.isEmpty()) { if (error) *error = problems.join('\n'); return buildResult; }

    int minRow = INT_MAX, maxRow = 0, minCol = INT_MAX, maxCol = 0;
    QMap<QPair<int,int>, qsizetype> positions;
    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        const auto& t = tiles[i];
        const auto key = qMakePair(t.row, t.column);
        if (positions.contains(key)) problems << QStringLiteral("坐标重复：%1,%2").arg(t.row).arg(t.column);
        else positions.insert(key, i);
        minRow = std::min(minRow, t.row); maxRow = std::max(maxRow, t.row);
        minCol = std::min(minCol, t.column); maxCol = std::max(maxCol, t.column);
    }
    const qint64 rowSpan = qint64(maxRow) - minRow + 1;
    const qint64 columnSpan = qint64(maxCol) - minCol + 1;
    const qint64 expected = rowSpan * columnSpan;
    if (expected > 1000000) {
        problems << QStringLiteral("行列范围过大且明显存在大量缺块。");
    } else if (expected != static_cast<qint64>(tiles.size())) {
        for (qint64 r = minRow; r <= qint64(maxRow); ++r) {
            for (qint64 c = minCol; c <= qint64(maxCol); ++c) {
                const auto coordinate = qMakePair(static_cast<int>(r), static_cast<int>(c));
                if (!positions.contains(coordinate)) {
                    ++buildResult.missingCount;
                    if (buildResult.missingCoordinateSamples.size() < 20)
                        buildResult.missingCoordinateSamples << QStringLiteral("%1,%2").arg(r).arg(c);
                }
            }
        }
    }
    if (!problems.isEmpty()) { if (error) *error = problems.join('\n'); return buildResult; }

    buildResult.rows = static_cast<int>(rowSpan);
    buildResult.columns = static_cast<int>(columnSpan);
    if (buildResult.missingCount > 0 && !options.allowMissingTiles) {
        buildResult.status = MosaicBuildStatus::MissingTiles;
        return buildResult;
    }

    std::vector<ImageDocumentPtr> docs(tiles.size());
    ImageMetadata reference;
    int referenceBits = 0;
    bool colorOutput = false;
    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        if (token->isCancelled()) return buildResult;
        QString loadError;
        docs[i] = ImageLoader::load(tiles[i].path, generation, &loadError);
        if (!docs[i]) problems << QStringLiteral("无法读取 %1：%2").arg(QFileInfo(tiles[i].path).fileName(), loadError);
        else if (docs[i]->metadata.sourceBitsPerPixel != 1 &&
                 docs[i]->metadata.sourceBitsPerPixel != 8 &&
                 docs[i]->metadata.sourceBitsPerPixel != 16)
            problems << QStringLiteral("只支持 1-bit、8-bit 或 16-bit 图像：%1")
                            .arg(QFileInfo(tiles[i].path).fileName());
        else if (referenceBits == 0) {
            reference = docs[i]->metadata;
            referenceBits = docs[i]->metadata.sourceBitsPerPixel;
            colorOutput = docs[i]->metadata.kind == PixelKind::Color;
        } else if (docs[i]->metadata.sourceBitsPerPixel != referenceBits) {
            problems << QStringLiteral("位深不一致：%1（%2-bit，要求 %3-bit）")
                            .arg(QFileInfo(tiles[i].path).fileName())
                            .arg(docs[i]->metadata.sourceBitsPerPixel)
                            .arg(referenceBits);
        } else {
            colorOutput = colorOutput || docs[i]->metadata.kind == PixelKind::Color;
        }
        if (progress) progress({QStringLiteral("读取并验证图块"), phaseSize + i + 1,
                                totalProgress});
    }
    if (!problems.isEmpty()) { if (error) *error = problems.join('\n'); return buildResult; }

    const int rows = buildResult.rows, cols = buildResult.columns;
    std::vector<qint64> columnWidths(static_cast<size_t>(cols));
    std::vector<qint64> rowHeights(static_cast<size_t>(rows));
    qint64 globalMaximumWidth = 0;
    qint64 globalMaximumHeight = 0;
    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        const int row = tiles[i].row - minRow;
        const int column = tiles[i].column - minCol;
        columnWidths[static_cast<size_t>(column)] =
            std::max(columnWidths[static_cast<size_t>(column)], docs[i]->metadata.width);
        rowHeights[static_cast<size_t>(row)] =
            std::max(rowHeights[static_cast<size_t>(row)], docs[i]->metadata.height);
        globalMaximumWidth = std::max(globalMaximumWidth, docs[i]->metadata.width);
        globalMaximumHeight = std::max(globalMaximumHeight, docs[i]->metadata.height);
    }
    for (auto& width : columnWidths) if (width == 0) width = globalMaximumWidth;
    for (auto& height : rowHeights) if (height == 0) height = globalMaximumHeight;

    std::vector<qint64> columnOffsets(static_cast<size_t>(cols));
    std::vector<qint64> rowOffsets(static_cast<size_t>(rows));
    qint64 mosaicWidth = 0, mosaicHeight = 0;
    for (int column = 0; column < cols; ++column) {
        columnOffsets[static_cast<size_t>(column)] = mosaicWidth;
        mosaicWidth += columnWidths[static_cast<size_t>(column)];
    }
    for (int row = 0; row < rows; ++row) {
        rowOffsets[static_cast<size_t>(row)] = mosaicHeight;
        mosaicHeight += rowHeights[static_cast<size_t>(row)];
    }
    if (mosaicWidth <= 0 || mosaicHeight <= 0 ||
        mosaicWidth > std::numeric_limits<int>::max() ||
        mosaicHeight > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("合并结果尺寸超出 libvips 支持范围：%1 × %2")
                                .arg(mosaicWidth).arg(mosaicHeight);
        return buildResult;
    }

    std::vector<VipsImagePtr> normalized;
    normalized.reserve(docs.size());
    for (qsizetype i = 0; i < static_cast<qsizetype>(docs.size()); ++i) {
        if (token->isCancelled()) return buildResult;
        QString normalizationError;
        auto image = normalizeTile(docs[i]->image.get(), colorOutput, referenceBits,
                                   &normalizationError);
        if (!image) {
            if (error) *error = QStringLiteral("无法统一图块格式 %1：%2")
                                    .arg(QFileInfo(tiles[i].path).fileName(), normalizationError);
            return buildResult;
        }
        normalized.push_back(std::move(image));
        if (progress) progress({QStringLiteral("统一图块格式"), phaseSize * 2 + i + 1,
                                totalProgress});
    }

    VipsImage* blackSource = nullptr;
    VipsImage* black = nullptr;
    VipsImage* white = nullptr;
    VipsImage* mosaic = nullptr;
    const int outputBands = colorOutput ? 3 : 1;
    const VipsInterpretation interpretation = colorOutput ? VIPS_INTERPRETATION_sRGB
                                                           : VIPS_INTERPRETATION_B_W;
    if (vips_black(&blackSource, static_cast<int>(mosaicWidth), static_cast<int>(mosaicHeight),
                   "bands", outputBands, nullptr) != 0 ||
        (referenceBits == 16
             ? vips_cast(blackSource, &black, VIPS_FORMAT_USHORT, nullptr)
             : vips_copy(blackSource, &black, nullptr)) != 0 ||
        vips_invert(black, &white, nullptr) != 0 ||
        vips_copy(white, &mosaic, "interpretation", interpretation, nullptr) != 0) {
        if (blackSource) g_object_unref(blackSource);
        if (black) g_object_unref(black);
        if (white) g_object_unref(white);
        if (error) *error = VipsSupport::lastError();
        return buildResult;
    }
    g_object_unref(blackSource);
    g_object_unref(black);
    g_object_unref(white);

    for (qsizetype i = 0; i < static_cast<qsizetype>(tiles.size()); ++i) {
        if (token->isCancelled()) {
            g_object_unref(mosaic);
            return buildResult;
        }
        const int row = tiles[i].row - minRow;
        const int column = tiles[i].column - minCol;
        VipsImage* next = nullptr;
        if (vips_insert(mosaic, normalized[static_cast<size_t>(i)].get(), &next,
                        static_cast<int>(columnOffsets[static_cast<size_t>(column)]),
                        static_cast<int>(rowOffsets[static_cast<size_t>(row)]),
                        "expand", FALSE, nullptr) != 0) {
            g_object_unref(mosaic);
            if (error) *error = VipsSupport::lastError();
            return buildResult;
        }
        g_object_unref(mosaic);
        mosaic = next;
        if (progress) progress({QStringLiteral("构建虚拟拼图"), phaseSize * 3 + i + 1,
                                totalProgress});
    }

    auto result = std::make_shared<ImageDocument>();
    result->displayName = QStringLiteral("合并结果 (%1×%2)").arg(rows).arg(cols);
    result->metadata = reference;
    result->metadata.width = mosaicWidth;
    result->metadata.height = mosaicHeight;
    result->metadata.bands = outputBands;
    result->metadata.kind = colorOutput ? PixelKind::Color
        : referenceBits == 1 ? PixelKind::Binary1
        : referenceBits == 16 ? PixelKind::Gray16 : PixelKind::Gray8;
    result->metadata.sourceBitsPerPixel = referenceBits;
    result->image = VipsSupport::adopt(mosaic);
    result->generation = generation;
    result->virtualMosaic = true;
    buildResult.status = MosaicBuildStatus::Success;
    buildResult.document = result;
    return buildResult;
}
