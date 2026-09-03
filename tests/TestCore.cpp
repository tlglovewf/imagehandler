#include "io/DirectoryScanner.h"
#include "domain/MergeInputSelector.h"
#include "domain/StorageSizeFormatter.h"
#include "io/BmpStreamWriter.h"
#include "io/MergedImageExporter.h"
#include "io/MaskSliceExporter.h"
#include "io/ExportPreflight.h"
#include "io/LayeredPackageExporter.h"
#include "io/SplitExporter.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/MaskSliceGenerator.h"
#include "imaging/MaskStackMeshBuilder.h"
#include "imaging/LayeredMaskProcessor.h"
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
    void pointCloudRegionPreservesLocalSamples();
    void pointCloudRegionExtrema();
    void pointCloudRootBudget();
    void pointCloudMappingOptions();
    void maskSliceAnalysisAndExport();
    void maskSliceZeroHeightAndLimits();
    void maskStackGeometryAndSampling();
    void pointCloudImageFromEnvironment();
    void gray16ImageFromEnvironment();
    void gray16MosaicDirectoryFromEnvironment();
    void indexedColorBmpIsNotGrayscale();
    void mosaicPixels();
    void mosaicGray16PngPreservesSamples();
    void mosaicAllowsDifferentTileSizesAndPrefixes();
    void mosaicRejectsDifferentBitDepths();
    void mosaicMissingTilesRequireConfirmation();
    void mosaicProgressIsCumulative();
    void mergedExporterWritesGray8();
    void mergedExporterWritesGray16PngWithoutDownsampling();
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
    void selectedTilesKeepGlobalNamesAndPixels();
    void selectedBinaryTileKeepsBitsIndependentOfThreshold();
    void splitExportProgressIsBoundedAndMonotonic();
    void exportRollbackRestoresOriginal();
    void printCanvasCoordinates();
    void printPackageExportAndReplacement();
    void layeredPackageUsesPhysicalCoordinatesAndUniformLayers();
    void layeredMasksPadTransformAndValidate();
    void exportPreflightEstimatesAndSpacePolicy();
    void storageSizeFormatting();
    void exportPreflightConflictsAndPortableReports();
    void exportReportFailureLeavesTargetsUntouched();
    void realPatternImageFromEnvironment();
};

void TestCore::initTestCase() { QString error; QVERIFY2(VipsSupport::initialize("ImageHandlerTests",&error),qPrintable(error)); }
void TestCore::cleanupTestCase() { VipsSupport::shutdown(); }

void TestCore::maskSliceAnalysisAndExport() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(2,1,QImage::Format_Grayscale8);source.scanLine(0)[0]=0;source.scanLine(0)[1]=255;
    const QString sourcePath=directory.filePath("slice-source.bmp");QVERIFY(source.save(sourcePath,"BMP"));
    QString error;const auto document=ImageLoader::load(sourcePath,950,&error);QVERIFY2(document,qPrintable(error));
    MaskSliceRequest request{document,{0,0,2,1},{0.25,1.0,false},{0.1,4,2,false}};
    const auto token=std::make_shared<CancellationToken>();
    const MaskSliceAnalysis analysis=MaskSliceGenerator::analyze(request,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(analysis.layerCount,10);
    QCOMPARE(analysis.sourceSize,QSize(2,1));QCOMPARE(analysis.thumbnails.size(),10);
    QVERIFY(std::abs(analysis.actualMaximumHeight-1.0)<0.000001);
    for(int layer=0;layer<analysis.thumbnails.size();++layer){const QImage& thumbnail=analysis.thumbnails[layer];QVERIFY(!thumbnail.isNull());QCOMPARE(thumbnail.pixelColor(0,0),layer==0?QColor(Qt::white):QColor(Qt::black));QCOMPARE(thumbnail.pixelColor(thumbnail.width()-1,0),QColor(Qt::white));}

    const QString outputPath=directory.filePath("masks");QVERIFY(QDir().mkpath(outputPath));
    QVERIFY2(MaskSliceExporter::exportAll(outputPath,request,analysis.layerCount,false,token,{},&error),qPrintable(error));
    QCOMPARE(QDir(outputPath).entryList({QStringLiteral("*.bmp")},QDir::Files).size(),10);
    QCOMPARE(MaskSliceExporter::bmpFileSize(4,2),quint64(70));
    const auto first=ImageLoader::load(QDir(outputPath).filePath("1.bmp"),951,&error);QVERIFY2(first,qPrintable(error));
    QCOMPARE(first->metadata.kind,PixelKind::Binary1);QCOMPARE(first->metadata.width,qint64(4));QCOMPARE(first->metadata.height,qint64(2));
    const QImage pixels=VipsSupport::renderRegion(first,0,0,4,2,1.0,&error);QVERIFY2(!pixels.isNull(),qPrintable(error));
    QCOMPARE(pixels.pixelColor(0,0),QColor(Qt::white));QCOMPARE(pixels.pixelColor(1,0),QColor(Qt::white));
    QCOMPARE(pixels.pixelColor(2,0),QColor(Qt::white));QCOMPARE(pixels.pixelColor(3,0),QColor(Qt::white));

    QFile original(QDir(outputPath).filePath("1.bmp"));QVERIFY(original.open(QIODevice::WriteOnly|QIODevice::Truncate));
    QCOMPARE(original.write("original",8),qint64(8));original.close();
    error.clear();QVERIFY(!MaskSliceExporter::exportAll(outputPath,request,analysis.layerCount,false,token,{},&error));
    QVERIFY(error.startsWith(QStringLiteral("MASK_TARGETS_EXIST:")));
    QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),QByteArray("original"));original.close();
    error.clear();QVERIFY2(MaskSliceExporter::exportAll(outputPath,request,analysis.layerCount,true,token,{},&error),qPrintable(error));
    QCOMPARE(QDir(outputPath).entryList({QStringLiteral(".image_handler_masks_*")},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot).size(),0);

    request.settings.invertMask=true;
    const auto inverted=MaskSliceGenerator::analyze(request,token,{},&error);QCOMPARE(inverted.layerCount,10);
    QCOMPARE(inverted.thumbnails.front().pixelColor(0,0),QColor(Qt::black));
    QCOMPARE(inverted.thumbnails.front().pixelColor(inverted.thumbnails.front().width()-1,0),QColor(Qt::black));
}

