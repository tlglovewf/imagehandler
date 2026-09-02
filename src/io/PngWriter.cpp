#include "io/PngWriter.h"
#include "imaging/VipsSupport.h"

#include <QFile>
#include <vips/vips.h>

namespace {
struct EvaluationContext {
    CancellationTokenPtr token;
    PngWriter::Progress progress;
};

void reportEvaluation(VipsImage* image, VipsProgress* value, gpointer opaque) {
    auto* context = static_cast<EvaluationContext*>(opaque);
    const bool cancelled = context->token && context->token->isCancelled();
    const bool continueWriting = !context->progress ||
        context->progress(value ? value->percent : 0, 100);
    if (cancelled || !continueWriting) vips_image_set_kill(image, TRUE);
}

VipsImagePtr preparePngImage(const ImageDocumentPtr& document, VipsImage* cropped,
                             int* bitDepth, QString* error) {
    VipsImage* singleBand = nullptr;
    if (document->metadata.kind == PixelKind::Color) {
        *bitDepth = 8;
        return VipsSupport::ref(cropped);
    }
    if (cropped->Bands == 1) {
        singleBand = cropped;
        g_object_ref(singleBand);
    } else if (vips_extract_band(cropped, &singleBand, 0, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return {};
    }

    const VipsBandFormat expectedFormat = document->metadata.kind == PixelKind::Gray16
        ? VIPS_FORMAT_USHORT : VIPS_FORMAT_UCHAR;
    if (singleBand->BandFmt != expectedFormat) {
        if (error) {
            *error = document->metadata.kind == PixelKind::Gray16
                ? QStringLiteral("16-bit 灰度图的内部像素格式不是 unsigned short，已拒绝降位导出。")
                : QStringLiteral("图像内部像素格式与声明位深不一致。");
        }
        g_object_unref(singleBand);
        return {};
    }

    *bitDepth = document->metadata.kind == PixelKind::Binary1 ? 1
              : document->metadata.kind == PixelKind::Gray16 ? 16 : 8;
    VipsImage* interpreted = nullptr;
    const VipsInterpretation interpretation = *bitDepth == 16
        ? VIPS_INTERPRETATION_GREY16 : VIPS_INTERPRETATION_B_W;
    if (vips_copy(singleBand, &interpreted, "interpretation", interpretation, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        g_object_unref(singleBand);
        return {};
    }
    g_object_unref(singleBand);
    return VipsSupport::adopt(interpreted);
}
}

bool PngWriter::write(const QString& path, const ImageDocumentPtr& document,
                      int x, int y, int width, int height,
                      const CancellationTokenPtr& token,
                      const Progress& progress, QString* error) {
    if (!document || !document->image || width <= 0 || height <= 0 ||
        x < 0 || y < 0 || qint64(x) + width > document->metadata.width ||
        qint64(y) + height > document->metadata.height) {
        if (error) *error = QStringLiteral("PNG 导出区域无效。");
        return false;
    }
    if (token && token->isCancelled()) return false;

    VipsImage* croppedRaw = nullptr;
    if (vips_crop(document->image.get(), &croppedRaw, x, y, width, height, nullptr) != 0) {
        if (error) *error = VipsSupport::lastError();
        return false;
    }
    const auto cropped = VipsSupport::adopt(croppedRaw);
    int bitDepth = 0;
    const auto prepared = preparePngImage(document, cropped.get(), &bitDepth, error);
    if (!prepared) return false;

    EvaluationContext context{token, progress};
    vips_image_set_progress(prepared.get(), TRUE);
    const gulong handler = g_signal_connect(
        prepared.get(), "eval", G_CALLBACK(reportEvaluation), &context);
    const QByteArray encodedPath = QFile::encodeName(path);
    const int result = vips_pngsave(prepared.get(), encodedPath.constData(),
                                    "bitdepth", bitDepth, nullptr);
    g_signal_handler_disconnect(prepared.get(), handler);
    if (result != 0) {
        if (token && token->isCancelled()) {
            VipsSupport::lastError();
        } else if (error) {
            *error = VipsSupport::lastError();
        }
        return false;
    }
    if (token && token->isCancelled()) return false;
    return !progress || progress(100, 100);
}
