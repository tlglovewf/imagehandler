#include "imaging/ElementGrouping.h"
#include "imaging/ImageLoader.h"
#include "imaging/PatternDetector.h"
#include "imaging/VipsSupport.h"
#include "io/LayeredPackageExporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
int runBenchmark() {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;

    constexpr int imageSide = 2048;
    constexpr int cellSide = 64;
    QImage source(imageSide, imageSide, QImage::Format_Grayscale8);
    for (int y = 0; y < imageSide; ++y) {
        uchar* row = source.scanLine(y);
        const int localY = y % cellSide;
        const int cellRow = y / cellSide;
        for (int x = 0; x < imageSide; ++x) {
            const int localX = x % cellSide;
            const int pattern = (cellRow + x / cellSide) % 4;
            switch (pattern) {
            case 0: row[x] = static_cast<uchar>((localX + localY) * 2); break;
            case 1: row[x] = localX < localY ? 32 : 224; break;
            case 2: row[x] = static_cast<uchar>((localX * 3 + localY * 5) & 0xff); break;
            default: row[x] = (localX / 8 + localY / 8) % 2 ? 64 : 192; break;
            }
        }
    }
    const QString sourcePath = temporary.filePath(QStringLiteral("benchmark.bmp"));
    if (!source.save(sourcePath, "BMP")) return 3;

    QString error;
    const auto document = ImageLoader::load(sourcePath, 1, &error);
    if (!document) return 4;
    const auto segmentation = PatternDetector::makeManualGrid(
        document, {cellSide, cellSide, 128, ForegroundPolarity::Dark});
    const auto token = std::make_shared<CancellationToken>();
    ExactGrayElementComparison comparison;
    QElapsedTimer timer;
    timer.start();
    const auto grouping = ElementGrouper::group(
        document, segmentation, comparison, token, {}, &error);
    const qint64 groupingMilliseconds = timer.elapsed();
    if (!grouping.valid || grouping.groups.size() != 4) return 5;

    ElementGroupingResult singleGroup = grouping;
    singleGroup.groups.resize(1);
    singleGroup.groups[0].cells.clear();
    singleGroup.groupByCell.fill(0);
    const QString output = temporary.filePath(QStringLiteral("layered"));
    if (!QDir().mkpath(output)) return 6;
    LayeredPackageOptions options;
    options.outputWidth = cellSide;
    options.outputHeight = cellSide;
    LayeredPackageRequest request{output, document, segmentation, TraversalMode::SnakeZ,
                                  options, true, singleGroup};
    const auto preflight = LayeredPackageExporter::inspect(request, token, {});
    if (!preflight.valid) return 7;
    timer.restart();
    if (!LayeredPackageExporter::exportPackage(
            request, {false, true, preflight.targetFingerprint}, token, {}, &error))
        return 8;
    const qint64 exportMilliseconds = timer.elapsed();

    QTextStream outputStream(stdout);
    outputStream << "grouping_ms=" << groupingMilliseconds
                 << " cells=" << segmentation.rows * segmentation.columns
                 << " groups=" << grouping.groups.size() << '\n'
                 << "layered_export_ms=" << exportMilliseconds
                 << " canvas=" << options.canvasWidth << 'x' << options.canvasHeight
                 << " layers=" << LayeredPackageExporter::layerCount(options)
                 << " groups=1\n";
    outputStream.flush();
    return 0;
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QString error;
    if (!VipsSupport::initialize("ImageHandlerPerformance", &error)) return 1;
    const int result = runBenchmark();
    VipsSupport::shutdown();
    return result;
}
