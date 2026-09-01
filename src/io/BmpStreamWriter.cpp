#include "io/BmpStreamWriter.h"
#include "imaging/VipsSupport.h"

#include <QDataStream>
#include <QFile>
#include <limits>
#include <vips/vips.h>

bool BmpStreamWriter::write(const QString& path, const ImageDocumentPtr& document,
                            int x, int y, int width, int height,
                            const SegmentationSettings& settings,
                            const RowProgress& progress, QString* error) {
    if (!document || !document->image || width <= 0 || height <= 0) return false;
    const int bits = document->metadata.kind == PixelKind::Binary1 ? 1 : 8;
    const quint64 stride = ((quint64(width) * bits + 31) / 32) * 4;
    const quint32 paletteBytes = bits == 1 ? 8 : 1024;
    const quint32 pixelOffset = 14 + 40 + paletteBytes;
    const quint64 fileSize64 = quint64(pixelOffset) + stride * quint64(height);
    if (fileSize64 > std::numeric_limits<quint32>::max()) {
        if (error) *error = QStringLiteral("单个 BMP 超过经典 BMP 的 4GB 限制。");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file.errorString();
        return false;
    }
    QDataStream out(&file); out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("BM", 2);
    out << quint32(fileSize64) << quint16(0) << quint16(0) << pixelOffset;
    out << quint32(40) << qint32(width) << qint32(-height) << quint16(1) << quint16(bits)
        << quint32(0) << quint32(stride * height) << qint32(2835) << qint32(2835)
        << quint32(bits == 1 ? 2 : 256) << quint32(0);
    if (bits == 1) {
        out << quint32(0x00000000) << quint32(0x00ffffff);
    } else {
        for (quint32 i = 0; i < 256; ++i) out << quint32(i | (i << 8) | (i << 16));
    }

    VipsImage* gray = nullptr;
    if (document->image->Bands == 1) { gray = document->image.get(); g_object_ref(gray); }
    else if (vips_extract_band(document->image.get(), &gray, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError(); return false;
    }
    auto ucharImage = VipsSupport::toUchar(
        gray, document->metadata.kind == PixelKind::Gray16, error);
    g_object_unref(gray);
    if (!ucharImage) return false;
    VipsRegion* region = vips_region_new(ucharImage.get());
    QByteArray row(static_cast<qsizetype>(stride), '\0');
    for (int iy = 0; iy < height; ++iy) {
        if (progress && !progress(iy, height)) { g_object_unref(region); file.close(); return false; }
        VipsRect rect{x, y + iy, width, 1};
        if (vips_region_prepare(region, &rect) != 0) {
            g_object_unref(region);
            if (error) *error = VipsSupport::lastError(); return false;
        }
        const uchar* src = VIPS_REGION_ADDR(region, x, y + iy);
        row.fill('\0');
        if (bits == 8) {
            memcpy(row.data(), src, static_cast<size_t>(width));
        } else {
            for (int ix = 0; ix < width; ++ix) {
                const bool white = src[ix] > settings.threshold;
                if (white) row[ix / 8] = char(uchar(row[ix / 8]) | uchar(0x80u >> (ix % 8)));
            }
        }
        if (file.write(row) != row.size()) {
            g_object_unref(region);
            if (error) *error = file.errorString(); return false;
        }
    }
    if (progress) progress(height, height);
    g_object_unref(region);
    if (!file.flush()) { if (error) *error = file.errorString(); return false; }
    return true;
}
