#include "imaging/VipsSupport.h"

#include <QByteArray>
#include <algorithm>
#include <mutex>
#include <vips/vips.h>

void VipsImageDeleter::operator()(VipsImage* image) const noexcept {
    if (image) g_object_unref(image);
}

namespace VipsSupport {
bool initialize(const char* argv0, QString* error) {
    if (VIPS_INIT(argv0) != 0) {
        if (error) *error = lastError();
        return false;
    }
    vips_cache_set_max(200);
    vips_cache_set_max_mem(512ULL * 1024ULL * 1024ULL);
    return true;
}

void shutdown() { vips_shutdown(); }

QString lastError() {
    const char* text = vips_error_buffer();
    const QString result = QString::fromUtf8(text ? text : "libvips error");
    vips_error_clear();
    return result;
}

VipsImagePtr adopt(VipsImage* image) { return VipsImagePtr(image, VipsImageDeleter{}); }

VipsImagePtr ref(VipsImage* image) {
    if (image) g_object_ref(image);
    return adopt(image);
}

QImage renderRegion(const ImageDocumentPtr& document, qint64 x, qint64 y,
                    qint64 width, qint64 height, double scale, QString* error) {
    if (!document || !document->image || width <= 0 || height <= 0 || scale <= 0.0) return {};
    VipsImage* cropped = nullptr;
    VipsImage* resized = nullptr;
    VipsImage* prepared = nullptr;
    x = std::clamp<qint64>(x, 0, document->metadata.width - 1);
    y = std::clamp<qint64>(y, 0, document->metadata.height - 1);
    width = std::min(width, document->metadata.width - x);
    height = std::min(height, document->metadata.height - y);

    if (vips_crop(document->image.get(), &cropped, static_cast<int>(x), static_cast<int>(y),
                  static_cast<int>(width), static_cast<int>(height), nullptr) != 0) goto fail;
    if (scale != 1.0) {
        if (vips_resize(cropped, &resized, scale, "kernel", VIPS_KERNEL_LINEAR, nullptr) != 0) goto fail;
    } else {
        resized = cropped;
        g_object_ref(resized);
    }

    if (document->metadata.kind == PixelKind::Color) {
        VipsImage* srgb = nullptr;
        if (vips_colourspace(resized, &srgb, VIPS_INTERPRETATION_sRGB, nullptr) != 0) goto fail;
        if (srgb->Bands > 3) {
            if (vips_extract_band(srgb, &prepared, 0, "n", 3, nullptr) != 0) {
                g_object_unref(srgb);
                goto fail;
            }
            g_object_unref(srgb);
        } else {
            prepared = srgb;
        }
    } else {
        if (resized->Bands == 1) {
            prepared = resized;
            g_object_ref(prepared);
        } else if (vips_extract_band(resized, &prepared, 0, nullptr) != 0) goto fail;
    }
    if (prepared->BandFmt != VIPS_FORMAT_UCHAR) {
        VipsImage* casted = nullptr;
        if (vips_cast(prepared, &casted, VIPS_FORMAT_UCHAR, nullptr) != 0) goto fail;
        g_object_unref(prepared);
        prepared = casted;
    }

    {
        size_t bytes = 0;
        void* memory = vips_image_write_to_memory(prepared, &bytes);
        if (!memory) goto fail;
        const int w = prepared->Xsize;
        const int h = prepared->Ysize;
        const int bands = prepared->Bands;
        QImage image(reinterpret_cast<const uchar*>(memory), w, h, w * bands,
                     bands == 1 ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
        QImage copy = image.copy();
        g_free(memory);
        g_object_unref(prepared);
        g_object_unref(resized);
        g_object_unref(cropped);
        return copy;
    }

fail:
    if (prepared) g_object_unref(prepared);
    if (resized) g_object_unref(resized);
    if (cropped) g_object_unref(cropped);
    if (error) *error = lastError();
    return {};
}
}
