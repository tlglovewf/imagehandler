#include "imaging/BmpRegionLoader.h"

#include <QFile>
#include <array>
#include <limits>
#include <memory>
#include <vips/vips.h>

namespace {
quint16 readLe16(const QByteArray& bytes, int offset) {
    return quint16(uchar(bytes[offset])) |
           (quint16(uchar(bytes[offset + 1])) << 8);
}

quint32 readLe32(const QByteArray& bytes, int offset) {
    return quint32(uchar(bytes[offset])) |
           (quint32(uchar(bytes[offset + 1])) << 8) |
           (quint32(uchar(bytes[offset + 2])) << 16) |
           (quint32(uchar(bytes[offset + 3])) << 24);
}

qint32 readLeS32(const QByteArray& bytes, int offset) {
    return static_cast<qint32>(readLe32(bytes, offset));
}

struct BmpSource {
    QString path;
    int width = 0;
    int height = 0;
    int bits = 0;
    qint64 pixelOffset = 0;
    qint64 rowStride = 0;
    bool topDown = false;
    std::array<uchar, 256> palette{};
};

struct BmpSequence {
    QFile file;
    explicit BmpSequence(const QString& path) : file(path) {}
};

void* bmpStart(VipsImage*, void* opaque, void*) {
    const auto* source = static_cast<const BmpSource*>(opaque);
    auto sequence = std::make_unique<BmpSequence>(source->path);
    if (!sequence->file.open(QIODevice::ReadOnly)) {
        const QByteArray message = sequence->file.errorString().toUtf8();
        vips_error("bmp-region-loader", "%s", message.constData());
        return nullptr;
    }
    return sequence.release();
}

int bmpGenerate(VipsRegion* output, void* sequenceOpaque, void* sourceOpaque,
                void*, gboolean*) {
    auto* sequence = static_cast<BmpSequence*>(sequenceOpaque);
    const auto* source = static_cast<const BmpSource*>(sourceOpaque);
    const VipsRect rect = output->valid;
    const int firstByte = source->bits == 1 ? rect.left / 8 : rect.left;
    const int lastByte = source->bits == 1
                             ? (rect.left + rect.width - 1) / 8
                             : rect.left + rect.width - 1;
    const qint64 byteCount = qint64(lastByte) - firstByte + 1;
    QByteArray packed(static_cast<qsizetype>(byteCount), Qt::Uninitialized);

    for (int y = rect.top; y < rect.top + rect.height; ++y) {
        const int storedRow = source->topDown ? y : source->height - 1 - y;
        const qint64 position = source->pixelOffset + qint64(storedRow) * source->rowStride + firstByte;
        if (!sequence->file.seek(position) || sequence->file.read(packed.data(), byteCount) != byteCount) {
            const QByteArray message = sequence->file.errorString().toUtf8();
            vips_error("bmp-region-loader", "cannot read scanline %d: %s", y, message.constData());
            return -1;
        }

        auto* destination = VIPS_REGION_ADDR(output, rect.left, y);
        if (source->bits == 8) {
            for (int x = 0; x < rect.width; ++x)
                destination[x] = source->palette[uchar(packed[x])];
        } else {
            for (int x = 0; x < rect.width; ++x) {
                const int sourceX = rect.left + x;
                const int byteIndex = sourceX / 8 - firstByte;
                const int paletteIndex = (uchar(packed[byteIndex]) >> (7 - sourceX % 8)) & 1;
                destination[x] = source->palette[static_cast<size_t>(paletteIndex)];
            }
        }
    }
    return 0;
}

int bmpStop(void* sequence, void*, void*) {
    delete static_cast<BmpSequence*>(sequence);
    return 0;
}

void destroySource(gpointer opaque) {
    delete static_cast<BmpSource*>(opaque);
}
}

