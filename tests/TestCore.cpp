#include "io/DirectoryScanner.h"
#include "domain/MergeInputSelector.h"
#include "domain/StorageSizeFormatter.h"
#include "io/BmpStreamWriter.h"
#include "io/MergedImageExporter.h"
#include "io/ExportPreflight.h"
#include "io/SplitExporter.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/PatternDetector.h"
#include "imaging/PointCloudBuilder.h"
#include "imaging/PointCloudMath.h"
#include "imaging/VipsSupport.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <set>

class TestCore final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void naturalSort();
    void mergeSelectionRules();
    void parseMergeName();
    void snakeTraversal();
    void spiralTraversal();
    void traversalLookupMatchesOrder();
    void bmpDepthPreserved();
    void nativeBmpRegionPixels();
    void gray16PngImportPreviewAndExport();
    void gray16PatternDetection();
    void pointCloudCoordinatesAndLodAverage();
    void pointCloudRootBudget();
    void pointCloudMappingOptions();
    void pointCloudImageFromEnvironment();
    void gray16ImageFromEnvironment();
    void gray16MosaicDirectoryFromEnvironment();
    void indexedColorBmpIsNotGrayscale();
    void mosaicPixels();
    void mosaicGray16PngPreservesSamples();
    void mosaicAllowsDifferentTileSizesAndPrefixes();
    void mosaicRejectsDifferentBitDepths();
    void mosaicMissingTilesRequireConfirmation();
    void mergedExporterWritesGray8();
    void mergedExporterConvertsColorAndIsAtomic();
    void mergedExporterRejectsClassicBmpOverflow();
    void fundamentalPatternPeriod();
    void blankImageIsRejected();
    void gridUsesFixedRect();
    void gridAdjustsToExactDivisor();
    void gridRejectsUnsafeAdjustment();
    void gridRejectsExcessiveCellCount();
    void manualGridCountExamples();
    void manualGridPreservesValuesAndCoversRemainder();
    void partialEdgeTilesAreExportedAtRemainingSize();
    void splitExportProgressIsBoundedAndMonotonic();
    void exportRollbackRestoresOriginal();
    void printCanvasCoordinates();
    void printPackageExportAndReplacement();
    void exportPreflightEstimatesAndSpacePolicy();
    void storageSizeFormatting();
    void exportPreflightConflictsAndPortableReports();
    void exportReportFailureLeavesTargetsUntouched();
    void realPatternImageFromEnvironment();
};

void TestCore::initTestCase() { QString error; QVERIFY2(VipsSupport::initialize("ImageHandlerTests",&error),qPrintable(error)); }
void TestCore::cleanupTestCase() { VipsSupport::shutdown(); }

void TestCore::pointCloudCoordinatesAndLodAverage() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(4,4,QImage::Format_Grayscale16);
    for(int y=0;y<4;++y){auto* row=reinterpret_cast<quint16*>(source.scanLine(y));for(int x=0;x<4;++x)row[x]=quint16((y*4+x)*4096);}
    const QString path=directory.filePath("cloud16.png");QVERIFY(source.save(path,"PNG"));
    QString error;const auto image=ImageLoader::load(path,901,&error);QVERIFY2(image,qPrintable(error));
    const PointCloudSettings settings{0.25,12.5};
    const auto cloud=PointCloudBuilder::build(image,settings,902,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(cloud,qPrintable(error));QCOMPARE(cloud->rootLevel,0);QCOMPARE(cloud->rootTiles.size(),size_t(1));
    const auto& exact=cloud->rootTiles.front()->vertices;QCOMPARE(exact.size(),size_t(16));
    QCOMPARE(exact[5].pixelX,1.0F);QCOMPARE(exact[5].pixelY,1.0F);
    QVERIFY(std::abs(exact[5].normalizedGray-float(5*4096)/65535.0F)<0.00001F);
    QCOMPARE(double(exact[5].pixelX)*settings.pixelScale,0.25);
    QVERIFY(std::abs(PointCloudMath::heightForNormalized(exact[5].normalizedGray,settings)-
                     (1.0-double(5*4096)/65535.0)*12.5)<0.00001);
    const auto lod=PointCloudBuilder::buildTile(cloud,1,0,0,std::make_shared<CancellationToken>(),&error);
    QVERIFY2(lod,qPrintable(error));QCOMPARE(lod->vertices.size(),size_t(4));
    QCOMPARE(lod->vertices[0].pixelX,0.5F);QCOMPARE(lod->vertices[0].pixelY,0.5F);
    const float expectedAverage=float((0+4096+4*4096+5*4096)/4.0)/65535.0F;
    QVERIFY(std::abs(lod->vertices[0].normalizedGray-expectedAverage)<0.00001F);
    QImage edgeSource(3,3,QImage::Format_Grayscale16);edgeSource.fill(0);reinterpret_cast<quint16*>(edgeSource.scanLine(2))[2]=65535;
    const QString edgePath=directory.filePath("cloud-edge16.png");QVERIFY(edgeSource.save(edgePath,"PNG"));
    const auto edgeImage=ImageLoader::load(edgePath,903,&error);QVERIFY2(edgeImage,qPrintable(error));
    const auto edgeCloud=PointCloudBuilder::build(edgeImage,settings,904,std::make_shared<CancellationToken>(),{},&error);QVERIFY2(edgeCloud,qPrintable(error));
    const auto edgeLod=PointCloudBuilder::buildTile(edgeCloud,1,0,0,std::make_shared<CancellationToken>(),&error);QVERIFY2(edgeLod,qPrintable(error));
    QCOMPARE(edgeLod->vertices.size(),size_t(4));QCOMPARE(edgeLod->vertices.back().pixelX,2.0F);QCOMPARE(edgeLod->vertices.back().pixelY,2.0F);QCOMPARE(edgeLod->vertices.back().normalizedGray,1.0F);
}

void TestCore::pointCloudRootBudget() {
    ImageMetadata metadata;metadata.width=28000;metadata.height=28000;metadata.kind=PixelKind::Gray16;
    const int level=PointCloudBuilder::rootLevelFor(metadata);QVERIFY(level>0);
    const qint64 step=qint64(1)<<level;
    const qint64 columns=(metadata.width+step-1)/step;
    const qint64 rows=(metadata.height+step-1)/step;
    QVERIFY(columns*rows<=kPointCloudRootPointBudget);
    if(level>0){const qint64 finer=qint64(1)<<(level-1);QVERIFY(((metadata.width+finer-1)/finer)*((metadata.height+finer-1)/finer)>kPointCloudRootPointBudget);}
}

