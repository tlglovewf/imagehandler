#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <QFile>
#include <QFileInfo>
#include <vips/vips.h>

namespace {
quint16 le16(const QByteArray& b, int p) {
    return quint16(uchar(b[p])) | (quint16(uchar(b[p + 1])) << 8);
}
}

int ImageLoader::bmpBitsPerPixel(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QByteArray h = file.read(30);
    if (h.size() < 30 || h[0] != 'B' || h[1] != 'M') return 0;
    return le16(h, 28);
}

ImageDocumentPtr ImageLoader::load(const QString& path, quint64 generation, QString* error) {
    const QByteArray utf8 = QFile::encodeName(path);
    VipsImage* raw = vips_image_new_from_file(utf8.constData(), "access", VIPS_ACCESS_RANDOM,
                                              "fail_on", VIPS_FAIL_ON_ERROR, nullptr);
    if (!raw) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }
    auto doc = std::make_shared<ImageDocument>();
    doc->displayName = QFileInfo(path).fileName();
    doc->sourcePath = path;
    doc->image = VipsSupport::adopt(raw);
    doc->generation = generation;
    doc->metadata.width = raw->Xsize;
    doc->metadata.height = raw->Ysize;
    doc->metadata.bands = raw->Bands;

    const int bmpBits = QFileInfo(path).suffix().compare("bmp", Qt::CaseInsensitive) == 0
                            ? bmpBitsPerPixel(path) : 0;
    int metadataBits = 0;
    if (vips_image_get_typeof(raw, VIPS_META_BITS_PER_SAMPLE))
        vips_image_get_int(raw, VIPS_META_BITS_PER_SAMPLE, &metadataBits);
    if (bmpBits == 1 || (metadataBits == 1 && raw->Bands <= 2)) {
        doc->metadata.kind = PixelKind::Binary1;
        doc->metadata.sourceBitsPerPixel = 1;
    } else if (bmpBits == 8 || (raw->Bands <= 2 && raw->BandFmt == VIPS_FORMAT_UCHAR)) {
        doc->metadata.kind = PixelKind::Gray8;
        doc->metadata.sourceBitsPerPixel = 8;
    } else if (raw->Bands >= 3) {
        doc->metadata.kind = PixelKind::Color;
        doc->metadata.sourceBitsPerPixel = bmpBits > 0 ? bmpBits : raw->Bands * 8;
    } else {
        doc->metadata.kind = PixelKind::Unsupported;
        doc->metadata.sourceBitsPerPixel = 0;
    }
    return doc;
}

bool ImageLoader::isProcessable(const ImageMetadata& metadata) {
    return metadata.kind == PixelKind::Binary1 || metadata.kind == PixelKind::Gray8;
}