BmpRegionLoader::Result BmpRegionLoader::load(const QString& path) {
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.status = Status::Error;
        result.error = QStringLiteral("无法打开 BMP：%1").arg(file.errorString());
        return result;
    }

    const QByteArray header = file.read(54);
    if (header.size() < 2 || header[0] != 'B' || header[1] != 'M')
        return result;
    if (header.size() < 54) {
        result.status = Status::Error;
        result.error = QStringLiteral("BMP 文件头不完整。");
        return result;
    }

    const quint32 dibSize = readLe32(header, 14);
    const qint32 signedWidth = readLeS32(header, 18);
    const qint32 signedHeight = readLeS32(header, 22);
    const quint16 planes = readLe16(header, 26);
    const quint16 bits = readLe16(header, 28);
    const quint32 compression = readLe32(header, 30);
    const quint32 pixelOffset = readLe32(header, 10);
    result.sourceBitsPerPixel = bits;

    if (dibSize < 40 || signedWidth <= 0 || signedHeight == 0 ||
        signedHeight == std::numeric_limits<qint32>::min() || planes != 1) {
        result.status = Status::Error;
        result.error = QStringLiteral("不支持或损坏的 BMP 文件头。");
        return result;
    }
    if (bits != 1 && bits != 8) {
        result.status = Status::UseGenericLoader;
        return result;
    }

    const int paletteCapacity = 1 << bits;
    quint32 colorCount = readLe32(header, 46);
    if (colorCount == 0) colorCount = static_cast<quint32>(paletteCapacity);
    if (colorCount > static_cast<quint32>(paletteCapacity)) {
        result.status = Status::Error;
        result.error = QStringLiteral("BMP 调色板大小无效。");
        return result;
    }

    const quint64 paletteOffset = 14ull + dibSize;
    const quint64 paletteBytes = quint64(colorCount) * 4;
    if (paletteOffset + paletteBytes > pixelOffset || paletteOffset > std::numeric_limits<qint64>::max() ||
        !file.seek(static_cast<qint64>(paletteOffset))) {
        result.status = Status::Error;
        result.error = QStringLiteral("BMP 调色板或像素偏移无效。");
        return result;
    }
    const QByteArray palette = file.read(static_cast<qint64>(paletteBytes));
    if (palette.size() != static_cast<qsizetype>(paletteBytes)) {
        result.status = Status::Error;
        result.error = QStringLiteral("BMP 调色板不完整。");
        return result;
    }

    auto source = std::make_unique<BmpSource>();
    for (int i = 0; i < 256; ++i) source->palette[static_cast<size_t>(i)] = static_cast<uchar>(i);
    bool grayscale = true;
    for (quint32 i = 0; i < colorCount; ++i) {
        const uchar blue = uchar(palette[static_cast<qsizetype>(i * 4)]);
        const uchar green = uchar(palette[static_cast<qsizetype>(i * 4 + 1)]);
        const uchar red = uchar(palette[static_cast<qsizetype>(i * 4 + 2)]);
        grayscale = grayscale && red == green && green == blue;
        source->palette[static_cast<size_t>(i)] = red;
    }
    result.grayscalePalette = grayscale;
    if (!grayscale || compression != 0) {
        result.status = Status::UseGenericLoader;
        return result;
    }

    source->path = path;
    source->width = signedWidth;
    source->height = std::abs(signedHeight);
    source->bits = bits;
    source->pixelOffset = pixelOffset;
    source->topDown = signedHeight < 0;
    source->rowStride = ((qint64(source->width) * bits + 31) / 32) * 4;
    const quint64 requiredSize = quint64(pixelOffset) + quint64(source->rowStride) * quint64(source->height);
    if (requiredSize > static_cast<quint64>(file.size())) {
        result.status = Status::Error;
        result.error = QStringLiteral("BMP 像素数据不完整。");
        return result;
    }

    VipsImage* image = vips_image_new();
    if (!image) {
        result.status = Status::Error;
        result.error = QStringLiteral("无法创建 libvips 图像。");
        return result;
    }
    vips_image_init_fields(image, source->width, source->height, 1,
                           VIPS_FORMAT_UCHAR, VIPS_CODING_NONE,
                           VIPS_INTERPRETATION_B_W, 1.0, 1.0);
    vips_image_set_int(image, VIPS_META_BITS_PER_SAMPLE, bits);
    if (vips_image_pipelinev(image, VIPS_DEMAND_STYLE_SMALLTILE,
                             static_cast<VipsImage*>(nullptr))) {
        g_object_unref(image);
        result.status = Status::Error;
        result.error = QStringLiteral("无法初始化 BMP 区域读取管线。");
        return result;
    }
    BmpSource* sourcePointer = source.release();
    g_object_set_data_full(G_OBJECT(image), "image-handler-bmp-source", sourcePointer, destroySource);
    if (vips_image_generate(image, bmpStart, bmpGenerate, bmpStop, sourcePointer, nullptr)) {
        g_object_unref(image);
        result.status = Status::Error;
        result.error = QStringLiteral("无法创建 BMP 区域读取管线。");
        return result;
    }

    result.status = Status::Loaded;
    result.image = image;
    return result;
}