void TestCore::pointCloudMappingOptions() {
    PointCloudSettings settings;
    QCOMPARE(settings.invertHeight,true);QCOMPARE(settings.startColor,QColor(0,0,255));QCOMPARE(settings.endColor,QColor(0,255,0));
    QCOMPARE(PointCloudMath::heightForNormalized(0.0,settings),12.5);QCOMPARE(PointCloudMath::heightForNormalized(1.0,settings),0.0);QCOMPARE(PointCloudMath::heightForNormalized(0.5,settings),6.25);
    const double middle16=PointCloudMath::heightForRaw(32768.0,65535.0,settings);QVERIFY(std::abs(middle16-(1.0-32768.0/65535.0)*12.5)<0.000001);
    settings.invertHeight=false;QCOMPARE(PointCloudMath::heightForNormalized(0.0,settings),0.0);QCOMPARE(PointCloudMath::heightForNormalized(1.0,settings),12.5);QCOMPARE(PointCloudMath::heightForRaw(128.0,255.0,settings),128.0/255.0*12.5);
    QCOMPARE(PointCloudMath::interpolateColor(0.0,QColor(0,0,255),QColor(0,255,0)),QColor(0,0,255));QCOMPARE(PointCloudMath::interpolateColor(1.0,QColor(0,0,255),QColor(0,255,0)),QColor(0,255,0));QCOMPARE(PointCloudMath::interpolateColor(0.5,QColor(0,0,255),QColor(0,255,0)),QColor(0,128,128));
    QCOMPARE(PointCloudMath::axisMaximum(1,0.25),0.0);QCOMPARE(PointCloudMath::axisMaximum(4,0.25),0.75);QCOMPARE(PointCloudMath::formatAxisMaximum(6999.750000),QStringLiteral("6999.75"));
    QCOMPARE(PointCloudMath::physicalDimension(220,0.25),55.0);QCOMPARE(PointCloudMath::physicalDimension(0,0.25),0.0);QCOMPARE(PointCloudMath::physicalDimension(3,0.125),0.375);
}

