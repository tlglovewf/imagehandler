#include "io/MergedImageExporter.h"
#include "imaging/VipsSupport.h"

#include <QDataStream>
#include <QSaveFile>
#include <limits>
#include <vips/vips.h>

namespace {
VipsImage* makeGrayUchar(VipsImage* input, QString* error) {
    VipsImage* gray = nullptr;
    if (input->Bands == 1) {
        gray = input;
        g_object_ref(gray);
    } else if (vips_colourspace(input, &gray, VIPS_INTERPRETATION_B_W, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return nullptr;
    }

    if (gray->Bands > 1) {
        VipsImage* firstBand = nullptr;
        if (vips_extract_band(gray, &firstBand, 0, nullptr) != 0) {
            g_object_unref(gray);
            if (error) *error = VipsSupport::lastError();
            return nullptr;
        }
        g_object_unref(gray);
        gray = firstBand;
    }
    if (gray->BandFmt != VIPS_FORMAT_UCHAR) {
        VipsImage* casted = nullptr;
        if (vips_cast(gray, &casted, VIPS_FORMAT_UCHAR, nullptr) != 0) {
            g_object_unref(gray);
            if (error) *error = VipsSupport::lastError();
            return nullptr;
        }
        g_object_unref(gray);
        gray = casted;
    }
    return gray;
}
}

bool MergedImageExporter::writeGray8Bmp(const QString& path, const ImageDocumentPtr& document,
                                        const CancellationTokenPtr& token,
                                        const Progress& progress, QString* error) {
    if (!document || !document->image || document->metadata.width <= 0 ||
        document->metadata.height <= 0 ||
        document->metadata.width > std::numeric_limits<qint32>::max() ||
        document->metadata.height > std::numeric_limits<qint32>::max()) {
        if (error) *error = QStringLiteral("合并图像或尺寸无效。");
        return false;
    }

    const quint64 width = static_cast<quint64>(document->metadata.width);
    const quint64 height = static_cast<quint64>(document->metadata.height);
    const quint64 stride = ((width * 8 + 31) / 32) * 4;
    constexpr quint32 paletteBytes = 256 * 4;
    constexpr quint32 pixelOffset = 14 + 40 + paletteBytes;
    const quint64 fileSize = quint64(pixelOffset) + stride * height;
    if (fileSize > std::numeric_limits<quint32>::max()) {
        if (error) *error = QStringLiteral("合并图导出结果超过经典 BMP 的 4GB 限制。");
        return false;
    }
    if (token && token->isCancelled()) return false;

    QString conversionError;
    VipsImage* gray = makeGrayUchar(document->image.get(), &conversionError);
    if (!gray) {
        if (error) *error = conversionError;
        return false;
    }
    VipsRegion* region = vips_region_new(gray);
    if (!region) {
        g_object_unref(gray);
        if (error) *error = VipsSupport::lastError();
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        g_object_unref(region);
        g_object_unref(gray);
        if (error) *error = file.errorString();
        return false;
    }

    QDataStream output(&file);
    output.setByteOrder(QDataStream::LittleEndian);
    output.writeRawData("BM", 2);
    output << quint32(fileSize) << quint16(0) << quint16(0) << pixelOffset;
    output << quint32(40) << qint32(width) << qint32(-qint64(height))
           << quint16(1) << quint16(8) << quint32(0)
           << quint32(stride * height) << qint32(2835) << qint32(2835)
           << quint32(256) << quint32(0);
    for (quint32 value = 0; value < 256; ++value)
        output << quint32(value | (value << 8) | (value << 16));
    if (output.status() != QDataStream::Ok) {
        file.cancelWriting();
        g_object_unref(region);
        g_object_unref(gray);
        if (error) *error = file.errorString();
        return false;
    }

    QByteArray row(static_cast<qsizetype>(stride), '\0');
    const int imageWidth = static_cast<int>(width);
    const int imageHeight = static_cast<int>(height);
    for (int y = 0; y < imageHeight; ++y) {
        if (token && token->isCancelled()) {
            file.cancelWriting();
            g_object_unref(region);
            g_object_unref(gray);
            return false;
        }
        VipsRect rectangle{0, y, imageWidth, 1};
        if (vips_region_prepare(region, &rectangle) != 0) {
            file.cancelWriting();
            g_object_unref(region);
            g_object_unref(gray);
            if (error) *error = VipsSupport::lastError();
            return false;
        }
        const auto* source = VIPS_REGION_ADDR(region, 0, y);
        memcpy(row.data(), source, static_cast<size_t>(imageWidth));
        if (stride > width)
            memset(row.data() + imageWidth, 0, static_cast<size_t>(stride - width));
        if (file.write(row) != row.size()) {
            file.cancelWriting();
            g_object_unref(region);
            g_object_unref(gray);
            if (error) *error = file.errorString();
            return false;
        }
        if (progress) progress({QStringLiteral("写入 8-bit BMP 扫描行"), y + 1, imageHeight});
    }

    g_object_unref(region);
    g_object_unref(gray);
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
