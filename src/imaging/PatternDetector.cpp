#include "imaging/PatternDetector.h"
#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"

#include <QQueue>
#include <QMap>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace {
bool cancelled(const CancellationTokenPtr& token) {
    return token && token->isCancelled();
}

int otsu(const QImage& image, const CancellationTokenPtr& token) {
    std::array<qint64, 256> hist{};
    for (int y = 0; y < image.height(); ++y) {
        if ((y & 31) == 0 && cancelled(token)) return -1;
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

int bestPeriod(const std::vector<int>& projection, const CancellationTokenPtr& token,
               double* confidence = nullptr) {
    const int n = static_cast<int>(projection.size());
    if (n < 8) return 0;
    const double mean = std::accumulate(projection.begin(), projection.end(), 0.0) / n;
    const int minLag = std::max(2, n / 128);
    const int maxLag = n / 2;
    std::vector<double> scores(static_cast<size_t>(maxLag + 2), -1.0);
    for (int lag = minLag; lag <= n / 2; ++lag) {
        if ((lag & 31) == 0 && cancelled(token)) return 0;
        double dot = 0, a2 = 0, b2 = 0;
        for (int i = 0; i + lag < n; ++i) {
            const double a = projection[i] - mean;
            const double b = projection[i + lag] - mean;
            dot += a * b;
            a2 += a * a;
            b2 += b * b;
        }
        scores[static_cast<size_t>(lag)] = (a2 > 0 && b2 > 0) ? dot / std::sqrt(a2 * b2) : -1.0;
    }

    // A repetitive signal has strong peaks at P, 2P, 3P ... . Selecting the
    // absolute maximum often chooses a harmonic because it is affected less by
    // resampling and truncated borders. Find genuine local peaks, then select
    // the smallest peak whose score is close to the strongest one.
    std::vector<int> peaks;
    double strongest = -1.0;
    for (int lag = minLag + 1; lag < maxLag; ++lag) {
        const double score = scores[static_cast<size_t>(lag)];
        if (score >= scores[static_cast<size_t>(lag - 1)] &&
            score > scores[static_cast<size_t>(lag + 1)] && score > 0.15) {
            peaks.push_back(lag);
            strongest = std::max(strongest, score);
        }
    }
    if (peaks.empty()) { if (confidence) *confidence = 0.0; return 0; }
    const double accepted = std::max(0.25, strongest * 0.85);
    for (const int lag : peaks) {
        if (scores[static_cast<size_t>(lag)] >= accepted) {
            if (confidence) *confidence = scores[static_cast<size_t>(lag)];
            return lag;
        }
    }
    if (confidence) *confidence = 0.0;
    return 0;
}

int bestPhase(const std::vector<int>& projection, int period) {
    if (period <= 0 || projection.empty()) return 0;
    std::vector<double> sums(static_cast<size_t>(period));
    std::vector<int> counts(static_cast<size_t>(period));
    for (int i = 0; i < static_cast<int>(projection.size()); ++i) {
        sums[static_cast<size_t>(i % period)] += projection[static_cast<size_t>(i)];
        ++counts[static_cast<size_t>(i % period)];
    }
    int phase = 0;
    double best = -1.0;
    for (int i = 0; i < period; ++i) {
        const double score = counts[static_cast<size_t>(i)] > 0
                                 ? sums[static_cast<size_t>(i)] / counts[static_cast<size_t>(i)] : 0.0;
        if (score > best) { best = score; phase = i; }
    }
    return phase;
}

int nearestExactCellSize(qint64 imageSize, int estimatedSize) {
    if (imageSize <= 0 || estimatedSize <= 0) return 0;
    qint64 best = 0;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    for (qint64 divisor = 1; divisor * divisor <= imageSize; ++divisor) {
        if (imageSize % divisor != 0) continue;
        const qint64 paired = imageSize / divisor;
        for (const qint64 candidate : {divisor, paired}) {
            if (candidate > INT_MAX) continue;
            const qint64 distance = std::llabs(candidate - qint64(estimatedSize));
            if (distance < bestDistance || (distance == bestDistance && candidate > best)) {
                best = candidate;
                bestDistance = distance;
            }
        }
    }
    const qint64 maximumAdjustment = std::max<qint64>(2, qint64(std::ceil(estimatedSize * 0.12)));
    if (best == 0 || bestDistance > maximumAdjustment) return 0;
    return static_cast<int>(best);
}
}

SegmentationResult PatternDetector::detect(const ImageDocumentPtr& document,
                                           const CancellationTokenPtr& token,
                                           const Progress& progress, QString* error) {
    SegmentationResult result;
    if (!document || !ImageLoader::isProcessable(document->metadata)) {
        if (error) *error = QStringLiteral("只有 1-bit 二值图、8-bit 或 16-bit 灰度图可以分割。");
        return result;
    }
    const double scale = std::min(1.0, 2048.0 / std::max(document->metadata.width, document->metadata.height));
    if (progress) progress({QStringLiteral("生成分析缩略图"), 0, 0});
    QString renderError;
    QImage sample = VipsSupport::renderRegion(document, 0, 0, document->metadata.width,
                                              document->metadata.height, scale, &renderError, token);
    if (sample.isNull()) {
        if (!cancelled(token) && error) *error = renderError;
        return result;
    }
    if (sample.format() == QImage::Format_Grayscale16)
        sample = sample.convertToFormat(QImage::Format_Grayscale8);
    if (cancelled(token)) return result;
    if (progress) progress({QStringLiteral("计算直方图"), 1, 4});
    int minimumGray = 255, maximumGray = 0;
    for (int y = 0; y < sample.height(); ++y) {
        if ((y & 31) == 0 && cancelled(token)) return result;
        const uchar* row = sample.constScanLine(y);
        for (int x = 0; x < sample.width(); ++x) {
            minimumGray = std::min(minimumGray, int(row[x]));
            maximumGray = std::max(maximumGray, int(row[x]));
        }
    }
    if (maximumGray - minimumGray < 4) {
        if (error) *error = QStringLiteral("图像灰度变化不足，未识别到可重复的图案。");
        return result;
    }
    const int threshold = otsu(sample, token);
    if (threshold < 0) return result;
    qint64 borderSum = 0, borderCount = 0;
    for (int x = 0; x < sample.width(); ++x) { borderSum += sample.constScanLine(0)[x]; borderSum += sample.constScanLine(sample.height()-1)[x]; borderCount += 2; }
    for (int y = 1; y + 1 < sample.height(); ++y) { borderSum += sample.constScanLine(y)[0]; borderSum += sample.constScanLine(y)[sample.width()-1]; borderCount += 2; }
    const bool darkForeground = borderCount == 0 || borderSum / borderCount > threshold;

    const int w = sample.width(), h = sample.height();
    std::vector<uchar> foreground(static_cast<size_t>(w * h));
    std::vector<int> px(w), py(h);
    qint64 foregroundPixels = 0;
    for (int y = 0; y < h; ++y) {
        if ((y & 31) == 0 && cancelled(token)) return result;
        const uchar* row = sample.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const bool on = darkForeground ? row[x] <= threshold : row[x] > threshold;
            foreground[static_cast<size_t>(y*w+x)] = on;
            if (on) { ++px[x]; ++py[y]; ++foregroundPixels; }
        }
    }
    const qint64 samplePixels = qint64(w) * h;
    if (foregroundPixels < std::max<qint64>(16, samplePixels / 1000) ||
        foregroundPixels > samplePixels - std::max<qint64>(16, samplePixels / 1000)) {
        if (error) *error = QStringLiteral("前景像素过少或过多，无法可靠识别分割周期。");
        return result;
    }
    if (progress) progress({QStringLiteral("分析重复周期"), 2, 4});
    double periodConfidenceX = 0.0, periodConfidenceY = 0.0;
    int periodX = bestPeriod(px, token, &periodConfidenceX);
    int periodY = bestPeriod(py, token, &periodConfidenceY);
    if (cancelled(token)) return result;
    if (periodX <= 0 || periodY <= 0 || periodConfidenceX < 0.25 || periodConfidenceY < 0.25) {
        if (error) *error = QStringLiteral("横向或纵向重复周期不可靠，无法生成固定 Rect2D 网格。");
        return result;
    }
    const int centerPhaseX = bestPhase(px, periodX);
    const int centerPhaseY = bestPhase(py, periodY);

    std::vector<uchar> seen(foreground.size());
    int components = 0;
    struct GroupBox { int minX=INT_MAX,minY=INT_MAX,maxX=INT_MIN,maxY=INT_MIN; bool touchesEdge=false; };
    QMap<QPair<int,int>,GroupBox> groups;
    QQueue<QPoint> queue;
    qint64 visitedPixels = 0;
    for (int sy = 0; sy < h; ++sy) for (int sx = 0; sx < w; ++sx) {
        if (sx == 0 && (sy & 15) == 0 && cancelled(token)) return result;
        const int start = sy*w+sx;
        if (!foreground[start] || seen[start]) continue;
        seen[start] = 1; queue.enqueue({sx,sy});
        int minX=sx,maxX=sx,minY=sy,maxY=sy,pixels=0;
        while (!queue.isEmpty()) {
            if (((++visitedPixels) & 0xffff) == 0 && cancelled(token)) return result;
            const QPoint p = queue.dequeue(); ++pixels;
            minX=std::min(minX,p.x()); maxX=std::max(maxX,p.x()); minY=std::min(minY,p.y()); maxY=std::max(maxY,p.y());
            for (int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                const int nx=p.x()+dx, ny=p.y()+dy;
                if(nx<0||ny<0||nx>=w||ny>=h) continue;
                const int ni=ny*w+nx;
                if(foreground[ni]&&!seen[ni]) { seen[ni]=1; queue.enqueue({nx,ny}); }
            }
        }
        if (pixels >= 3) {
            ++components;
            if (periodX > 0 && periodY > 0) {
                const int groupX=static_cast<int>(std::llround((0.5*(minX+maxX)-centerPhaseX)/periodX));
                const int groupY=static_cast<int>(std::llround((0.5*(minY+maxY)-centerPhaseY)/periodY));
                auto& box=groups[qMakePair(groupX,groupY)];
                box.minX=std::min(box.minX,minX);box.maxX=std::max(box.maxX,maxX);
                box.minY=std::min(box.minY,minY);box.maxY=std::max(box.maxY,maxY);
                box.touchesEdge=box.touchesEdge||minX==0||minY==0||maxX==w-1||maxY==h-1;
            }
        }
    }
    if (progress) progress({QStringLiteral("细化图案尺寸"), 3, 4});

    // Group disconnected components by lattice cell and measure the actual
    // foreground box relative to the periodic center. Border-touching groups
    // are excluded because they may be incomplete patterns.
    int relativeMinX=INT_MAX,relativeMaxX=INT_MIN,relativeMinY=INT_MAX,relativeMaxY=INT_MIN;
    int validGroups=0;
    for(auto it=groups.cbegin();it!=groups.cend();++it) {
        const auto& box=it.value(); if(box.touchesEdge)continue;
        const int centerX=centerPhaseX+it.key().first*periodX;
        const int centerY=centerPhaseY+it.key().second*periodY;
        relativeMinX=std::min(relativeMinX,box.minX-centerX);relativeMaxX=std::max(relativeMaxX,box.maxX-centerX);
        relativeMinY=std::min(relativeMinY,box.minY-centerY);relativeMaxY=std::max(relativeMaxY,box.maxY-centerY);
        ++validGroups;
    }
    const bool grouped=validGroups>0&&relativeMinX<=relativeMaxX&&relativeMinY<=relativeMaxY;
    if (components < 4 || !grouped) {
        if (error) *error = QStringLiteral("检测到的完整图案数量不足，无法可靠生成分割网格。");
        return result;
    }
    // Fixed Rect2D splitting starts at (0,0), therefore its width and height
    // are the lattice periods. Component boxes validate that the periodic
    // candidates correspond to multiple complete patterns.
    const int sampleCellW=periodX;
    const int sampleCellH=periodY;
    const int estimatedWidth=std::max(1,static_cast<int>(std::llround(sampleCellW/scale)));
    const int estimatedHeight=std::max(1,static_cast<int>(std::llround(sampleCellH/scale)));
    result.settings.cellWidth=nearestExactCellSize(document->metadata.width,estimatedWidth);
    result.settings.cellHeight=nearestExactCellSize(document->metadata.height,estimatedHeight);
    if (result.settings.cellWidth <= 0 || result.settings.cellHeight <= 0) {
        if (error) *error = QStringLiteral("识别尺寸与图像尺寸不匹配，附近没有可整除图像的 Rect2D 尺寸。");
        return {};
    }
    result.settings.threshold = threshold;
    result.settings.polarity = darkForeground ? ForegroundPolarity::Dark : ForegroundPolarity::Light;
    result = makeGrid(document, result.settings);
    if (result.rows <= 0 || result.columns <= 0) {
        if (error) *error = result.warning.isEmpty() ? QStringLiteral("无法生成有效分割网格。") : result.warning;
        return {};
    }
    result.confidence = (validGroups > 1 && periodX > 0 && periodY > 0)
                            ? std::clamp(std::min(periodConfidenceX, periodConfidenceY), 0.0, 1.0)
                            : 0.45;
    if (result.confidence < 0.6) result.warning = QStringLiteral("自动识别置信度较低，请复核分割宽度和高度。");
    if (progress) progress({QStringLiteral("生成分割网格"), 4, 4});
    return result;
}