void TestCore::maskSliceZeroHeightAndLimits() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage zero(1,1,QImage::Format_Grayscale16);reinterpret_cast<quint16*>(zero.scanLine(0))[0]=0;
    const QString path=directory.filePath("zero16.png");QVERIFY(zero.save(path,"PNG"));
    QString error;const auto document=ImageLoader::load(path,960,&error);QVERIFY2(document,qPrintable(error));
    MaskSliceRequest request{document,{0,0,1,1},{0.25,1.0,false},{0.1,1,1,false}};
    const auto token=std::make_shared<CancellationToken>();
    const auto analysis=MaskSliceGenerator::analyze(request,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(analysis.actualMaximumHeight,0.0);QCOMPARE(analysis.layerCount,10);
    QVERIFY(analysis.stackPreview);QVERIFY(std::all_of(analysis.stackPreview->columnLayers.cbegin(),analysis.stackPreview->columnLayers.cend(),[](quint16 level){return level==1;}));
    QVERIFY(analysis.stackPreview->solidGeometry);QVERIFY(!analysis.stackPreview->solidGeometry->indices.empty());
    QCOMPARE(analysis.thumbnails.front().pixelColor(0,0),QColor(Qt::white));
    for(int layer=1;layer<analysis.thumbnails.size();++layer)
        QCOMPARE(analysis.thumbnails[layer].pixelColor(0,0),QColor(Qt::black));
    error.clear();QCOMPARE(MaskSliceGenerator::layerCount(0.95,1.0,0.1,&error),10);QVERIFY(error.isEmpty());
    QCOMPARE(MaskSliceGenerator::layerCount(1.0,1.0,0.0009,&error),0);QVERIFY(error.contains(QStringLiteral("1000")));

    reinterpret_cast<quint16*>(zero.scanLine(0))[0]=32768;const QString middlePath=directory.filePath("middle16.png");QVERIFY(zero.save(middlePath,"PNG"));
    const auto middleDocument=ImageLoader::load(middlePath,961,&error);QVERIFY2(middleDocument,qPrintable(error));
    request.source=middleDocument;error.clear();const auto middle=MaskSliceGenerator::analyze(request,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(middle.layerCount,6);
    QVERIFY(std::abs(middle.actualMaximumHeight-32768.0/65535.0)<0.000001);

    QImage boundary(1,1,QImage::Format_Grayscale8);boundary.fill(242);
    const QString boundaryPath=directory.filePath("boundary.bmp");QVERIFY(boundary.save(boundaryPath,"BMP"));
    const auto boundaryDocument=ImageLoader::load(boundaryPath,962,&error);QVERIFY2(boundaryDocument,qPrintable(error));
    request.source=boundaryDocument;request.mapping={0.25,1.0,false};error.clear();
    const auto boundaryAnalysis=MaskSliceGenerator::analyze(request,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(boundaryAnalysis.layerCount,10);
    QCOMPARE(boundaryAnalysis.thumbnails.back().pixelColor(0,0),QColor(Qt::white));

    QImage binary(2,1,QImage::Format_Mono);binary.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});binary.setPixel(0,0,0);binary.setPixel(1,0,1);
    const QString binaryPath=directory.filePath("binary.bmp");QVERIFY(binary.save(binaryPath,"BMP"));
    const auto binaryDocument=ImageLoader::load(binaryPath,963,&error);QVERIFY2(binaryDocument,qPrintable(error));
    QCOMPARE(binaryDocument->metadata.kind,PixelKind::Binary1);
    request.source=binaryDocument;request.region={0,0,2,1};request.settings={0.5,2,1,false};error.clear();
    const auto binaryAnalysis=MaskSliceGenerator::analyze(request,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(binaryAnalysis.layerCount,2);
    QCOMPARE(binaryAnalysis.thumbnails.back().pixelColor(0,0),QColor(Qt::black));
    QCOMPARE(binaryAnalysis.thumbnails.back().pixelColor(1,0),QColor(Qt::white));

    const QString cancelledOutput=directory.filePath("cancelled");QVERIFY(QDir().mkpath(cancelledOutput));
    QFile existing(QDir(cancelledOutput).filePath("1.bmp"));QVERIFY(existing.open(QIODevice::WriteOnly));QCOMPARE(existing.write("keep",4),qint64(4));existing.close();
    const auto cancelledToken=std::make_shared<CancellationToken>();cancelledToken->cancel();
    error.clear();QVERIFY(!MaskSliceExporter::exportAll(cancelledOutput,request,binaryAnalysis.layerCount,true,cancelledToken,{},&error));
    QVERIFY(existing.open(QIODevice::ReadOnly));QCOMPARE(existing.readAll(),QByteArray("keep"));
    QCOMPARE(QDir(cancelledOutput).entryList({QStringLiteral(".image_handler_masks_*")},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot).size(),0);
}

void TestCore::maskStackGeometryAndSampling() {
    MaskStackPreviewData grid;
    grid.gridSize={2,1};grid.requestedOutputSize={2,1};grid.physicalSize={2.0,1.0};
    grid.columnLayers={1,2};grid.layerHeight=0.1;grid.layerCount=2;
    const auto token=std::make_shared<CancellationToken>();
    const auto solid=MaskStackMeshBuilder::buildSolid(grid,token);
    QVERIFY(solid);QCOMPARE(solid->indices.size(),size_t(54));
    QVERIFY(std::any_of(solid->vertices.cbegin(),solid->vertices.cend(),
        [](const MaskStackVertex& value){return value.faceKind==0.0F&&std::abs(value.z-0.2F)<0.00001F;}));
    QVERIFY(std::any_of(solid->vertices.cbegin(),solid->vertices.cend(),
        [](const MaskStackVertex& value){return value.faceKind==1.0F&&std::abs(value.z-0.1F)<0.00001F;}));
    const auto exploded=MaskStackMeshBuilder::buildExploded(grid,token);
    QVERIFY(exploded);QCOMPARE(exploded->layerStride,1);QCOMPARE(exploded->indices.size(),size_t(12));
    MaskStackPreviewData oriented;oriented.gridSize={1,2};oriented.requestedOutputSize={1,2};
    oriented.physicalSize={1.0,2.0};oriented.columnLayers={1,0};oriented.layerHeight=0.1;oriented.layerCount=1;
    const auto orientedSolid=MaskStackMeshBuilder::buildSolid(oriented,token);QVERIFY(orientedSolid);
    QVERIFY(std::all_of(orientedSolid->vertices.cbegin(),orientedSolid->vertices.cend(),
        [](const MaskStackVertex& value){return value.y>=1.0F;}));

    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(2,1,QImage::Format_Grayscale8);source.scanLine(0)[0]=0;source.scanLine(0)[1]=255;
    const QString path=directory.filePath("stack.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,980,&error);QVERIFY2(document,qPrintable(error));
    MaskSliceRequest request{document,{0,0,2,1},{0.25,1.0,false},{0.1,4,2,false}};
    const auto analysis=MaskSliceGenerator::analyze(request,token,{},&error);QVERIFY2(analysis.stackPreview,qPrintable(error));
    QCOMPARE(analysis.stackPreview->gridSize,QSize(4,2));
    QCOMPARE(analysis.stackPreview->physicalSize,QSizeF(0.5,0.25));
    QVERIFY(!analysis.stackPreview->downsampled);
    QCOMPARE(analysis.stackPreview->columnLayers,QVector<quint16>({1,1,10,10,1,1,10,10}));
    const auto logicalLevels=analysis.stackPreview->columnLayers;
    request.settings.invertMask=true;error.clear();
    const auto inverted=MaskSliceGenerator::analyze(request,token,{},&error);QVERIFY2(inverted.stackPreview,qPrintable(error));
    QCOMPARE(inverted.stackPreview->columnLayers,logicalLevels);

    request.settings.outputWidth=1024;request.settings.outputHeight=256;error.clear();
    const auto sampled=MaskSliceGenerator::analyze(request,token,{},&error);QVERIFY2(sampled.stackPreview,qPrintable(error));
    QCOMPARE(sampled.stackPreview->gridSize,QSize(512,128));QVERIFY(sampled.stackPreview->downsampled);
    QCOMPARE(sampled.stackPreview->requestedOutputSize,QSize(1024,256));

    QImage groupSource(6,2,QImage::Format_Grayscale8);groupSource.fill(0);
    for(int y=0;y<2;++y){groupSource.scanLine(y)[0]=255;groupSource.scanLine(y)[1]=255;
        groupSource.scanLine(y)[4]=128;groupSource.scanLine(y)[5]=128;}
    const QString groupPath=directory.filePath("stack-group.bmp");QVERIFY(groupSource.save(groupPath,"BMP"));
    const auto groupDocument=ImageLoader::load(groupPath,981,&error);QVERIFY2(groupDocument,qPrintable(error));
    MaskStackGroupRequest groupRequest{groupDocument,
        {{{0,0,2,2},0,0,0},{{4,0,2,2},0,2,2}},
        {0.5,1.0,false},0.1,1.0,1.0,7};
    error.clear();const auto group=MaskSliceGenerator::analyzeStackGroup(groupRequest,token,{},&error);
    QVERIFY2(group.preview,qPrintable(error));QCOMPARE(group.requestGeneration,quint64(7));
    QCOMPARE(group.commonLayerCount,10);QCOMPARE(group.elements.size(),2);
    QCOMPARE(group.elements[0].layerCount,10);QCOMPARE(group.elements[1].layerCount,6);
    QCOMPARE(group.preview->gridSize,QSize(6,2));QCOMPARE(group.preview->physicalSize,QSizeF(3.0,1.0));
    QCOMPARE(group.preview->layerCount,10);
    QCOMPARE(group.preview->blocks.size(),2);QCOMPARE(group.preview->blocks[0].physicalRect,QRectF(0,0,1,1));
    QCOMPARE(group.preview->blocks[1].physicalRect,QRectF(2,0,1,1));
    QCOMPARE(group.preview->columnLayers,QVector<quint16>({10,10,0,0,6,6,10,10,0,0,6,6}));
    QVERIFY(std::abs(group.preview->actualMaximumHeight-1.0)<0.000001);
    MaskStackGroupRequest zeroGroupRequest=groupRequest;
    zeroGroupRequest.elements={{{2,0,2,2},0,1,1},{{4,0,2,2},0,2,2}};
    zeroGroupRequest.requestGeneration=9;
    error.clear();const auto zeroGroup=MaskSliceGenerator::analyzeStackGroup(
        zeroGroupRequest,token,{},&error);
    QVERIFY2(zeroGroup.preview,qPrintable(error));QCOMPARE(zeroGroup.commonLayerCount,10);
    QCOMPARE(zeroGroup.elements[0].layerCount,10);QCOMPARE(zeroGroup.elements[1].layerCount,6);
    QCOMPARE(zeroGroup.elements[0].actualMaximumHeight,0.0);
    zeroGroupRequest.layerHeight=0.0009;zeroGroupRequest.requestGeneration=10;
    error.clear();const auto excessiveGroup=MaskSliceGenerator::analyzeStackGroup(
        zeroGroupRequest,token,{},&error);
    QVERIFY(!excessiveGroup.preview);QVERIFY(error.contains(QStringLiteral("1000")));
    groupRequest.outputScaleX=200.0;groupRequest.outputScaleY=200.0;groupRequest.requestGeneration=8;
    error.clear();const auto reduced=MaskSliceGenerator::analyzeStackGroup(groupRequest,token,{},&error);
    QVERIFY2(reduced.preview,qPrintable(error));QCOMPARE(reduced.preview->requestedOutputSize,QSize(1200,400));
    QCOMPARE(reduced.preview->gridSize,QSize(512,171));QVERIFY(reduced.preview->downsampled);

    MaskSliceRequest lowRequest{groupDocument,{4,0,2,2},{0.5,1.0,false},{0.1,2,2,false}};
    error.clear();const auto lowAnalysis=MaskSliceGenerator::analyze(lowRequest,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(lowAnalysis.layerCount,6);
    const QString paddedPath=directory.filePath("padded-masks");QVERIFY(QDir().mkpath(paddedPath));
    QVERIFY2(MaskSliceExporter::exportAll(paddedPath,lowRequest,group.commonLayerCount,
                                          false,token,{},&error),qPrintable(error));
    QCOMPARE(QDir(paddedPath).entryList({QStringLiteral("*.bmp")},QDir::Files).size(),10);
    const auto padded=ImageLoader::load(QDir(paddedPath).filePath("7.bmp"),982,&error);
    QVERIFY2(padded,qPrintable(error));
    const QImage paddedPixels=VipsSupport::renderRegion(padded,0,0,2,2,1.0,&error);
    QVERIFY2(!paddedPixels.isNull(),qPrintable(error));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        QCOMPARE(paddedPixels.pixelColor(x,y),QColor(Qt::black));

    lowRequest.settings.invertMask=true;
    const QString invertedPaddedPath=directory.filePath("padded-masks-inverted");
    QVERIFY(QDir().mkpath(invertedPaddedPath));
    error.clear();QVERIFY2(MaskSliceExporter::exportAll(invertedPaddedPath,lowRequest,
        group.commonLayerCount,false,token,{},&error),qPrintable(error));
    const auto invertedPadded=ImageLoader::load(
        QDir(invertedPaddedPath).filePath("7.bmp"),983,&error);
    QVERIFY2(invertedPadded,qPrintable(error));
    const QImage invertedPaddedPixels=VipsSupport::renderRegion(
        invertedPadded,0,0,2,2,1.0,&error);
    QVERIFY2(!invertedPaddedPixels.isNull(),qPrintable(error));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        QCOMPARE(invertedPaddedPixels.pixelColor(x,y),QColor(Qt::white));

    request.generateStackPreview=false;error.clear();
    const auto masksOnly=MaskSliceGenerator::analyze(request,token,{},&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QVERIFY(!masksOnly.stackPreview);
}

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

void TestCore::pointCloudRegionPreservesLocalSamples() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(5,4,QImage::Format_Grayscale16);
    for(int y=0;y<source.height();++y){auto* row=reinterpret_cast<quint16*>(source.scanLine(y));for(int x=0;x<source.width();++x)row[x]=quint16((y*source.width()+x)*3000);}
    const QString path=directory.filePath("region16.png");QVERIFY(source.save(path,"PNG"));
    QString error;const auto image=ImageLoader::load(path,920,&error);QVERIFY2(image,qPrintable(error));
    const ImageRegion region{3,2,2,2};
    const PointCloudSettings settings{0.5,20.0,false,QColor(10,20,30),QColor(200,210,220)};
    const auto cloud=PointCloudBuilder::buildRegion(image,region,settings,921,
        std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(cloud,qPrintable(error));QCOMPARE(cloud->source->metadata.width,qint64(2));QCOMPARE(cloud->source->metadata.height,qint64(2));
    QCOMPARE(cloud->settings.pixelScale,0.5);QCOMPARE(cloud->settings.maximumHeight,20.0);QVERIFY(!cloud->settings.invertHeight);
    const auto& vertices=cloud->rootTiles.front()->vertices;QCOMPARE(vertices.size(),size_t(4));
    QCOMPARE(vertices.front().pixelX,0.0F);QCOMPARE(vertices.front().pixelY,0.0F);QCOMPARE(vertices.back().pixelX,1.0F);QCOMPARE(vertices.back().pixelY,1.0F);
    const float expected=float((2*5+3)*3000)/65535.0F;QVERIFY(std::abs(vertices.front().normalizedGray-expected)<0.00001F);

    QImage binary(3,2,QImage::Format_Mono);binary.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});binary.fill(0);binary.setPixel(2,1,1);
    const QString binaryPath=directory.filePath("region1.bmp");QVERIFY(binary.save(binaryPath,"BMP"));
    const auto binaryImage=ImageLoader::load(binaryPath,922,&error);QVERIFY2(binaryImage,qPrintable(error));
    const auto binaryCloud=PointCloudBuilder::buildRegion(binaryImage,{2,1,1,1},{},923,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(binaryCloud,qPrintable(error));QCOMPARE(binaryCloud->rootTiles.front()->vertices.front().normalizedGray,1.0F);
    QVERIFY(binaryCloud->highestPoint.has_value());QVERIFY(binaryCloud->lowestPoint.has_value());
    QCOMPARE(binaryCloud->highestPoint->pixelX,qint64(0));QCOMPARE(binaryCloud->highestPoint->pixelY,qint64(0));
    QCOMPARE(binaryCloud->lowestPoint->pixelX,qint64(0));QCOMPARE(binaryCloud->lowestPoint->pixelY,qint64(0));
    QCOMPARE(binaryCloud->highestPoint->normalizedGray,1.0);QCOMPARE(binaryCloud->lowestPoint->normalizedGray,1.0);

    QImage gray8(2,1,QImage::Format_Grayscale8);gray8.scanLine(0)[0]=17;gray8.scanLine(0)[1]=231;
    const QString gray8Path=directory.filePath("region8.bmp");QVERIFY(gray8.save(gray8Path,"BMP"));
    const auto gray8Image=ImageLoader::load(gray8Path,924,&error);QVERIFY2(gray8Image,qPrintable(error));
    const auto gray8Cloud=PointCloudBuilder::buildRegion(gray8Image,{1,0,1,1},{},925,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(gray8Cloud,qPrintable(error));QVERIFY(std::abs(gray8Cloud->rootTiles.front()->vertices.front().normalizedGray-231.0F/255.0F)<0.00001F);
}

void TestCore::pointCloudRegionExtrema() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(5,4,QImage::Format_Grayscale8);source.fill(100);
    source.scanLine(1)[1]=40;source.scanLine(1)[2]=220;source.scanLine(1)[3]=220;
    source.scanLine(2)[1]=40;source.scanLine(2)[2]=120;source.scanLine(2)[3]=80;
    const QString path=directory.filePath("extrema8.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto image=ImageLoader::load(path,930,&error);QVERIFY2(image,qPrintable(error));
    PointCloudSettings settings{0.25,55.0,false};
    std::vector<TaskProgress> updates;
    const auto cloud=PointCloudBuilder::buildRegion(image,{1,1,3,2},settings,931,
        std::make_shared<CancellationToken>(),
        [&updates](const TaskProgress& value){updates.push_back(value);},&error);
    QVERIFY2(cloud,qPrintable(error));QCOMPARE(cloud->rootLevel,0);
    QVERIFY(cloud->highestPoint.has_value());QVERIFY(cloud->lowestPoint.has_value());
    QCOMPARE(cloud->highestPoint->pixelX,qint64(1));QCOMPARE(cloud->highestPoint->pixelY,qint64(0));
    QCOMPARE(cloud->lowestPoint->pixelX,qint64(0));QCOMPARE(cloud->lowestPoint->pixelY,qint64(0));
    QVERIFY(std::abs(cloud->highestPoint->normalizedGray-220.0/255.0)<0.000001);
    QVERIFY(std::abs(cloud->lowestPoint->normalizedGray-40.0/255.0)<0.000001);
    QVERIFY(!updates.empty());qint64 previous=-1;
    for(const auto& update:updates){QCOMPARE(update.total,qint64(1000));QVERIFY(update.completed>=previous);QVERIFY(update.completed<=update.total);previous=update.completed;}
    QCOMPARE(updates.back().completed,qint64(1000));

    settings.invertHeight=true;
    const auto inverted=PointCloudBuilder::buildRegion(image,{1,1,3,2},settings,932,
        std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(inverted,qPrintable(error));
    QCOMPARE(inverted->highestPoint->pixelX,qint64(0));QCOMPARE(inverted->highestPoint->pixelY,qint64(0));
    QCOMPARE(inverted->lowestPoint->pixelX,qint64(1));QCOMPARE(inverted->lowestPoint->pixelY,qint64(0));

    const auto wholeImageCloud=PointCloudBuilder::build(image,settings,933,
        std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(wholeImageCloud,qPrintable(error));QVERIFY(!wholeImageCloud->highestPoint.has_value());QVERIFY(!wholeImageCloud->lowestPoint.has_value());

    QImage gray16(2,2,QImage::Format_Grayscale16);gray16.fill(32768);
    reinterpret_cast<quint16*>(gray16.scanLine(0))[0]=0;
    reinterpret_cast<quint16*>(gray16.scanLine(1))[1]=65535;
    const QString path16=directory.filePath("extrema16.png");QVERIFY(gray16.save(path16,"PNG"));
    const auto image16=ImageLoader::load(path16,934,&error);QVERIFY2(image16,qPrintable(error));
    settings.invertHeight=false;
    const auto cloud16=PointCloudBuilder::buildRegion(image16,{0,0,2,2},settings,935,
        std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(cloud16,qPrintable(error));
    QCOMPARE(cloud16->highestPoint->pixelX,qint64(1));QCOMPARE(cloud16->highestPoint->pixelY,qint64(1));
    QCOMPARE(cloud16->lowestPoint->pixelX,qint64(0));QCOMPARE(cloud16->lowestPoint->pixelY,qint64(0));
    QCOMPARE(PointCloudMath::formatAxisMaximum(PointCloudMath::heightForNormalized(
        cloud16->highestPoint->normalizedGray,settings)),QStringLiteral("55"));
    QCOMPARE(PointCloudMath::formatAxisMaximum(PointCloudMath::heightForNormalized(
        cloud16->lowestPoint->normalizedGray,settings)),QStringLiteral("0"));

    QImage large(1001,1000,QImage::Format_Grayscale8);large.fill(127);
    large.scanLine(998)[999]=0;large.scanLine(999)[1000]=255;
    const QString largePath=directory.filePath("extrema-lod.bmp");QVERIFY(large.save(largePath,"BMP"));
    const auto largeImage=ImageLoader::load(largePath,936,&error);QVERIFY2(largeImage,qPrintable(error));
    const auto largeCloud=PointCloudBuilder::buildRegion(largeImage,{0,0,1001,1000},settings,937,
        std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(largeCloud,qPrintable(error));QVERIFY(largeCloud->rootLevel>0);
    QCOMPARE(largeCloud->highestPoint->pixelX,qint64(1000));QCOMPARE(largeCloud->highestPoint->pixelY,qint64(999));
    QCOMPARE(largeCloud->lowestPoint->pixelX,qint64(999));QCOMPARE(largeCloud->lowestPoint->pixelY,qint64(998));
    QCOMPARE(largeCloud->highestPoint->normalizedGray,1.0);QCOMPARE(largeCloud->lowestPoint->normalizedGray,0.0);

    auto cancelledToken=std::make_shared<CancellationToken>();error.clear();
    const auto cancelled=PointCloudBuilder::buildRegion(largeImage,{0,0,1001,1000},settings,938,
        cancelledToken,[cancelledToken](const TaskProgress& value){if(value.completed>=600)cancelledToken->cancel();},&error);
    QVERIFY(!cancelled);QVERIFY(cancelledToken->isCancelled());QVERIFY(error.isEmpty());
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
    QVERIFY(PointCloudMath::maskLayerActive(0.0,0,0.1));QVERIFY(!PointCloudMath::maskLayerActive(0.0,1,0.1));
    QVERIFY(PointCloudMath::maskLayerActive(0.100001,1,0.1));QVERIFY(!PointCloudMath::maskLayerActive(0.1,1,0.1));
    QCOMPARE(PointCloudMath::interpolateColor(0.0,QColor(0,0,255),QColor(0,255,0)),QColor(0,0,255));QCOMPARE(PointCloudMath::interpolateColor(1.0,QColor(0,0,255),QColor(0,255,0)),QColor(0,255,0));QCOMPARE(PointCloudMath::interpolateColor(0.5,QColor(0,0,255),QColor(0,255,0)),QColor(0,128,128));
    QCOMPARE(PointCloudMath::axisMaximum(1,0.25),0.25);QCOMPARE(PointCloudMath::axisMaximum(4,0.25),1.0);QCOMPARE(PointCloudMath::formatAxisMaximum(6999.750000),QStringLiteral("6999.75"));
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

    const QString output=dir.filePath("gray16.bmp");
    const SegmentationSettings settings{3,1,128,ForegroundPolarity::Dark};
    QVERIFY2(BmpStreamWriter::write(output,document,0,0,3,1,settings,{},&error),qPrintable(error));
    const auto exported=ImageLoader::load(output,5,&error);QVERIFY2(exported,qPrintable(error));
    QCOMPARE(exported->metadata.kind,PixelKind::Gray16);
    QCOMPARE(exported->metadata.sourceBitsPerPixel,16);
    QFile outputFile(output);QVERIFY(outputFile.open(QIODevice::ReadOnly));
    const QByteArray outputHeader=outputFile.read(30);QCOMPARE(outputHeader.size(),30);
    QCOMPARE(int(uchar(outputHeader[28])|(uchar(outputHeader[29])<<8)),16);
    const QImage exportedPixels=VipsSupport::renderRegion(exported,0,0,3,1,1.0,&error);
    QVERIFY2(!exportedPixels.isNull(),qPrintable(error));
    const auto* exportedValues=reinterpret_cast<const quint16*>(exportedPixels.constScanLine(0));
    QCOMPARE(exportedValues[0],quint16(0));
    QCOMPARE(exportedValues[1],quint16(32768));
    QCOMPARE(exportedValues[2],quint16(65535));

    const auto grid=PatternDetector::makeManualGrid(document,{1,1,128,ForegroundPolarity::Dark});
    const QString selectedOutput=dir.filePath("selected-gray16");QVERIFY(QDir().mkpath(selectedOutput));
    error.clear();QVERIFY2(SplitExporter::exportSelectedTiles(
        selectedOutput,document,grid,TraversalMode::SnakeZ,{QPoint(1,0)},false,
        std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
    const QString selectedPath=QDir(selectedOutput).filePath("2.png");
    QVERIFY(QFileInfo::exists(selectedPath));
    QVERIFY(!QFileInfo::exists(QDir(selectedOutput).filePath("2.bmp")));
    QFile selectedFile(selectedPath);QVERIFY(selectedFile.open(QIODevice::ReadOnly));
    const QByteArray pngHeader=selectedFile.read(26);QCOMPARE(pngHeader.size(),26);
    QCOMPARE(pngHeader.left(8),QByteArray::fromHex("89504e470d0a1a0a"));
    QCOMPARE(uchar(pngHeader[24]),uchar(16));QCOMPARE(uchar(pngHeader[25]),uchar(0));
    selectedFile.close();
    const auto selected=ImageLoader::load(selectedPath,6,&error);
    QVERIFY2(selected,qPrintable(error));QCOMPARE(selected->metadata.kind,PixelKind::Gray16);
    QCOMPARE(selected->metadata.sourceBitsPerPixel,16);
    QCOMPARE(selected->metadata.width,qint64(1));QCOMPARE(selected->metadata.height,qint64(1));
    const QImage selectedPixels=VipsSupport::renderRegion(selected,0,0,1,1,1.0,&error);
    QVERIFY2(!selectedPixels.isNull(),qPrintable(error));
    const auto* selectedValues=reinterpret_cast<const quint16*>(selectedPixels.constScanLine(0));
    QCOMPARE(selectedValues[0],quint16(32768));
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
    if(QFileInfo(path).suffix().compare(QStringLiteral("png"),Qt::CaseInsensitive)==0){
        const auto grid=PatternDetector::makeManualGrid(document,{220,220,128,ForegroundPolarity::Dark});
        QTemporaryDir output;QVERIFY(output.isValid());
        QVERIFY2(SplitExporter::exportSelectedTiles(
            output.path(),document,grid,TraversalMode::SnakeZ,{QPoint(0,0)},false,
            std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
        const QString exportedPath=output.filePath("1.png");
        QFile file(exportedPath);QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray header=file.read(26);QCOMPARE(header.size(),26);
        QCOMPARE(uchar(header[24]),uchar(16));QCOMPARE(uchar(header[25]),uchar(0));file.close();
        const auto exported=ImageLoader::load(exportedPath,8,&error);QVERIFY2(exported,qPrintable(error));
        QCOMPARE(exported->metadata.kind,PixelKind::Gray16);
        const QImage sourcePixels=VipsSupport::renderRegion(document,0,0,220,220,1.0,&error);
        const QImage exportedPixels=VipsSupport::renderRegion(exported,0,0,220,220,1.0,&error);
        QCOMPARE(exportedPixels,sourcePixels);
    }
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
void TestCore::mosaicProgressIsCumulative() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QStringList paths;
    for(int column=0;column<2;++column){QImage image(2,2,QImage::Format_Grayscale8);image.fill(20+column);const QString path=dir.filePath(QString("tile_0_%1.bmp").arg(column));QVERIFY(image.save(path,"BMP"));paths<<path;}
    std::vector<TaskProgress> updates;QString error;
    const auto result=MosaicBuilder::build(paths,30,{},std::make_shared<CancellationToken>(),
        [&updates](const TaskProgress& value){updates.push_back(value);},&error);
    QVERIFY2(result.document,qPrintable(error));QVERIFY(!updates.empty());
    qint64 previous=-1;QSet<QString> phases;
    for(const auto& update:updates){QCOMPARE(update.total,qint64(10));QVERIFY(update.completed>=previous);QVERIFY(update.completed<=8);previous=update.completed;phases.insert(update.phase);}
    QCOMPARE(previous,qint64(8));QCOMPARE(phases.size(),4);
}
void TestCore::mergedExporterWritesGray8() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QImage source(2,1,QImage::Format_Mono);source.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});source.setPixel(0,0,0);source.setPixel(1,0,1);
    const QString input=dir.filePath("binary.bmp");QVERIFY(source.save(input,"BMP"));QString error;const auto document=ImageLoader::load(input,20,&error);QVERIFY2(document,qPrintable(error));
    const QString output=dir.filePath("merged.bmp");QVERIFY2(MergedImageExporter::writeGray8Bmp(output,document,std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
    QFile file(output);QVERIFY(file.open(QIODevice::ReadOnly));const QByteArray header=file.read(30);QCOMPARE(int(uchar(header[28])|(uchar(header[29])<<8)),8);file.close();
    const auto exported=ImageLoader::load(output,21,&error);QVERIFY2(exported,qPrintable(error));const QImage pixels=VipsSupport::renderRegion(exported,0,0,2,1,1.0,&error);QCOMPARE(pixels.constScanLine(0)[0],uchar(0));QCOMPARE(pixels.constScanLine(0)[1],uchar(255));
}
void TestCore::mergedExporterWritesGray16PngWithoutDownsampling() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QStringList paths;
    const quint16 values[2][2]={{0,32768},{50000,65535}};
    for(int column=0;column<2;++column){
        QImage tile(2,1,QImage::Format_Grayscale16);
        auto* row=reinterpret_cast<quint16*>(tile.scanLine(0));
        row[0]=values[column][0];row[1]=values[column][1];
        const QString path=dir.filePath(QString("tile_0_%1.png").arg(column));
        QVERIFY(tile.save(path,"PNG"));paths<<path;
    }
    QString error;const auto result=MosaicBuilder::build(
        paths,25,{},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(result.document,qPrintable(error));
    QCOMPARE(result.document->metadata.kind,PixelKind::Gray16);
    QCOMPARE(result.document->metadata.sourceBitsPerPixel,16);

    const QString output=dir.filePath("merged.png");
    std::vector<TaskProgress> updates;
    QVERIFY2(MergedImageExporter::writePng(
        output,result.document,std::make_shared<CancellationToken>(),
        [&](const TaskProgress& value){updates.push_back(value);},&error),qPrintable(error));
    QVERIFY(!updates.empty());QCOMPARE(updates.back().completed,updates.back().total);
    QFile file(output);QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray header=file.read(26);QCOMPARE(header.size(),26);
    QCOMPARE(header.left(8),QByteArray::fromHex("89504e470d0a1a0a"));
    QCOMPARE(uchar(header[24]),uchar(16));QCOMPARE(uchar(header[25]),uchar(0));
    file.close();
    const auto exported=ImageLoader::load(output,26,&error);QVERIFY2(exported,qPrintable(error));
    QCOMPARE(exported->metadata.kind,PixelKind::Gray16);
    QCOMPARE(exported->metadata.sourceBitsPerPixel,16);
    QCOMPARE(exported->metadata.width,qint64(4));QCOMPARE(exported->metadata.height,qint64(1));
    const QImage pixels=VipsSupport::renderRegion(exported,0,0,4,1,1.0,&error);
    QVERIFY2(!pixels.isNull(),qPrintable(error));
    const auto* row=reinterpret_cast<const quint16*>(pixels.constScanLine(0));
    QCOMPARE(row[0],quint16(0));QCOMPARE(row[1],quint16(32768));
    QCOMPARE(row[2],quint16(50000));QCOMPARE(row[3],quint16(65535));
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
void TestCore::selectedTilesKeepGlobalNamesAndPixels() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(6,4,QImage::Format_Grayscale8);
    for(int y=0;y<source.height();++y)
        for(int x=0;x<source.width();++x)source.scanLine(y)[x]=uchar(y*10+x);
    const QString input=directory.filePath("selected-source.bmp");QVERIFY(source.save(input,"BMP"));
    QString error;const auto document=ImageLoader::load(input,36,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{2,2,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.columns,3);QCOMPARE(grid.rows,2);
    const QString outputPath=directory.filePath("selected");QVERIFY(QDir().mkpath(outputPath));
    const QVector<QPoint> cells{QPoint(2,0),QPoint(0,1)};
    std::vector<TaskProgress> updates;
    QVERIFY2(SplitExporter::exportSelectedTiles(
        outputPath,document,grid,TraversalMode::SnakeZ,cells,false,
        std::make_shared<CancellationToken>(),
        [&](const TaskProgress& progress){updates.push_back(progress);},&error),qPrintable(error));
    QCOMPARE(QDir(outputPath).entryList({QStringLiteral("*.bmp")},QDir::Files,QDir::Name),
             QStringList({QStringLiteral("3.bmp"),QStringLiteral("6.bmp")}));
    QVERIFY(!updates.empty());QCOMPARE(updates.back().completed,updates.back().total);

    const auto third=ImageLoader::load(QDir(outputPath).filePath("3.bmp"),37,&error);QVERIFY2(third,qPrintable(error));
    const auto sixth=ImageLoader::load(QDir(outputPath).filePath("6.bmp"),38,&error);QVERIFY2(sixth,qPrintable(error));
    QCOMPARE(third->metadata.kind,PixelKind::Gray8);QCOMPARE(third->metadata.width,qint64(2));QCOMPARE(third->metadata.height,qint64(2));
    QCOMPARE(sixth->metadata.kind,PixelKind::Gray8);QCOMPARE(sixth->metadata.width,qint64(2));QCOMPARE(sixth->metadata.height,qint64(2));
    const QImage thirdPixels=VipsSupport::renderRegion(third,0,0,2,2,1.0,&error);QVERIFY2(!thirdPixels.isNull(),qPrintable(error));
    const QImage sixthPixels=VipsSupport::renderRegion(sixth,0,0,2,2,1.0,&error);QVERIFY2(!sixthPixels.isNull(),qPrintable(error));
    QCOMPARE(thirdPixels.constScanLine(0)[0],uchar(4));
    QCOMPARE(thirdPixels.constScanLine(1)[1],uchar(15));
    QCOMPARE(sixthPixels.constScanLine(0)[0],uchar(20));
    QCOMPARE(sixthPixels.constScanLine(1)[1],uchar(31));

    QFile keep(QDir(outputPath).filePath("keep.txt"));QVERIFY(keep.open(QIODevice::WriteOnly));
    QCOMPARE(keep.write("keep"),qint64(4));keep.close();
    QFile conflict(QDir(outputPath).filePath("3.bmp"));QVERIFY(conflict.open(QIODevice::WriteOnly|QIODevice::Truncate));
    QCOMPARE(conflict.write("existing"),qint64(8));conflict.close();
    error.clear();QVERIFY(!SplitExporter::exportSelectedTiles(
        outputPath,document,grid,TraversalMode::SnakeZ,cells,false,
        std::make_shared<CancellationToken>(),{},&error));
    QVERIFY(error.contains(QStringLiteral("3.bmp")));
    QVERIFY(conflict.open(QIODevice::ReadOnly));QCOMPARE(conflict.readAll(),QByteArray("existing"));conflict.close();
    error.clear();QVERIFY2(SplitExporter::exportSelectedTiles(
        outputPath,document,grid,TraversalMode::SnakeZ,cells,true,
        std::make_shared<CancellationToken>(),{},&error),qPrintable(error));
    QVERIFY(keep.open(QIODevice::ReadOnly));QCOMPARE(keep.readAll(),QByteArray("keep"));
    const QStringList leftovers=QDir(outputPath).entryList(
        {QStringLiteral(".image_handler_selected_*")},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot);
    QVERIFY2(leftovers.isEmpty(),qPrintable(leftovers.join(',')));
}
void TestCore::selectedBinaryTileKeepsBitsIndependentOfThreshold() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(7,3,QImage::Format_Mono);
    source.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});
    source.fill(0);
    const QPoint whitePixels[]={{3,0},{5,0},{4,1},{3,2},{4,2},{5,2}};
    for(const QPoint& pixel:whitePixels)source.setPixel(pixel,1);
    const QString input=directory.filePath("selected-binary-source.bmp");
    QVERIFY(source.save(input,"BMP"));
    QString error;const auto document=ImageLoader::load(input,39,&error);
    QVERIFY2(document,qPrintable(error));QCOMPARE(document->metadata.kind,PixelKind::Binary1);
    auto grid=PatternDetector::makeManualGrid(document,{3,2,255,ForegroundPolarity::Dark});
    QCOMPARE(grid.columns,3);QCOMPARE(grid.rows,2);
    const QString outputPath=directory.filePath("selected-binary");
    QVERIFY(QDir().mkpath(outputPath));
    QVERIFY2(SplitExporter::exportSelectedTiles(
        outputPath,document,grid,TraversalMode::SnakeZ,{QPoint(1,0)},false,
        std::make_shared<CancellationToken>(),{},&error),qPrintable(error));

    const QString exportedPath=QDir(outputPath).filePath("2.bmp");
    QFile exportedFile(exportedPath);QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    const QByteArray header=exportedFile.read(30);QCOMPARE(header.size(),30);
    QCOMPARE(int(uchar(header[28])|(uchar(header[29])<<8)),1);
    exportedFile.close();
    const auto exported=ImageLoader::load(exportedPath,40,&error);QVERIFY2(exported,qPrintable(error));
    QCOMPARE(exported->metadata.kind,PixelKind::Binary1);
    QCOMPARE(exported->metadata.width,qint64(3));QCOMPARE(exported->metadata.height,qint64(2));
    const QImage pixels=VipsSupport::renderRegion(exported,0,0,3,2,1.0,&error);
    QVERIFY2(!pixels.isNull(),qPrintable(error));
    for(int y=0;y<2;++y)for(int x=0;x<3;++x)
        QCOMPARE(pixels.pixelColor(x,y),source.pixelColor(x+3,y));
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
void TestCore::layeredPackageUsesPhysicalCoordinatesAndUniformLayers() {
    LayeredPackageOptions options{0.5,1.0,0.3,4,2,false,false,true,
                                  LayerCountStrategy::RoundUp};
    QString error;
    QCOMPARE(LayeredPackageExporter::layerCount(options,&error),4);
    QVERIFY(error.isEmpty());
    options.layerCountStrategy=LayerCountStrategy::RequireExact;
    QCOMPARE(LayeredPackageExporter::layerCount(options,&error),0);
    QVERIFY(error.contains(QStringLiteral("整除")));
    options.maximumHeight=1.2;
    QCOMPARE(LayeredPackageExporter::layerCount(options,&error),4);
    options.maximumHeight=100.1;options.layerHeight=0.1;
    QCOMPARE(LayeredPackageExporter::layerCount(options,&error),0);
    QVERIFY(error.contains(QStringLiteral("1000")));

    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(3,2,QImage::Format_Grayscale8);
    source.setPixel(0,0,0);source.setPixel(1,0,32);source.setPixel(2,0,255);
    source.setPixel(0,1,64);source.setPixel(1,1,128);source.setPixel(2,1,255);
    const QString sourcePath=directory.filePath("layered.bmp");QVERIFY(source.save(sourcePath,"BMP"));
    const auto document=ImageLoader::load(sourcePath,1201,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{2,1,128,ForegroundPolarity::Dark});
    QCOMPARE(grid.columns,2);QCOMPARE(grid.rows,2);
    const QString output=directory.filePath("package");QVERIFY(QDir().mkpath(output));
    LayeredPackageRequest request{output,document,grid,TraversalMode::SnakeZ,
        {0.5,1.0,0.2,4,2,false,false,true,LayerCountStrategy::RoundUp},true};
    request.options.attachToCanvas=false;
    const auto estimate=LayeredPackageExporter::estimateOutput(request);
    QVERIFY2(estimate.valid,qPrintable(estimate.error));QCOMPARE(estimate.bmpFileCount,qint64(20));
    const auto preflight=LayeredPackageExporter::inspect(request);
    QVERIFY2(preflight.valid,qPrintable(preflight.blockingError));
    QVERIFY2(LayeredPackageExporter::exportPackage(request,
        {false,true,preflight.targetFingerprint},std::make_shared<CancellationToken>(),{},&error),
        qPrintable(error));
    for(int block=0;block<4;++block){
        QDir blockDirectory(QDir(output).filePath(QString::number(block)));
        QCOMPARE(blockDirectory.entryList({QStringLiteral("*.bmp")},QDir::Files).size(),5);
        const auto first=ImageLoader::load(blockDirectory.filePath("1.bmp"),1210+block,&error);
        QVERIFY2(first,qPrintable(error));QCOMPARE(first->metadata.kind,PixelKind::Binary1);
        QCOMPARE(first->metadata.height,qint64(2));
        QCOMPARE(first->metadata.width,qint64(4));
    }
    const auto padded=ImageLoader::load(QDir(output).filePath("0/5.bmp"),1220,&error);
    QVERIFY2(padded,qPrintable(error));
    const QImage paddedPixels=VipsSupport::renderRegion(padded,0,0,
        padded->metadata.width,padded->metadata.height,1.0,&error);
    QVERIFY2(!paddedPixels.isNull(),qPrintable(error));
    for(int y=0;y<paddedPixels.height();++y)for(int x=0;x<paddedPixels.width();++x)
        QCOMPARE(paddedPixels.pixelColor(x,y),QColor(Qt::black));

    QFile job(QDir(output).filePath("job.json"));QVERIFY(job.open(QIODevice::ReadOnly));
    const QByteArray jobBytes=job.readAll();job.close();
    QVERIFY(jobBytes.contains("\"path\": \"0\""));
    QVERIFY(jobBytes.contains("\"by\": [1.0, 0.0, 0.0]"));
    QVERIFY(jobBytes.contains("\"by\": [1.0, -0.5, 0.0]"));
    QFile report(QDir(output).filePath("export-report.json"));QVERIFY(report.open(QIODevice::ReadOnly));
    const QJsonObject reportObject=QJsonDocument::fromJson(report.readAll()).object();
    QCOMPARE(reportObject.value("mode").toString(),QStringLiteral("layered_package"));
    const QJsonObject reportOutput=reportObject.value("output").toObject();
    QCOMPARE(reportOutput.value("residualAlignment").toString(),QStringLiteral("top_left"));
    QCOMPARE(reportOutput.value("attachToCanvas").toBool(),false);
    QCOMPARE(reportOutput.value("transformSampling").toString(),QStringLiteral("nearest"));

    const auto replacementPreflight=LayeredPackageExporter::inspect(request);
    QVERIFY(replacementPreflight.valid);QCOMPARE(replacementPreflight.conflictCount,qint64(6));
    error.clear();
    QVERIFY(!LayeredPackageExporter::exportPackage(request,{false,true,{}},
        std::make_shared<CancellationToken>(),{},&error));
    QVERIFY(error.startsWith(QStringLiteral("LAYERED_TARGETS_EXIST:")));
    auto cancelled=std::make_shared<CancellationToken>();cancelled->cancel();error.clear();
    QVERIFY(!LayeredPackageExporter::exportPackage(request,
        {true,true,replacementPreflight.targetFingerprint},cancelled,{},&error));
    QFile unchangedJob(QDir(output).filePath("job.json"));QVERIFY(unchangedJob.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedJob.readAll(),jobBytes);

    const QString unsafeOutput=directory.filePath("unsafe-package");
    QVERIFY(QDir().mkpath(unsafeOutput));QFile unsafe(QDir(unsafeOutput).filePath("0"));
    QVERIFY(unsafe.open(QIODevice::WriteOnly));unsafe.write("not a directory");unsafe.close();
    request.outputDirectory=unsafeOutput;
    const auto unsafePreflight=LayeredPackageExporter::inspect(request);
    QVERIFY(!unsafePreflight.valid);QVERIFY(unsafePreflight.blockingError.contains(QStringLiteral("不安全")));
}
void TestCore::layeredMasksPadTransformAndValidate() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QImage source(3, 1, QImage::Format_Grayscale8);
    source.fill(0); source.setPixel(2, 0, 255);
    const QString path = directory.filePath("residual.bmp");
    QVERIFY(source.save(path, "BMP"));
    QString error;
    const auto document = ImageLoader::load(path, 1301, &error);
    QVERIFY2(document, qPrintable(error));

    LayeredPackageOptions options{0.5, 1.0, 1.0, 4, 2, false, false, false,
                                  LayerCountStrategy::RoundUp};
    options.attachToCanvas = false;
    LayeredMaskRequest request{document, {2, 0, 1, 1}, {2, 1}, options, 1};
    QCOMPARE(LayeredMaskProcessor::contentSize(request), QSize(2, 2));
    QVERIFY2(LayeredMaskProcessor::validate(request, &error), qPrintable(error));
    const QString plain = directory.filePath("plain"); QVERIFY(QDir().mkpath(plain));
    QVERIFY2(LayeredMaskProcessor::exportAll(plain, request,
        std::make_shared<CancellationToken>(), {}, &error), qPrintable(error));
    const auto plainDocument = ImageLoader::load(QDir(plain).filePath("1.bmp"), 1302, &error);
    QVERIFY2(plainDocument, qPrintable(error));
    QCOMPARE(plainDocument->metadata.kind, PixelKind::Binary1);
    QCOMPARE(plainDocument->metadata.width, qint64(4));
    QCOMPARE(plainDocument->metadata.height, qint64(2));
    const QImage plainPixels = VipsSupport::renderRegion(plainDocument, 0, 0, 4, 2, 1.0, &error);
    for (int y = 0; y < 2; ++y) {
        QCOMPARE(plainPixels.pixelColor(0, y), QColor(Qt::white));
        QCOMPARE(plainPixels.pixelColor(1, y), QColor(Qt::white));
        QCOMPARE(plainPixels.pixelColor(2, y), QColor(Qt::black));
        QCOMPARE(plainPixels.pixelColor(3, y), QColor(Qt::black));
    }

    request.options.attachToCanvas = true;
    request.options.canvasWidth = 8; request.options.canvasHeight = 6;
    const QString attached = directory.filePath("attached"); QVERIFY(QDir().mkpath(attached));
    QVERIFY2(LayeredMaskProcessor::exportAll(attached, request,
        std::make_shared<CancellationToken>(), {}, &error), qPrintable(error));
    const auto attachedDocument = ImageLoader::load(QDir(attached).filePath("1.bmp"), 1303, &error);
    QVERIFY2(attachedDocument, qPrintable(error));
    QCOMPARE(attachedDocument->metadata.width, qint64(8));
    QCOMPARE(attachedDocument->metadata.height, qint64(6));
    const QImage attachedPixels = VipsSupport::renderRegion(attachedDocument, 0, 0, 8, 6, 1.0, &error);
    QCOMPARE(attachedPixels.pixelColor(2, 2), QColor(Qt::white));
    QCOMPARE(attachedPixels.pixelColor(3, 3), QColor(Qt::white));
    QCOMPARE(attachedPixels.pixelColor(1, 2), QColor(Qt::black));
    QCOMPARE(attachedPixels.pixelColor(4, 2), QColor(Qt::black));

    const auto grid = PatternDetector::makeManualGrid(
        document, {3, 1, 128, ForegroundPolarity::Dark});
    const QString packagePath = directory.filePath("package-attached");
    QVERIFY(QDir().mkpath(packagePath));
    LayeredPackageRequest package{packagePath, document, grid, TraversalMode::SnakeZ,
                                  request.options, true};
    const auto estimate = LayeredPackageExporter::estimateOutput(package);
    QVERIFY2(estimate.valid, qPrintable(estimate.error));
    QCOMPARE(estimate.estimatedBmpBytes,
             qint64(LayeredMaskProcessor::bmpFileSize(QSize(8, 6))));
    const auto preflight = LayeredPackageExporter::inspect(package);
    QVERIFY2(preflight.valid, qPrintable(preflight.blockingError));
    QVERIFY2(LayeredPackageExporter::exportPackage(package,
        {false, true, preflight.targetFingerprint}, std::make_shared<CancellationToken>(),
        {}, &error), qPrintable(error));
    const auto packageMask = ImageLoader::load(
        QDir(packagePath).filePath("0/1.bmp"), 1304, &error);
    QVERIFY2(packageMask, qPrintable(error));
    QCOMPARE(packageMask->metadata.width, qint64(8));
    QCOMPARE(packageMask->metadata.height, qint64(6));
    QFile report(QDir(packagePath).filePath("export-report.json"));
    QVERIFY(report.open(QIODevice::ReadOnly));
    const QJsonObject output = QJsonDocument::fromJson(report.readAll()).object()
                                   .value("output").toObject();
    QCOMPARE(output.value("attachToCanvas").toBool(), true);
    QCOMPARE(output.value("canvasWidth").toInt(), 8);
    QCOMPARE(output.value("canvasHeight").toInt(), 6);

    request.options.rotationDegrees = 90.0;
    request.options.transformSampling = BinaryTransformSampling::SmoothThreshold;
    const auto preview = LayeredMaskProcessor::preview(
        request, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(preview.stackPreview, qPrintable(error));
    QCOMPARE(preview.outputSize, QSize(8, 6));
    QCOMPARE(preview.thumbnails.size(), 1);
    request.options.scaleY = 5.0;
    QVERIFY(!LayeredMaskProcessor::validate(request, &error));
    QVERIFY(error.contains(QStringLiteral("超过导出底图")));
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