void TestCore::pointCloudImageFromEnvironment() {
    const QString path=qEnvironmentVariable("IMAGE_HANDLER_POINT_CLOUD_TEST_IMAGE");
    if(path.isEmpty())QSKIP("IMAGE_HANDLER_POINT_CLOUD_TEST_IMAGE is not set");
    QString error;const auto image=ImageLoader::load(path,905,&error);QVERIFY2(image,qPrintable(error));
    const auto cloud=PointCloudBuilder::build(image,{0.25,12.5},906,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(cloud,qPrintable(error));size_t points=0;for(const auto& tile:cloud->rootTiles)points+=tile->vertices.size();
    QVERIFY(points>0);QVERIFY(points<=static_cast<size_t>(kPointCloudRootPointBudget));QVERIFY(cloud->rootLevel>0);
}

void TestCore::naturalSort() { QVERIFY(DirectoryScanner::naturalLessThan("2.bmp","10.bmp")); }
void TestCore::mergeSelectionRules() { const QStringList directory={"a.bmp","b.PNG","c.BMP","d.jpg"};QCOMPARE(MergeInputSelector::resolve(directory,{}),QStringList({"a.bmp","b.PNG","c.BMP"}));QCOMPARE(MergeInputSelector::resolve(directory,{"a.bmp"}),QStringList({"a.bmp","b.PNG","c.BMP"}));QCOMPARE(MergeInputSelector::resolve(directory,{"a.bmp","b.PNG"}),QStringList({"a.bmp","b.PNG"})); }
void TestCore::parseMergeName() { MergeTile t; QVERIFY(MosaicBuilder::parseName("sample_part_12_3.bmp",&t)); QCOMPARE(t.prefix,QString("sample_part"));QCOMPARE(t.row,12);QCOMPARE(t.column,3);QVERIFY(MosaicBuilder::parseName("12_3.png",&t));QVERIFY(!MosaicBuilder::parseName("bad.bmp",&t)); }
void TestCore::snakeTraversal() { const auto p=SplitExporter::traversal(2,3,TraversalMode::SnakeZ); QCOMPARE(p.size(),size_t(6));QCOMPARE(p[0],QPoint(0,0));QCOMPARE(p[2],QPoint(2,0));QCOMPARE(p[3],QPoint(2,1));QCOMPARE(p[5],QPoint(0,1)); }
void TestCore::spiralTraversal() { const auto p=SplitExporter::traversal(3,3,TraversalMode::ClockwiseSpiral); QCOMPARE(p.size(),size_t(9));QCOMPARE(p.front(),QPoint(0,0));QCOMPARE(p[2],QPoint(2,0));QCOMPARE(p.back(),QPoint(1,1));std::set<std::pair<int,int>> unique;for(const auto&q:p)unique.emplace(q.x(),q.y());QCOMPARE(unique.size(),size_t(9)); }
void TestCore::traversalLookupMatchesOrder() {
    const QList<QSize> dimensions={{1,1},{1,5},{5,1},{2,3},{3,2},{4,4},{5,6}};
    const QList<TraversalMode> modes={TraversalMode::SnakeZ,TraversalMode::ClockwiseSpiral};
    for(const QSize size:dimensions)for(const auto mode:modes){
        const auto order=SplitExporter::traversal(size.height(),size.width(),mode);
        for(size_t i=0;i<order.size();++i){
            const QPoint cell=order[i];
            QCOMPARE(SplitExporter::traversalIndex(size.height(),size.width(),mode,cell.y(),cell.x()),qint64(i));
            const QPoint expected=i+1<order.size()?order[i+1]:QPoint(-1,-1);
            QCOMPARE(SplitExporter::nextTraversalCell(size.height(),size.width(),mode,cell.y(),cell.x()),expected);
        }
    }
    QCOMPARE(SplitExporter::traversalIndex(3,3,TraversalMode::SnakeZ,-1,0),qint64(-1));
    QCOMPARE(SplitExporter::nextTraversalCell(3,3,TraversalMode::SnakeZ,3,0),QPoint(-1,-1));
}
void TestCore::bmpDepthPreserved() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QImage source(16,8,QImage::Format_Mono);
    source.setColorTable({qRgb(0,0,0),qRgb(255,255,255)}); source.fill(0);
    for(int y=2;y<6;++y) for(int x=4;x<12;++x) source.setPixel(x,y,1);
    const QString input=dir.filePath("input.bmp"); QVERIFY(source.save(input,"BMP"));
    QString error; auto doc=ImageLoader::load(input,1,&error); QVERIFY2(doc,qPrintable(error));
    QCOMPARE(doc->metadata.kind,PixelKind::Binary1);
    const QString output=dir.filePath("output.bmp");
    SegmentationSettings settings{8,4,0,ForegroundPolarity::Dark};
    QVERIFY2(BmpStreamWriter::write(output,doc,0,0,8,4,settings,{},&error),qPrintable(error));
    QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray header=file.read(30);
    QCOMPARE(int(uchar(header[28])|(uchar(header[29])<<8)),1);
    file.close();
    const auto outputDocument=ImageLoader::load(output,2,&error);QVERIFY2(outputDocument,qPrintable(error));
    const QImage pixels=VipsSupport::renderRegion(outputDocument,0,0,8,4,1.0,&error);
    QVERIFY2(!pixels.isNull(),qPrintable(error));
    QCOMPARE(pixels.constScanLine(0)[0],uchar(0));
    QCOMPARE(pixels.constScanLine(3)[5],uchar(255));
}
void TestCore::nativeBmpRegionPixels() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QImage source(257,129,QImage::Format_Grayscale8); source.fill(240);
    source.scanLine(7)[13]=33; source.scanLine(120)[13]=201;
    const QString path=dir.filePath("direction.bmp"); QVERIFY(source.save(path,"BMP"));
    QString error;const auto doc=ImageLoader::load(path,2,&error);QVERIFY2(doc,qPrintable(error));
    QCOMPARE(doc->metadata.kind,PixelKind::Gray8);QCOMPARE(doc->metadata.bands,1);
    const QImage top=VipsSupport::renderRegion(doc,13,7,1,1,1.0,&error);
    const QImage bottom=VipsSupport::renderRegion(doc,13,120,1,1,1.0,&error);
    QVERIFY2(!top.isNull()&&!bottom.isNull(),qPrintable(error));
    QCOMPARE(top.constScanLine(0)[0],uchar(33));QCOMPARE(bottom.constScanLine(0)[0],uchar(201));
}
void TestCore::gray16PngImportPreviewAndExport() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QImage source(3,1,QImage::Format_Grayscale16);
    auto* pixels=reinterpret_cast<quint16*>(source.scanLine(0));
    pixels[0]=0;pixels[1]=32768;pixels[2]=65535;
    const QString input=dir.filePath("gray16.png");QVERIFY(source.save(input,"PNG"));
    QString error;const auto document=ImageLoader::load(input,4,&error);QVERIFY2(document,qPrintable(error));
    QCOMPARE(document->metadata.kind,PixelKind::Gray16);
    QCOMPARE(document->metadata.sourceBitsPerPixel,16);
    QCOMPARE(document->metadata.bands,1);
    QVERIFY(ImageLoader::isProcessable(document->metadata));

    const QImage preview=VipsSupport::renderRegion(document,0,0,3,1,1.0,&error);
    QVERIFY2(!preview.isNull(),qPrintable(error));
    QCOMPARE(preview.format(),QImage::Format_Grayscale16);
    const auto* previewPixels=reinterpret_cast<const quint16*>(preview.constScanLine(0));
    QCOMPARE(previewPixels[0],quint16(0));
    QCOMPARE(previewPixels[1],quint16(32768));
    QCOMPARE(previewPixels[2],quint16(65535));
    const QImage scaledPreview=VipsSupport::renderRegion(document,0,0,3,1,0.5,&error);
    QVERIFY2(!scaledPreview.isNull(),qPrintable(error));
    QCOMPARE(scaledPreview.format(),QImage::Format_Grayscale16);

    const QString output=dir.filePath("gray8.bmp");
    const SegmentationSettings settings{3,1,128,ForegroundPolarity::Dark};
    QVERIFY2(BmpStreamWriter::write(output,document,0,0,3,1,settings,{},&error),qPrintable(error));
    const auto exported=ImageLoader::load(output,5,&error);QVERIFY2(exported,qPrintable(error));
    QCOMPARE(exported->metadata.kind,PixelKind::Gray8);
    const QImage exportedPixels=VipsSupport::renderRegion(exported,0,0,3,1,1.0,&error);
    QVERIFY2(!exportedPixels.isNull(),qPrintable(error));
    QCOMPARE(exportedPixels.constScanLine(0)[0],uchar(0));
    QVERIFY(std::abs(int(exportedPixels.constScanLine(0)[1])-128)<=1);
    QCOMPARE(exportedPixels.constScanLine(0)[2],uchar(255));
}
void TestCore::gray16PatternDetection() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    constexpr int pitch=60;
    QImage pattern(480,360,QImage::Format_Grayscale16);
    for(int y=0;y<pattern.height();++y){
        auto* row=reinterpret_cast<quint16*>(pattern.scanLine(y));
        for(int x=0;x<pattern.width();++x){
            const int cx=(x/pitch)*pitch+pitch/2,cy=(y/pitch)*pitch+pitch/2;
            const double dx=x-cx,dy=y-cy;
            const double darkness=std::exp(-(dx*dx+dy*dy)/(2.0*11.0*11.0));
            row[x]=quint16(std::clamp(int(std::lround(65535.0*(1.0-darkness))),0,65535));
        }
    }
    const QString path=dir.filePath("pattern16.png");QVERIFY(pattern.save(path,"PNG"));
    QString error;const auto document=ImageLoader::load(path,6,&error);QVERIFY2(document,qPrintable(error));
    const auto result=PatternDetector::detect(document,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));
    QVERIFY2(std::abs(result.settings.cellWidth-pitch)<=1,qPrintable(QString("width=%1").arg(result.settings.cellWidth)));
    QVERIFY2(std::abs(result.settings.cellHeight-pitch)<=1,qPrintable(QString("height=%1").arg(result.settings.cellHeight)));
    QCOMPARE(result.columns,8);QCOMPARE(result.rows,6);
}
void TestCore::gray16ImageFromEnvironment() {
    const QString path=qEnvironmentVariable("IMAGE_HANDLER_16BIT_TEST_IMAGE");
    if(path.isEmpty())QSKIP("IMAGE_HANDLER_16BIT_TEST_IMAGE is not set");
    QString error;const auto document=ImageLoader::load(path,7,&error);QVERIFY2(document,qPrintable(error));
    QCOMPARE(document->metadata.kind,PixelKind::Gray16);
    QCOMPARE(document->metadata.sourceBitsPerPixel,16);
    QVERIFY(ImageLoader::isProcessable(document->metadata));
}
void TestCore::gray16MosaicDirectoryFromEnvironment() {
    const QString directory=qEnvironmentVariable("IMAGE_HANDLER_16BIT_MOSAIC_DIRECTORY");
    if(directory.isEmpty())QSKIP("IMAGE_HANDLER_16BIT_MOSAIC_DIRECTORY is not set");
    const QStringList paths=DirectoryScanner::scan(
        directory,std::make_shared<CancellationToken>(),{});
    QVERIFY(paths.size()>=2);
    QString error;const auto result=MosaicBuilder::build(
        paths,8,{},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(result.document,qPrintable(error));
    QCOMPARE(result.document->metadata.kind,PixelKind::Gray16);
    QCOMPARE(result.document->metadata.sourceBitsPerPixel,16);
}
void TestCore::indexedColorBmpIsNotGrayscale() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QImage source(16,16,QImage::Format_Indexed8);
    source.setColorTable({qRgb(255,0,0),qRgb(0,255,0)});source.fill(0);source.setPixel(8,8,1);
    const QString path=dir.filePath("indexed-color.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto doc=ImageLoader::load(path,3,&error);QVERIFY2(doc,qPrintable(error));
    QCOMPARE(doc->metadata.kind,PixelKind::Color);QVERIFY(!ImageLoader::isProcessable(doc->metadata));
}
void TestCore::mosaicPixels() {
    try {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QStringList paths;
    const int values[4]={20,70,140,220};
    for(int r=0;r<2;++r) for(int c=0;c<2;++c) {
        QImage tile(6,5,QImage::Format_Grayscale8); tile.fill(values[r*2+c]);
        const QString path=dir.filePath(QString("tile_%1_%2.bmp").arg(r).arg(c));
        QVERIFY(tile.save(path,"BMP")); paths<<path;
    }
    QString error; auto token=std::make_shared<CancellationToken>();
    const auto outcome=MosaicBuilder::build(paths,9,{},token,{},&error);const auto doc=outcome.document; QVERIFY2(doc,qPrintable(error));
    QCOMPARE(doc->metadata.width,qint64(12)); QCOMPARE(doc->metadata.height,qint64(10));
    QImage rendered=VipsSupport::renderRegion(doc,0,0,12,10,1.0,&error); QVERIFY2(!rendered.isNull(),qPrintable(error));
    QCOMPARE(rendered.constScanLine(1)[1],uchar(20)); QCOMPARE(rendered.constScanLine(1)[8],uchar(70));
    QCOMPARE(rendered.constScanLine(7)[1],uchar(140)); QCOMPARE(rendered.constScanLine(7)[8],uchar(220));
    } catch (const std::exception& exception) {
        QFAIL(exception.what());
    }
}
void TestCore::mosaicGray16PngPreservesSamples() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QStringList paths;
    const struct { const char* name; quint16 first; quint16 second; } specs[] = {
        {"tile_0_0.png",1000,2000},{"tile_0_2.png",50000,60000}};
    for(const auto& spec:specs){
        QImage image(2,1,QImage::Format_Grayscale16);
        auto* row=reinterpret_cast<quint16*>(image.scanLine(0));row[0]=spec.first;row[1]=spec.second;
        const QString path=dir.filePath(QString::fromLatin1(spec.name));QVERIFY(image.save(path,"PNG"));paths<<path;
    }
    QString error;const auto result=MosaicBuilder::build(
        paths,10,{true},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(result.document,qPrintable(error));
    QCOMPARE(result.document->metadata.kind,PixelKind::Gray16);
    QCOMPARE(result.document->metadata.sourceBitsPerPixel,16);
    const QImage rendered=VipsSupport::renderRegion(result.document,0,0,6,1,1.0,&error);
    QVERIFY2(!rendered.isNull(),qPrintable(error));QCOMPARE(rendered.format(),QImage::Format_Grayscale16);
    const auto* row=reinterpret_cast<const quint16*>(rendered.constScanLine(0));
    QCOMPARE(row[0],quint16(1000));QCOMPARE(row[1],quint16(2000));
    QCOMPARE(row[2],quint16(65535));QCOMPARE(row[3],quint16(65535));
    QCOMPARE(row[4],quint16(50000));QCOMPARE(row[5],quint16(60000));
}
void TestCore::mosaicAllowsDifferentTileSizesAndPrefixes() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    struct TileSpec { const char* name; int width; int height; int value; };
    const TileSpec specs[]={{"alpha_0_0.bmp",3,2,10},{"beta_0_1.bmp",2,3,20},
                            {"gamma_1_0.bmp",4,1,30},{"delta_1_1.bmp",1,2,40}};
    QStringList paths;
    for(const auto& spec:specs){QImage image(spec.width,spec.height,QImage::Format_Grayscale8);image.fill(spec.value);const QString path=dir.filePath(QString::fromLatin1(spec.name));QVERIFY(image.save(path,"BMP"));paths<<path;}
    QString error;const auto outcome=MosaicBuilder::build(paths,10,{},std::make_shared<CancellationToken>(),{},&error);const auto document=outcome.document;
    QVERIFY2(document,qPrintable(error));QCOMPARE(document->metadata.width,qint64(6));QCOMPARE(document->metadata.height,qint64(5));
    const QImage rendered=VipsSupport::renderRegion(document,0,0,6,5,1.0,&error);QVERIFY2(!rendered.isNull(),qPrintable(error));
    QCOMPARE(rendered.constScanLine(0)[0],uchar(10));QCOMPARE(rendered.constScanLine(0)[3],uchar(255));QCOMPARE(rendered.constScanLine(0)[4],uchar(20));
    QCOMPARE(rendered.constScanLine(2)[0],uchar(255));QCOMPARE(rendered.constScanLine(2)[4],uchar(20));
    QCOMPARE(rendered.constScanLine(3)[0],uchar(30));QCOMPARE(rendered.constScanLine(4)[0],uchar(255));QCOMPARE(rendered.constScanLine(4)[4],uchar(40));
}
void TestCore::mosaicRejectsDifferentBitDepths() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QImage binary(4,4,QImage::Format_Mono);binary.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});binary.fill(0);
    QImage gray(4,4,QImage::Format_Grayscale8);gray.fill(128);
    const QString first=dir.filePath("one_0_0.bmp"),second=dir.filePath("two_0_1.bmp");
    QVERIFY(binary.save(first,"BMP"));QVERIFY(gray.save(second,"BMP"));
    QString error;const auto outcome=MosaicBuilder::build({first,second},11,{},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY(!outcome.document);QVERIFY2(error.contains(QStringLiteral("位深不一致")),qPrintable(error));
}
void TestCore::mosaicMissingTilesRequireConfirmation() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QStringList paths;
    struct Spec{int row,column,width,height,value;};const Spec specs[]={{0,0,2,2,10},{0,2,3,2,20},{2,0,2,4,30},{2,2,3,4,40}};
    for(const auto& spec:specs){QImage image(spec.width,spec.height,QImage::Format_Grayscale8);image.fill(spec.value);const QString path=dir.filePath(QString("tile_%1_%2.bmp").arg(spec.row).arg(spec.column));QVERIFY(image.save(path,"BMP"));paths<<path;}
    QString error;const auto strict=MosaicBuilder::build(paths,12,{},std::make_shared<CancellationToken>(),{},&error);
    QCOMPARE(strict.status,MosaicBuildStatus::MissingTiles);QCOMPARE(strict.missingCount,qint64(5));QVERIFY(!strict.document);QVERIFY(error.isEmpty());
    const auto continued=MosaicBuilder::build(paths,13,{true},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(continued.document,qPrintable(error));QCOMPARE(continued.document->metadata.width,qint64(8));QCOMPARE(continued.document->metadata.height,qint64(10));
    const QImage rendered=VipsSupport::renderRegion(continued.document,0,0,8,10,1.0,&error);QVERIFY2(!rendered.isNull(),qPrintable(error));
    QCOMPARE(rendered.constScanLine(0)[0],uchar(10));QCOMPARE(rendered.constScanLine(0)[3],uchar(255));QCOMPARE(rendered.constScanLine(3)[0],uchar(255));QCOMPARE(rendered.constScanLine(6)[0],uchar(30));QCOMPARE(rendered.constScanLine(6)[5],uchar(40));
}
void TestCore::mergedExporterWritesGray8() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QImage source(2,1,QImage::Format_Mono);source.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});source.setPixel(0,0,0);source.setPixel(1,0,1);
    const QString input=dir.filePath("binary.bmp");QVERIFY(source.save(input,"BMP"));QString error;const auto document=ImageLoader::load(input,20,&error);QVERIFY2(document,qPrintable(error));
    const QString output=dir.filePath("merged.bmp");QVERIFY2(MergedImageExporter::writeGray8Bmp(output,document,std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
    QFile file(output);QVERIFY(file.open(QIODevice::ReadOnly));const QByteArray header=file.read(30);QCOMPARE(int(uchar(header[28])|(uchar(header[29])<<8)),8);file.close();
    const auto exported=ImageLoader::load(output,21,&error);QVERIFY2(exported,qPrintable(error));const QImage pixels=VipsSupport::renderRegion(exported,0,0,2,1,1.0,&error);QCOMPARE(pixels.constScanLine(0)[0],uchar(0));QCOMPARE(pixels.constScanLine(0)[1],uchar(255));
}
void TestCore::mergedExporterConvertsColorAndIsAtomic() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QImage source(2,1,QImage::Format_Indexed8);source.setColorTable({qRgb(255,0,0),qRgb(0,255,0)});source.setPixel(0,0,0);source.setPixel(1,0,1);
    const QString input=dir.filePath("color.bmp");QVERIFY(source.save(input,"BMP"));QString error;const auto document=ImageLoader::load(input,22,&error);QVERIFY2(document,qPrintable(error));QCOMPARE(document->metadata.kind,PixelKind::Color);
    const QString output=dir.filePath("gray.bmp");QVERIFY2(MergedImageExporter::writeGray8Bmp(output,document,std::make_shared<CancellationToken>(),{},&error),qPrintable(error));const auto exported=ImageLoader::load(output,23,&error);QVERIFY2(exported,qPrintable(error));const QImage pixels=VipsSupport::renderRegion(exported,0,0,2,1,1.0,&error);QVERIFY(pixels.constScanLine(0)[0]<pixels.constScanLine(0)[1]);
    QFile sentinel(output);QVERIFY(sentinel.open(QIODevice::WriteOnly|QIODevice::Truncate));const QByteArray original("keep-existing");QCOMPARE(sentinel.write(original),qint64(original.size()));sentinel.close();auto cancelled=std::make_shared<CancellationToken>();cancelled->cancel();QVERIFY(!MergedImageExporter::writeGray8Bmp(output,document,cancelled,{},&error));QVERIFY(sentinel.open(QIODevice::ReadOnly));QCOMPARE(sentinel.readAll(),original);
}
void TestCore::mergedExporterRejectsClassicBmpOverflow() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QImage source(1,1,QImage::Format_Grayscale8);source.fill(0);const QString input=dir.filePath("small.bmp");QVERIFY(source.save(input,"BMP"));QString error;auto document=ImageLoader::load(input,24,&error);QVERIFY2(document,qPrintable(error));document->metadata.width=100000;document->metadata.height=50000;QVERIFY(!MergedImageExporter::writeGray8Bmp(dir.filePath("huge.bmp"),document,std::make_shared<CancellationToken>(),{},&error));QVERIFY(error.contains(QStringLiteral("4GB")));QVERIFY(!QFileInfo::exists(dir.filePath("huge.bmp")));
}
void TestCore::fundamentalPatternPeriod() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    constexpr int pitch = 60;
    QImage pattern(480,360,QImage::Format_Grayscale8);
    for(int y=0;y<pattern.height();++y) {
        uchar* row=pattern.scanLine(y);
        for(int x=0;x<pattern.width();++x) {
            const int cx=(x/pitch)*pitch+pitch/2;
            const int cy=(y/pitch)*pitch+pitch/2;
            const double dx=x-cx,dy=y-cy;
            const double darkness=std::exp(-(dx*dx+dy*dy)/(2.0*11.0*11.0));
            row[x]=uchar(std::clamp(int(std::lround(255.0*(1.0-darkness))),0,255));
        }
    }
    const QString path=dir.filePath("period.bmp"); QVERIFY(pattern.save(path,"BMP"));
    QString error; auto doc=ImageLoader::load(path,11,&error); QVERIFY2(doc,qPrintable(error));
    auto token=std::make_shared<CancellationToken>();
    const SegmentationResult result=PatternDetector::detect(doc,token,{},&error); QVERIFY2(error.isEmpty(),qPrintable(error));
    QVERIFY2(std::abs(result.settings.cellWidth-pitch)<=1,qPrintable(QString("width=%1").arg(result.settings.cellWidth)));
    QVERIFY2(std::abs(result.settings.cellHeight-pitch)<=1,qPrintable(QString("height=%1").arg(result.settings.cellHeight)));
    QCOMPARE(result.columns,8); QCOMPARE(result.rows,6);
}
void TestCore::blankImageIsRejected() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QImage blank(257,263,QImage::Format_Grayscale8);blank.fill(255);
    const QString path=dir.filePath("blank.bmp");QVERIFY(blank.save(path,"BMP"));
    QString error;const auto doc=ImageLoader::load(path,12,&error);QVERIFY2(doc,qPrintable(error));
    const auto result=PatternDetector::detect(doc,std::make_shared<CancellationToken>(),{},&error);
    QCOMPARE(result.rows,0);QCOMPARE(result.columns,0);QVERIFY(!error.isEmpty());
}
void TestCore::gridUsesFixedRect() {
    auto document=std::make_shared<ImageDocument>();
    document->metadata.width=100;document->metadata.height=80;
    SegmentationSettings settings{20,10,128,ForegroundPolarity::Dark};
    const auto grid=PatternDetector::makeGrid(document,settings);
    QCOMPARE(grid.columns,5);QCOMPARE(grid.rows,8);
    QCOMPARE(grid.remainderWidth,0);QCOMPARE(grid.remainderHeight,0);
}
void TestCore::gridAdjustsToExactDivisor() {
    auto document=std::make_shared<ImageDocument>();
    document->metadata.width=22000;document->metadata.height=22000;
    SegmentationSettings settings{215,215,128,ForegroundPolarity::Dark};
    const auto grid=PatternDetector::makeGrid(document,settings);
    QCOMPARE(grid.settings.cellWidth,220);QCOMPARE(grid.settings.cellHeight,220);
    QCOMPARE(grid.columns,100);QCOMPARE(grid.rows,100);
    QCOMPARE(grid.remainderWidth,0);QCOMPARE(grid.remainderHeight,0);
}
void TestCore::gridRejectsUnsafeAdjustment() {
    auto document=std::make_shared<ImageDocument>();document->metadata.width=101;document->metadata.height=80;
    const auto grid=PatternDetector::makeGrid(document,{30,10,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.rows,0);QCOMPARE(grid.columns,0);QVERIFY(!grid.warning.isEmpty());
}
void TestCore::gridRejectsExcessiveCellCount() {
    auto document=std::make_shared<ImageDocument>();document->metadata.width=2000;document->metadata.height=2000;
    const auto grid=PatternDetector::makeGrid(document,{1,1,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.rows,0);QCOMPARE(grid.columns,0);QVERIFY(grid.warning.contains(QStringLiteral("安全上限")));
}
void TestCore::manualGridCountExamples() {
    auto document=std::make_shared<ImageDocument>();document->metadata.width=100;document->metadata.height=100;
    auto grid=PatternDetector::makeManualGrid(document,{10,10,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.columns,10);QCOMPARE(grid.rows,10);QCOMPARE(qint64(grid.columns)*grid.rows,qint64(100));
    document->metadata.width=1080;document->metadata.height=250;
    grid=PatternDetector::makeManualGrid(document,{100,80,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.columns,11);QCOMPARE(grid.rows,4);QCOMPARE(qint64(grid.columns)*grid.rows,qint64(44));
}
void TestCore::manualGridPreservesValuesAndCoversRemainder() {
    auto document=std::make_shared<ImageDocument>();document->metadata.width=1080;document->metadata.height=250;
    const SegmentationSettings settings{100,80,151,ForegroundPolarity::Light};
    const auto grid=PatternDetector::makeManualGrid(document,settings);
    QCOMPARE(grid.settings.cellWidth,100);QCOMPARE(grid.settings.cellHeight,80);
    QCOMPARE(grid.settings.threshold,151);QCOMPARE(grid.settings.polarity,ForegroundPolarity::Light);
    QCOMPARE(grid.columns,11);QCOMPARE(grid.rows,4);
    QCOMPARE(grid.remainderWidth,80);QCOMPARE(grid.remainderHeight,10);
    const auto rightEdge=segmentationCellRegion(grid,1080,250,0,10);
    QCOMPARE(rightEdge.x,qint64(1000));QCOMPARE(rightEdge.width,qint64(80));QCOMPARE(rightEdge.height,qint64(80));
    const auto bottomRight=segmentationCellRegion(grid,1080,250,3,10);
    QCOMPARE(bottomRight.width,qint64(80));QCOMPARE(bottomRight.height,qint64(10));
}
void TestCore::partialEdgeTilesAreExportedAtRemainingSize() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(250,180,QImage::Format_Grayscale8);source.fill(123);
    const QString sourcePath=directory.filePath("source.bmp");QVERIFY(source.save(sourcePath,"BMP"));
    QString error;const auto document=ImageLoader::load(sourcePath,31,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{100,100,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.columns,3);QCOMPARE(grid.rows,2);
    const QString outputPath=directory.filePath("tiles");QVERIFY(QDir().mkpath(outputPath));
    QVERIFY2(SplitExporter::exportTiles(outputPath,document,grid,TraversalMode::SnakeZ,false,
                                        std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
    const auto third=ImageLoader::load(QDir(outputPath).filePath("3.bmp"),32,&error);QVERIFY2(third,qPrintable(error));
    QCOMPARE(third->metadata.width,qint64(50));QCOMPARE(third->metadata.height,qint64(100));
    const auto fourth=ImageLoader::load(QDir(outputPath).filePath("4.bmp"),33,&error);QVERIFY2(fourth,qPrintable(error));
    QCOMPARE(fourth->metadata.width,qint64(50));QCOMPARE(fourth->metadata.height,qint64(80));
    const auto sixth=ImageLoader::load(QDir(outputPath).filePath("6.bmp"),34,&error);QVERIFY2(sixth,qPrintable(error));
    QCOMPARE(sixth->metadata.width,qint64(100));QCOMPARE(sixth->metadata.height,qint64(80));
}
void TestCore::splitExportProgressIsBoundedAndMonotonic() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(100,1000,QImage::Format_Grayscale8);source.fill(123);
    const QString sourcePath=directory.filePath("source.bmp");QVERIFY(source.save(sourcePath,"BMP"));
    QString error;const auto document=ImageLoader::load(sourcePath,35,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{10,1000,128,ForegroundPolarity::Dark});
    const QString outputPath=directory.filePath("tiles");QVERIFY(QDir().mkpath(outputPath));
    std::vector<TaskProgress> updates;
    QVERIFY2(SplitExporter::exportTiles(outputPath,document,grid,TraversalMode::SnakeZ,false,
                                        std::make_shared<CancellationToken>(),
                                        [&](const TaskProgress& value){updates.push_back(value);},&error),
             qPrintable(error));
    QVERIFY(!updates.empty());
    QVERIFY2(updates.size()<=1002,qPrintable(QString("progress updates: %1").arg(updates.size())));
    for(size_t i=1;i<updates.size();++i)QVERIFY(updates[i].completed>=updates[i-1].completed);
    QCOMPARE(updates.back().completed,updates.back().total);
}
void TestCore::exportRollbackRestoresOriginal() {
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QImage source(4,2,QImage::Format_Grayscale8);source.fill(128);
    const QString input=dir.filePath("source.bmp");QVERIFY(source.save(input,"BMP"));
    QString error;const auto doc=ImageLoader::load(input,14,&error);QVERIFY2(doc,qPrintable(error));
    const QString originalPath=dir.filePath("1.bmp");
    QFile original(originalPath);QVERIFY(original.open(QIODevice::WriteOnly));
    const QByteArray originalBytes("original-content");QCOMPARE(original.write(originalBytes),qint64(originalBytes.size()));original.close();
    QVERIFY(QDir(dir.path()).mkdir("2.bmp"));
    SegmentationResult segmentation;segmentation.settings={2,2,128,ForegroundPolarity::Dark};
    segmentation.rows=1;segmentation.columns=2;
    QVERIFY(!SplitExporter::exportTiles(dir.path(),doc,segmentation,TraversalMode::SnakeZ,true,
                                        std::make_shared<CancellationToken>(),{},&error));
    QFile restored(originalPath);QVERIFY(restored.open(QIODevice::ReadOnly));QCOMPARE(restored.readAll(),originalBytes);
    const QStringList leftovers=QDir(dir.path()).entryList({QStringLiteral(".image_handler_*")},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot);
    QVERIFY2(leftovers.isEmpty(),qPrintable(leftovers.join(',')));
}
void TestCore::printCanvasCoordinates() {
    ImageMetadata metadata;metadata.width=1000;metadata.height=500;
    SegmentationResult segmentation;segmentation.settings={500,250,128,ForegroundPolarity::Dark};segmentation.rows=2;segmentation.columns=2;
    PrintExportOptions options{20,10,3,false};
    QCOMPARE(SplitExporter::canvasPosition(metadata,segmentation,QPoint(0,0),options),QPointF(0.0,0.0));
    QCOMPARE(SplitExporter::canvasPosition(metadata,segmentation,QPoint(1,0),options),QPointF(10.0,0.0));
    QCOMPARE(SplitExporter::canvasPosition(metadata,segmentation,QPoint(1,1),options),QPointF(10.0,5.0));
    options.reverseY=true;
    QCOMPARE(SplitExporter::canvasPosition(metadata,segmentation,QPoint(1,1),options),QPointF(10.0,-5.0));
    metadata.width=3;metadata.height=3;segmentation.settings.cellWidth=1;segmentation.settings.cellHeight=1;options={10,10,1,false};
    const QPointF fractional=SplitExporter::canvasPosition(metadata,segmentation,QPoint(1,2),options);
    QVERIFY(std::abs(fractional.x()-10.0/3.0)<1e-12);QVERIFY(std::abs(fractional.y()-20.0/3.0)<1e-12);
}
void TestCore::printPackageExportAndReplacement() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(4,4,QImage::Format_Grayscale8);
    for(int y=0;y<4;++y){auto* row=source.scanLine(y);for(int x=0;x<4;++x)row[x]=uchar(y<2?(x<2?10:20):(x<2?30:40));}
    const QString sourcePath=directory.filePath("source.bmp");QVERIFY(source.save(sourcePath,"BMP"));
    QString error;const auto document=ImageLoader::load(sourcePath,40,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{2,2,128,ForegroundPolarity::Dark});
    const QString outputPath=directory.filePath("print");
    PrintExportOptions options{20,10,3,false};
    std::vector<TaskProgress> progress;
    QVERIFY2(SplitExporter::exportPrintPackage(outputPath,document,grid,TraversalMode::SnakeZ,
                                               options,false,std::make_shared<CancellationToken>(),
                                               [&](const TaskProgress& value){progress.push_back(value);},&error),qPrintable(error));
    QVERIFY(!progress.empty());QCOMPARE(progress.back().completed,progress.back().total);
    for(size_t i=1;i<progress.size();++i)QVERIFY(progress[i].completed>=progress[i-1].completed);
    const QList<int> expectedValues={10,20,40,30};
    for(int model=0;model<4;++model){
        const QDir modelDir(QDir(outputPath).filePath(QString::number(model)));
        QCOMPARE(modelDir.entryList({QStringLiteral("*.bmp")},QDir::Files,QDir::Name).size(),3);
        QFile first(modelDir.filePath("1.bmp"));QFile second(modelDir.filePath("2.bmp"));QFile third(modelDir.filePath("3.bmp"));
        QVERIFY(first.open(QIODevice::ReadOnly));QVERIFY(second.open(QIODevice::ReadOnly));QVERIFY(third.open(QIODevice::ReadOnly));
        const QByteArray bytes=first.readAll();QCOMPARE(second.readAll(),bytes);QCOMPARE(third.readAll(),bytes);
        const auto tile=ImageLoader::load(modelDir.filePath("1.bmp"),41+model,&error);QVERIFY2(tile,qPrintable(error));
        const QImage pixels=VipsSupport::renderRegion(tile,0,0,1,1,1.0,&error);QVERIFY2(!pixels.isNull(),qPrintable(error));
        QCOMPARE(qGray(pixels.pixel(0,0)),expectedValues[model]);
    }
    QFile job(QDir(outputPath).filePath("job.json"));QVERIFY(job.open(QIODevice::ReadOnly));const QByteArray firstJob=job.readAll();job.close();
    QVERIFY(firstJob.contains("\"by\": [0.0, 0.0, 0.0]"));
    QVERIFY(firstJob.contains("\"by\": [10.0, 5.0, 0.0]"));
    QJsonParseError parseError;const QJsonDocument json=QJsonDocument::fromJson(firstJob,&parseError);QCOMPARE(parseError.error,QJsonParseError::NoError);
    const QJsonArray models=json.object().value("models").toArray();const QJsonArray commands=json.object().value("commands").toArray();
    QCOMPARE(models.size(),4);QCOMPARE(commands.size(),8);QCOMPARE(models[2].toObject().value("label").toString(),QString("2"));QCOMPARE(models[2].toObject().value("path").toString(),QString("2"));
    QCOMPARE(commands[4].toObject().value("by").toArray()[0].toDouble(),10.0);QCOMPARE(commands[4].toObject().value("by").toArray()[1].toDouble(),5.0);QCOMPARE(commands[5].toObject().value("id").toInt(),2);

    QDir output(outputPath);QVERIFY(output.mkdir("9"));QFile stale(output.filePath("9/old.txt"));QVERIFY(stale.open(QIODevice::WriteOnly));stale.write("old");stale.close();
    QFile unrelated(output.filePath("keep.txt"));QVERIFY(unrelated.open(QIODevice::WriteOnly));unrelated.write("keep");unrelated.close();
    QVERIFY(!SplitExporter::exportPrintPackage(outputPath,document,grid,TraversalMode::SnakeZ,
                                               options,false,std::make_shared<CancellationToken>(),{},&error));
    QVERIFY(error.startsWith("PRINT_TARGETS_EXIST:"));QVERIFY(QDir(output.filePath("9")).exists());
    options.repeatTimes=2;options.reverseY=true;error.clear();
    QVERIFY2(SplitExporter::exportPrintPackage(outputPath,document,grid,TraversalMode::SnakeZ,
                                               options,true,std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
    QVERIFY(!QDir(output.filePath("9")).exists());QVERIFY(QFileInfo::exists(output.filePath("keep.txt")));
    for(int model=0;model<4;++model)QCOMPARE(QDir(output.filePath(QString::number(model))).entryList({QStringLiteral("*.bmp")},QDir::Files).size(),2);
    QVERIFY(job.open(QIODevice::ReadOnly));const QByteArray replacedJob=job.readAll();job.close();QVERIFY(replacedJob.contains("\"by\": [10.0, -5.0, 0.0]"));
    auto cancelled=std::make_shared<CancellationToken>();cancelled->cancel();
    QVERIFY(!SplitExporter::exportPrintPackage(outputPath,document,grid,TraversalMode::SnakeZ,
                                               options,true,cancelled,{},&error));
    QVERIFY(job.open(QIODevice::ReadOnly));QCOMPARE(job.readAll(),replacedJob);
}
void TestCore::exportPreflightEstimatesAndSpacePolicy() {
    auto document=std::make_shared<ImageDocument>();
    document->metadata.width=5;document->metadata.height=3;document->metadata.kind=PixelKind::Binary1;
    SegmentationResult grid;grid.settings={4,2,128,ForegroundPolarity::Dark};grid.rows=2;grid.columns=2;
    ExportRequest request{ExportMode::SplitTiles,{},document,grid,TraversalMode::SnakeZ,{},true};
    QString error;
    QCOMPARE(ExportPreflight::bmpFileSize(PixelKind::Binary1,4,2),qint64(70));
    QCOMPARE(ExportPreflight::estimateBmpBytes(request,&error),qint64(272));
    QVERIFY2(error.isEmpty(),qPrintable(error));
    auto estimate=ExportPreflight::estimateOutput(request);QVERIFY2(estimate.valid,qPrintable(estimate.error));
    QCOMPARE(estimate.bmpFileCount,qint64(4));QCOMPARE(estimate.estimatedBmpBytes,qint64(272));
    QCOMPARE(estimate.estimatedTotalBytes,qint64(272+8*1024));
    QCOMPARE(estimate.requiredBytes,qint64(272+8*1024+16*1024*1024));
    document->metadata.kind=PixelKind::Gray8;
    QCOMPARE(ExportPreflight::estimateBmpBytes(request,&error),qint64(4336));
    request.mode=ExportMode::PrintPackage;request.printOptions={20,10,3,false};
    QCOMPARE(ExportPreflight::estimateBmpBytes(request,&error),qint64(13008));
    estimate=ExportPreflight::estimateOutput(request);QVERIFY2(estimate.valid,qPrintable(estimate.error));
    QCOMPARE(estimate.bmpFileCount,qint64(12));QCOMPARE(estimate.estimatedBmpBytes,qint64(13008));
    QCOMPARE(estimate.estimatedTotalBytes,qint64(13008+8*1024+4*220));
    QCOMPARE(estimate.requiredBytes,estimate.estimatedTotalBytes+16*1024*1024);

    document->metadata.width=131070;document->metadata.height=65500;
    request.segmentation.settings.cellWidth=65535;request.segmentation.settings.cellHeight=65500;
    request.segmentation.rows=1;request.segmentation.columns=2;
    request.printOptions.repeatTimes=INT_MAX;
    estimate=ExportPreflight::estimateOutput(request);QVERIFY(!estimate.valid);
    QVERIFY(estimate.error.contains(QStringLiteral("超过程序可表示范围")));
    QVERIFY(ExportPreflight::hasSufficientSpace(100,100,true));
    QVERIFY(!ExportPreflight::hasSufficientSpace(101,100,true));
    QVERIFY(ExportPreflight::hasSufficientSpace(100,-1,false));
}

void TestCore::storageSizeFormatting() {
    QCOMPARE(formatStorageSize(-1),QStringLiteral("未知"));
    QCOMPARE(formatStorageSize(0),QStringLiteral("0 B"));
    QCOMPARE(formatStorageSize(1023),QStringLiteral("1023 B"));
    QCOMPARE(formatStorageSize(1024),QStringLiteral("1 KB"));
    QCOMPARE(formatStorageSize(1536),QStringLiteral("1.5 KB"));
    QCOMPARE(formatStorageSize(qint64(1024)*1024),QStringLiteral("1 MB"));
    QCOMPARE(formatStorageSize(qint64(1024)*1024*1024),QStringLiteral("1 GB"));
    QCOMPARE(formatStorageSize(qint64(1024)*1024*1024*1024),QStringLiteral("1 TB"));
    QCOMPARE(formatStorageSize(qint64(5)*1024*1024*1024*1024/4),QStringLiteral("1.25 TB"));
}

void TestCore::exportPreflightConflictsAndPortableReports() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(5,3,QImage::Format_Grayscale8);source.fill(77);
    const QString sourcePath=directory.filePath(QString::fromUtf8("源 图.bmp"));QVERIFY(source.save(sourcePath,"BMP"));
    QString error;const auto document=ImageLoader::load(sourcePath,90,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{4,2,128,ForegroundPolarity::Dark});
    const QString outputPath=directory.filePath("tiles");QVERIFY(QDir().mkpath(outputPath));
    ExportRequest request{ExportMode::SplitTiles,outputPath,document,grid,
                          TraversalMode::ClockwiseSpiral,{},true};
    auto token=std::make_shared<CancellationToken>();
    const ExportPreflightResult first=ExportPreflight::inspect(request,token,{});
    QVERIFY2(first.valid,qPrintable(first.blockingError));QCOMPARE(first.bmpFileCount,qint64(4));
    QCOMPARE(first.conflictCount,qint64(0));QCOMPARE(first.firstOutput,QStringLiteral("1.bmp"));
    QCOMPARE(first.lastOutput,QStringLiteral("4.bmp"));
    const ExportExecutionOptions execution{false,true,first.targetFingerprint};
    QVERIFY2(SplitExporter::exportTiles(outputPath,document,grid,TraversalMode::ClockwiseSpiral,
                                        execution,token,{},&error),qPrintable(error));
    QFile report(QDir(outputPath).filePath("export-report.json"));QVERIFY(report.open(QIODevice::ReadOnly));
    const QByteArray reportBytes=report.readAll();report.close();
    QVERIFY(!reportBytes.contains(QDir::toNativeSeparators(directory.path()).toUtf8()));
    QJsonParseError parseError;const QJsonObject root=QJsonDocument::fromJson(reportBytes,&parseError).object();
    QCOMPARE(parseError.error,QJsonParseError::NoError);QCOMPARE(root.value("schemaVersion").toInt(),1);
    QCOMPARE(root.value("mode").toString(),QStringLiteral("split_tiles"));
    QCOMPARE(root.value("source").toObject().value("name").toString(),QString::fromUtf8("源 图.bmp"));
    QCOMPARE(root.value("output").toObject().value("bmpCount").toInteger(),qint64(4));
    QCOMPARE(root.value("output").toObject().value("indexBase").toInt(),1);
    QVERIFY(!root.contains("print"));

    const ExportPreflightResult conflicts=ExportPreflight::inspect(request,token,{});
    QVERIFY(conflicts.valid);QCOMPARE(conflicts.conflictCount,qint64(5));
    QFile changed(QDir(outputPath).filePath("1.bmp"));QVERIFY(changed.open(QIODevice::Append));
    QCOMPARE(changed.write("x",1),qint64(1));changed.close();
    QString fingerprintError;const QString changedFingerprint=ExportPreflight::targetFingerprint(
        request,nullptr,nullptr,&fingerprintError,token,{});
    QVERIFY2(fingerprintError.isEmpty(),qPrintable(fingerprintError));
    QVERIFY(changedFingerprint!=conflicts.targetFingerprint);
    error.clear();
    QVERIFY(!SplitExporter::exportTiles(outputPath,document,grid,TraversalMode::ClockwiseSpiral,
                                         {true,true,conflicts.targetFingerprint},token,{},&error));
    QVERIFY(error.contains(QStringLiteral("预检后发生变化")));

    const QString printPath=directory.filePath("print");QVERIFY(QDir().mkpath(printPath));
    request={ExportMode::PrintPackage,printPath,document,grid,TraversalMode::SnakeZ,
             {20,10,2,true},true};
    const ExportPreflightResult printPreflight=ExportPreflight::inspect(request,token,{});
    QVERIFY2(printPreflight.valid,qPrintable(printPreflight.blockingError));
    QVERIFY2(SplitExporter::exportPrintPackage(printPath,document,grid,TraversalMode::SnakeZ,
        request.printOptions,{false,true,printPreflight.targetFingerprint},token,{},&error),qPrintable(error));
    QFile printReport(QDir(printPath).filePath("export-report.json"));QVERIFY(printReport.open(QIODevice::ReadOnly));
    const QJsonObject printRoot=QJsonDocument::fromJson(printReport.readAll()).object();
    QCOMPARE(printRoot.value("mode").toString(),QStringLiteral("print_package"));
    QCOMPARE(printRoot.value("output").toObject().value("indexBase").toInt(),0);
    QCOMPARE(printRoot.value("print").toObject().value("repeatTimes").toInt(),2);
    QCOMPARE(printRoot.value("print").toObject().value("reverseY").toBool(),true);

    const QString unsafePath=directory.filePath("unsafe");QVERIFY(QDir().mkpath(unsafePath));
    QVERIFY(QDir(unsafePath).mkdir("export-report.json"));request.outputDirectory=unsafePath;
    const auto unsafe=ExportPreflight::inspect(request,token,{});
    QVERIFY(!unsafe.valid);QVERIFY(unsafe.blockingError.contains(QStringLiteral("类型不安全")));
    QCOMPARE(QDir(unsafePath).entryList({QStringLiteral("*.bmp")},QDir::Files).size(),0);
}
void TestCore::exportReportFailureLeavesTargetsUntouched() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(2,2,QImage::Format_Grayscale8);source.fill(50);
    const QString sourcePath=directory.filePath("source.bmp");QVERIFY(source.save(sourcePath,"BMP"));
    QString error;const auto document=ImageLoader::load(sourcePath,91,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{2,2,128,ForegroundPolarity::Dark});
    const QString outputPath=directory.filePath("output");QVERIFY(QDir().mkpath(outputPath));
    QFile original(QDir(outputPath).filePath("1.bmp"));QVERIFY(original.open(QIODevice::WriteOnly));
    const QByteArray originalBytes("original");QCOMPARE(original.write(originalBytes),qint64(originalBytes.size()));original.close();
    ExportRequest request{ExportMode::SplitTiles,outputPath,document,grid,
                          TraversalMode::SnakeZ,{},true};
    auto token=std::make_shared<CancellationToken>();
    const auto preflight=ExportPreflight::inspect(request,token,{});QVERIFY(preflight.valid);
    bool injected=false;
    const auto progress=[&](const TaskProgress&){
        if(injected)return;
        const QStringList temporary=QDir(outputPath).entryList(
            {QStringLiteral(".image_handler_*")},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot);
        if(!temporary.isEmpty())injected=QDir(QDir(outputPath).filePath(temporary.front())).mkdir("export-report.json");
    };
    QVERIFY(!SplitExporter::exportTiles(outputPath,document,grid,TraversalMode::SnakeZ,
        {true,true,preflight.targetFingerprint},token,progress,&error));
    QVERIFY(injected);QVERIFY(error.contains(QStringLiteral("导出报告")));
    QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),originalBytes);
    QCOMPARE(QDir(outputPath).entryList({QStringLiteral(".image_handler_*")},
        QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot).size(),0);
}
void TestCore::realPatternImageFromEnvironment() {
    const QString path=qEnvironmentVariable("IMAGE_HANDLER_REAL_TEST_IMAGE");
    if(path.isEmpty())QSKIP("IMAGE_HANDLER_REAL_TEST_IMAGE is not set");
    QString error;auto document=ImageLoader::load(path,77,&error);QVERIFY2(document,qPrintable(error));
    auto token=std::make_shared<CancellationToken>();
    const auto result=PatternDetector::detect(document,token,{},&error);QVERIFY2(error.isEmpty(),qPrintable(error));
    QCOMPARE(result.settings.cellWidth,220);
    QCOMPARE(result.settings.cellHeight,220);
    QCOMPARE(result.columns,100);
    QCOMPARE(result.rows,100);
}

QTEST_GUILESS_MAIN(TestCore)
#include "TestCore.moc"