SegmentationResult PatternDetector::makeGrid(const ImageDocumentPtr& document,
                                             const SegmentationSettings& settings) {
    SegmentationResult result; result.settings = settings;
    if (!document || settings.cellWidth <= 0 || settings.cellHeight <= 0) return result;
    result.settings.cellWidth=nearestExactCellSize(document->metadata.width,settings.cellWidth);
    result.settings.cellHeight=nearestExactCellSize(document->metadata.height,settings.cellHeight);
    if (result.settings.cellWidth <= 0 || result.settings.cellHeight <= 0) {
        result.warning = QStringLiteral("输入尺寸与图像尺寸不匹配，附近没有可整除图像的尺寸。");
        return result;
    }
    result.columns=static_cast<int>(document->metadata.width/result.settings.cellWidth);
    result.rows=static_cast<int>(document->metadata.height/result.settings.cellHeight);
    const qint64 cellCount = qint64(result.rows) * result.columns;
    if (cellCount <= 0 || cellCount > kMaximumSegmentationCells) {
        result.rows = 0;
        result.columns = 0;
        result.warning = QStringLiteral("分割数量超过安全上限（%1 个），请增大分割尺寸。")
                             .arg(kMaximumSegmentationCells);
        return result;
    }
    result.remainderWidth=0;
    result.remainderHeight=0;
    return result;
}

