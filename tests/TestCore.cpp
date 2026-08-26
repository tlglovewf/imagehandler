#include "io/DirectoryScanner.h"
#include "domain/MergeInputSelector.h"
#include "io/BmpStreamWriter.h"
#include "io/MergedImageExporter.h"
#include "io/SplitExporter.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/PatternDetector.h"
#include "imaging/VipsSupport.h"
#include <QFile>
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
    void indexedColorBmpIsNotGrayscale();
    void mosaicPixels();
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
    void realPatternImageFromEnvironment();
};

void TestCore::initTestCase() { QString error; QVERIFY2(VipsSupport::initialize("ImageHandlerTests",&error),qPrintable(error)); }
void TestCore::cleanupTestCase() { VipsSupport::shutdown(); }

void TestCore::naturalSort() { QVERIFY(DirectoryScanner::naturalLessThan("2.bmp","10.bmp")); }
void TestCore::mergeSelectionRules() { const QStringList directory={"a.bmp","b.PNG","c.BMP"};QCOMPARE(MergeInputSelector::resolve(directory,{}),QStringList({"a.bmp","c.BMP"}));QCOMPARE(MergeInputSelector::resolve(directory,{"a.bmp"}),QStringList({"a.bmp","c.BMP"}));QCOMPARE(MergeInputSelector::resolve(directory,{"a.bmp","b.PNG"}),QStringList({"a.bmp","b.PNG"})); }
void TestCore::parseMergeName() { MergeTile t; QVERIFY(MosaicBuilder::parseName("sample_part_12_3.bmp",&t)); QCOMPARE(t.prefix,QString("sample_part"));QCOMPARE(t.row,12);QCOMPARE(t.column,3);QVERIFY(!MosaicBuilder::parseName("bad.bmp",&t)); }
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
