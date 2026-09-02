#include "io/MaskSliceExporter.h"

#include "imaging/ImageLoader.h"
#include "imaging/PointCloudMath.h"
#include "imaging/VipsSupport.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <vips/vips.h>

namespace {
VipsImagePtr prepareFloatRegion(const MaskSliceRequest& request, QString* error) {
    if (!request.source || !request.source->image || !request.region.isValid() ||
        request.region.x < 0 || request.region.y < 0 ||
        request.region.x + request.region.width > request.source->metadata.width ||
        request.region.y + request.region.height > request.source->metadata.height ||
        request.region.x > INT_MAX || request.region.y > INT_MAX ||
        request.region.width > INT_MAX || request.region.height > INT_MAX) {
        if (error) *error = QStringLiteral("切片区域无效或超出图像范围。");
        return {};
    }
    VipsImage* croppedRaw = nullptr;
    if (vips_crop(request.source->image.get(), &croppedRaw,
                  static_cast<int>(request.region.x), static_cast<int>(request.region.y),
                  static_cast<int>(request.region.width), static_cast<int>(request.region.height),
                  nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto cropped = VipsSupport::adopt(croppedRaw);
    VipsImage* bandRaw = nullptr;
    if (cropped->Bands == 1) { bandRaw = cropped.get(); g_object_ref(bandRaw); }
    else if (vips_extract_band(cropped.get(), &bandRaw, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    const auto band = VipsSupport::adopt(bandRaw);
    VipsImage* floatRaw = nullptr;
    if (vips_cast(band.get(), &floatRaw, VIPS_FORMAT_FLOAT, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    return VipsSupport::adopt(floatRaw);
}

double storedMaximum(PixelKind kind) {
    return kind == PixelKind::Gray16 ? 65535.0 : 255.0;
}

bool writeHeader(QFile* file, int width, int height, quint64 fileSize, QString* error) {
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file->errorString();
        return false;
    }
    constexpr quint32 pixelOffset = 14 + 40 + 8;
    const quint32 stride = static_cast<quint32>(((quint64(width) + 31) / 32) * 4);
    QDataStream out(file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("BM", 2);
    out << quint32(fileSize) << quint16(0) << quint16(0) << quint32(pixelOffset);
    out << quint32(40) << qint32(width) << qint32(-height) << quint16(1) << quint16(1)
        << quint32(0) << quint32(quint64(stride) * height) << qint32(2835) << qint32(2835)
        << quint32(2) << quint32(0);
    out << quint32(0x00000000) << quint32(0x00ffffff);
    if (out.status() != QDataStream::Ok) {
        if (error) *error = file->errorString();
        return false;
    }
    return true;
}

bool safeConflict(const QFileInfo& info) {
    return info.exists() && info.isFile() && !info.isSymLink();
}
}

quint64 MaskSliceExporter::bmpFileSize(int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const quint64 stride = ((quint64(width) + 31) / 32) * 4;
    if (stride > (std::numeric_limits<quint64>::max() - 62) / quint64(height)) return 0;
    return 62 + stride * quint64(height);
}

bool MaskSliceExporter::exportAll(
    const QString& outputDirectory, const MaskSliceRequest& request, int layerCount,
    bool overwrite, const CancellationTokenPtr& token, const Progress& progress,
    QString* error) {
    if (!request.source || !ImageLoader::isProcessable(request.source->metadata) ||
        layerCount <= 0 || layerCount > kMaximumMaskSliceLayers ||
        request.settings.outputWidth <= 0 || request.settings.outputHeight <= 0 ||
        request.settings.layerHeight <= 0.0) {
        if (error) *error = QStringLiteral("掩模导出参数无效。");
        return false;
    }
    QDir output(outputDirectory);
    if (!output.exists()) {
        if (error) *error = QStringLiteral("输出目录不存在。");
        return false;
    }
    const quint64 singleSize = bmpFileSize(request.settings.outputWidth,
                                          request.settings.outputHeight);
    if (singleSize == 0 || singleSize > std::numeric_limits<quint32>::max()) {
        if (error) *error = QStringLiteral("单个掩模超过经典 BMP 的 4 GB 限制。");
        return false;
    }
    if (singleSize > std::numeric_limits<quint64>::max() / quint64(layerCount)) {
        if (error) *error = QStringLiteral("掩模总输出大小溢出。");
        return false;
    }
    const quint64 stagedBytes = singleSize * quint64(layerCount);
    quint64 backupBytes = 0;
    QStringList conflicts;
    for (int layer = 1; layer <= layerCount; ++layer) {
        const QString name = QString::number(layer) + QStringLiteral(".bmp");
        const QFileInfo info(output.filePath(name));
        if (!info.exists() && !info.isSymLink()) continue;
        if (!safeConflict(info)) {
            if (error) *error = QStringLiteral("同名目标不是可安全替换的普通文件：%1").arg(name);
            return false;
        }
        conflicts << name;
        backupBytes += static_cast<quint64>(std::max<qint64>(0, info.size()));
    }
    if (!conflicts.isEmpty() && !overwrite) {
        if (error) *error = QStringLiteral("MASK_TARGETS_EXIST:%1").arg(conflicts.size());
        return false;
    }
    const quint64 requiredBytes = stagedBytes > std::numeric_limits<quint64>::max() - backupBytes
                                      ? std::numeric_limits<quint64>::max()
                                      : stagedBytes + backupBytes;
    const QStorageInfo storage(output.absolutePath());
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 &&
        requiredBytes > static_cast<quint64>(storage.bytesAvailable())) {
        if (error) *error = QStringLiteral("输出磁盘空间不足，至少需要 %1 字节。").arg(requiredBytes);
        return false;
    }

    const QString temporaryName = QStringLiteral(".image_handler_masks_%1")
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!output.mkdir(temporaryName)) {
        if (error) *error = QStringLiteral("无法创建掩模导出临时目录。");
        return false;
    }
    QDir temporary(output.filePath(temporaryName));
    if (!temporary.mkdir(QStringLiteral("staged")) ||
        !temporary.mkdir(QStringLiteral("backup"))) {
        temporary.removeRecursively();
        if (error) *error = QStringLiteral("无法初始化掩模导出临时目录。");
        return false;
    }
    QDir staged(temporary.filePath(QStringLiteral("staged")));
    QDir backup(temporary.filePath(QStringLiteral("backup")));
    const auto cleanup = [&] { return temporary.removeRecursively(); };

    const auto floatImage = prepareFloatRegion(request, error);
    if (!floatImage) { cleanup(); return false; }
    VipsRegion* sourceRegion = vips_region_new(floatImage.get());
    if (!sourceRegion) {
        cleanup();
        if (error) *error = VipsSupport::lastError();
        return false;
    }
    QVector<int> sourceColumns(request.settings.outputWidth);
    for (int x = 0; x < request.settings.outputWidth; ++x) {
        sourceColumns[x] = std::min(floatImage->Xsize - 1,
            static_cast<int>((quint64(x) * floatImage->Xsize) /
                             quint64(request.settings.outputWidth)));
    }
    const quint64 stride = ((quint64(request.settings.outputWidth) + 31) / 32) * 4;
    QByteArray packedRow(static_cast<qsizetype>(stride), '\0');
    const double divisor = storedMaximum(request.source->metadata.kind);
    const qint64 totalRows = qint64(layerCount) * request.settings.outputHeight;
    qint64 completedRows = 0;
    bool writeSucceeded = true;
    for (int layer = 0; layer < layerCount && writeSucceeded; ++layer) {
        if (token && token->isCancelled()) { writeSucceeded = false; break; }
        QFile file(staged.filePath(QString::number(layer + 1) + QStringLiteral(".bmp")));
        if (!writeHeader(&file, request.settings.outputWidth, request.settings.outputHeight,
                         singleSize, error)) { writeSucceeded = false; break; }
        for (int outputY = 0; outputY < request.settings.outputHeight; ++outputY) {
            if (token && token->isCancelled()) { writeSucceeded = false; break; }
            const int sourceY = std::min(floatImage->Ysize - 1,
                static_cast<int>((quint64(outputY) * floatImage->Ysize) /
                                 quint64(request.settings.outputHeight)));
            VipsRect rectangle{0, sourceY, floatImage->Xsize, 1};
            if (vips_region_prepare(sourceRegion, &rectangle) != 0) {
                if (error) *error = VipsSupport::lastError();
                writeSucceeded = false;
                break;
            }
            const auto* values = reinterpret_cast<const float*>(
                VIPS_REGION_ADDR(sourceRegion, 0, sourceY));
            packedRow.fill('\0');
            for (int outputX = 0; outputX < request.settings.outputWidth; ++outputX) {
                const bool active = PointCloudMath::maskLayerActive(
                    PointCloudMath::heightForRaw(values[sourceColumns[outputX]], divisor,
                                                 request.mapping),
                    layer, request.settings.layerHeight);
                const bool white = request.settings.invertMask ? !active : active;
                if (white) {
                    packedRow[outputX / 8] = char(uchar(packedRow[outputX / 8]) |
                                                        uchar(0x80u >> (outputX % 8)));
                }
            }
            if (file.write(packedRow) != packedRow.size()) {
                if (error) *error = file.errorString();
                writeSucceeded = false;
                break;
            }
            ++completedRows;
            if (progress) progress({QStringLiteral("导出第 %1/%2 层").arg(layer + 1).arg(layerCount),
                                    completedRows, totalRows});
        }
        if (!file.flush() && writeSucceeded) {
            if (error) *error = file.errorString();
            writeSucceeded = false;
        }
    }
    g_object_unref(sourceRegion);
    if (!writeSucceeded) {
        cleanup();
        return false;
    }

    QStringList backedUp;
    QStringList committed;
    for (const QString& name : conflicts) {
        if (token && token->isCancelled()) {
            if (error) *error = QStringLiteral("提交阶段已取消。");
            goto rollback;
        }
        if (!QDir().rename(output.filePath(name), backup.filePath(name))) {
            if (error) *error = QStringLiteral("无法备份旧掩模：%1").arg(name);
            goto rollback;
        }
        backedUp << name;
    }
    for (int layer = 1; layer <= layerCount; ++layer) {
        const QString name = QString::number(layer) + QStringLiteral(".bmp");
        if (token && token->isCancelled()) {
            if (error) *error = QStringLiteral("提交阶段已取消。");
            goto rollback;
        }
        if (!QDir().rename(staged.filePath(name), output.filePath(name))) {
            if (error) *error = QStringLiteral("无法提交掩模：%1").arg(name);
            goto rollback;
        }
        committed << name;
    }
    if (progress) progress({QStringLiteral("提交掩模"), totalRows, totalRows});
    cleanup();
    return true;

rollback:
    {
        QStringList rollbackErrors;
        for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
            if (!QFile::remove(output.filePath(*it)))
                rollbackErrors << QStringLiteral("无法移除新掩模 %1").arg(*it);
        }
        for (auto it = backedUp.crbegin(); it != backedUp.crend(); ++it) {
            const QString target = output.filePath(*it);
            if (QFileInfo::exists(target))
                rollbackErrors << QStringLiteral("目标仍被占用，未恢复 %1").arg(*it);
            else if (!QDir().rename(backup.filePath(*it), target))
                rollbackErrors << QStringLiteral("无法恢复旧掩模 %1").arg(*it);
        }
        if (rollbackErrors.isEmpty()) {
            cleanup();
        } else if (error) {
            *error += QStringLiteral("\n回滚未完全成功：%1。备份保留在：%2")
                          .arg(rollbackErrors.join(QStringLiteral("；")),
                               temporary.absolutePath());
        }
    }
    return false;
}