SegmentationResult PatternDetector::makeManualGrid(const ImageDocumentPtr& document,
                                                   const SegmentationSettings& settings) {
    SegmentationResult result;
    result.settings = settings;
    if (!document || document->metadata.width <= 0 || document->metadata.height <= 0 ||
        settings.cellWidth <= 0 || settings.cellHeight <= 0) {
        result.warning = QStringLiteral("分割文档或参数无效。");
        return result;
    }

    const qint64 columns = (document->metadata.width + settings.cellWidth - 1) / settings.cellWidth;
    const qint64 rows = (document->metadata.height + settings.cellHeight - 1) / settings.cellHeight;
    if (columns > INT_MAX || rows > INT_MAX) {
        result.warning = QStringLiteral("分割行列数超出程序可表示范围。");
        return result;
    }

    result.columns = static_cast<int>(columns);
    result.rows = static_cast<int>(rows);
    result.remainderWidth = static_cast<int>(document->metadata.width % settings.cellWidth);
    result.remainderHeight = static_cast<int>(document->metadata.height % settings.cellHeight);
    const qint64 cellCount = rows * columns;
    if (cellCount > kMaximumSegmentationCells) {
        result.warning = QStringLiteral("预计生成 %1 个分割文件，导出前将再次确认。")
                             .arg(cellCount);
    }
    return result;
}
