#include "imaging/PatternDetector.h"
#include "imaging/VipsSupport.h"

#include <QQueue>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
int otsu(const QImage& image) {
    std::array<qint64, 256> hist{};
    for (int y = 0; y < image.height(); ++y) {
        const uchar* p = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) ++hist[p[x]];
    }
    const qint64 total = qint64(image.width()) * image.height();
    double sum = 0;
    for (int i = 0; i < 256; ++i) sum += i * hist[i];
    qint64 wb = 0; double sb = 0, best = -1; int threshold = 128;
    for (int i = 0; i < 256; ++i) {
        wb += hist[i]; if (!wb) continue;
        const qint64 wf = total - wb; if (!wf) break;
        sb += i * hist[i];
        const double mb = sb / wb, mf = (sum - sb) / wf;
        const double between = double(wb) * double(wf) * (mb - mf) * (mb - mf);
        if (between > best) { best = between; threshold = i; }
    }
    return threshold;
}

int bestPeriod(const std::vector<int>& projection) {
    const int n = static_cast<int>(projection.size());
    if (n < 8) return 0;
    double best = -1; int bestLag = 0;
    const int minLag = std::max(2, n / 128);
    for (int lag = minLag; lag <= n / 2; ++lag) {
        double dot = 0, a2 = 0, b2 = 0;
        for (int i = 0; i + lag < n; ++i) {
            dot += double(projection[i]) * projection[i + lag];
            a2 += double(projection[i]) * projection[i];
            b2 += double(projection[i + lag]) * projection[i + lag];
        }
        const double score = (a2 > 0 && b2 > 0) ? dot / std::sqrt(a2 * b2) : 0;
        if (score > best + 0.002) { best = score; bestLag = lag; }
    }
    return bestLag;
}
}

SegmentationResult PatternDetector::detect(const ImageDocumentPtr& document,
                                           const CancellationTokenPtr& token,
                                           const Progress& progress, QString* error) {
    SegmentationResult result;
    if (!document || (document->metadata.kind != PixelKind::Binary1 && document->metadata.kind != PixelKind::Gray8)) {
        if (error) *error = QStringLiteral("只有 1-bit 二值图或 8-bit 灰度图可以分割。");
        return result;
    }
    const double scale = std::min(1.0, 2048.0 / std::max(document->metadata.width, document->metadata.height));
    if (progress) progress({QStringLiteral("生成分析缩略图"), 0, 0});
    QString renderError;
    QImage sample = VipsSupport::renderRegion(document, 0, 0, document->metadata.width,
                                              document->metadata.height, scale, &renderError);
    if (sample.isNull()) { if (error) *error = renderError; return result; }
    if (token->isCancelled()) return result;
    if (progress) progress({QStringLiteral("计算直方图"), 1, 4});
    const int threshold = otsu(sample);
    qint64 borderSum = 0, borderCount = 0;
    for (int x = 0; x < sample.width(); ++x) { borderSum += sample.constScanLine(0)[x]; borderSum += sample.constScanLine(sample.height()-1)[x]; borderCount += 2; }
    for (int y = 1; y + 1 < sample.height(); ++y) { borderSum += sample.constScanLine(y)[0]; borderSum += sample.constScanLine(y)[sample.width()-1]; borderCount += 2; }
    const bool darkForeground = borderCount == 0 || borderSum / borderCount > threshold;

    const int w = sample.width(), h = sample.height();
    std::vector<uchar> foreground(static_cast<size_t>(w * h));
    std::vector<int> px(w), py(h);
    for (int y = 0; y < h; ++y) {
        const uchar* row = sample.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const bool on = darkForeground ? row[x] <= threshold : row[x] > threshold;
            foreground[static_cast<size_t>(y*w+x)] = on;
            if (on) { ++px[x]; ++py[y]; }
        }
    }
    if (progress) progress({QStringLiteral("分析重复周期"), 2, 4});
    int periodX = bestPeriod(px), periodY = bestPeriod(py);

    std::vector<uchar> seen(foreground.size());
    int maxBoxW = 0, maxBoxH = 0, components = 0;
    QQueue<QPoint> queue;
    for (int sy = 0; sy < h; ++sy) for (int sx = 0; sx < w; ++sx) {
        const int start = sy*w+sx;
        if (!foreground[start] || seen[start]) continue;
        seen[start] = 1; queue.enqueue({sx,sy});
        int minX=sx,maxX=sx,minY=sy,maxY=sy,pixels=0;
        while (!queue.isEmpty()) {
            const QPoint p = queue.dequeue(); ++pixels;
            minX=std::min(minX,p.x()); maxX=std::max(maxX,p.x()); minY=std::min(minY,p.y()); maxY=std::max(maxY,p.y());
            for (int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                const int nx=p.x()+dx, ny=p.y()+dy;
                if(nx<0||ny<0||nx>=w||ny>=h) continue;
                const int ni=ny*w+nx;
                if(foreground[ni]&&!seen[ni]) { seen[ni]=1; queue.enqueue({nx,ny}); }
            }
        }
        if (pixels >= 3) { ++components; maxBoxW=std::max(maxBoxW,maxX-minX+1); maxBoxH=std::max(maxBoxH,maxY-minY+1); }
    }
    if (progress) progress({QStringLiteral("细化图案尺寸"), 3, 4});
    const int sampleCellW = std::max(periodX, maxBoxW);
    const int sampleCellH = std::max(periodY, maxBoxH);
    result.settings.cellWidth = std::max(1, static_cast<int>(std::llround(sampleCellW / scale)));
    result.settings.cellHeight = std::max(1, static_cast<int>(std::llround(sampleCellH / scale)));
    result.settings.threshold = threshold;
    result.settings.polarity = darkForeground ? ForegroundPolarity::Dark : ForegroundPolarity::Light;
    result = makeGrid(document, result.settings);
    result.confidence = (components > 1 && periodX > 0 && periodY > 0) ? 0.8 : 0.45;
    if (result.confidence < 0.6) result.warning = QStringLiteral("自动识别置信度较低，已采用最大包围盒尺寸，请复核宽高。");
    if (progress) progress({QStringLiteral("生成分割网格"), 4, 4});
    return result;
}

SegmentationResult PatternDetector::makeGrid(const ImageDocumentPtr& document,
                                             const SegmentationSettings& settings) {
    SegmentationResult result; result.settings = settings;
    if (!document || settings.cellWidth <= 0 || settings.cellHeight <= 0) return result;
    result.columns = static_cast<int>(document->metadata.width / settings.cellWidth);
    result.rows = static_cast<int>(document->metadata.height / settings.cellHeight);
    result.remainderWidth = static_cast<int>(document->metadata.width % settings.cellWidth);
    result.remainderHeight = static_cast<int>(document->metadata.height % settings.cellHeight);
    return result;
}
