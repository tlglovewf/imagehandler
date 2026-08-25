#include "io/DirectoryScanner.h"
#include "io/BmpStreamWriter.h"
#include "io/SplitExporter.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/VipsSupport.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <set>

class TestCore final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void naturalSort();
    void parseMergeName();
    void snakeTraversal();
    void spiralTraversal();
    void bmpDepthPreserved();
    void mosaicPixels();
};

void TestCore::initTestCase() { QString error; QVERIFY2(VipsSupport::initialize("ImageHandlerTests",&error),qPrintable(error)); }
void TestCore::cleanupTestCase() { VipsSupport::shutdown(); }

void TestCore::naturalSort() { QVERIFY(DirectoryScanner::naturalLessThan("2.bmp","10.bmp")); }
void TestCore::parseMergeName() { MergeTile t; QVERIFY(MosaicBuilder::parseName("sample_part_12_3.bmp",&t)); QCOMPARE(t.prefix,QString("sample_part"));QCOMPARE(t.row,12);QCOMPARE(t.column,3);QVERIFY(!MosaicBuilder::parseName("bad.bmp",&t)); }
void TestCore::snakeTraversal() { const auto p=SplitExporter::traversal(2,3,TraversalMode::SnakeZ); QCOMPARE(p.size(),size_t(6));QCOMPARE(p[0],QPoint(0,0));QCOMPARE(p[2],QPoint(2,0));QCOMPARE(p[3],QPoint(2,1));QCOMPARE(p[5],QPoint(0,1)); }
void TestCore::spiralTraversal() { const auto p=SplitExporter::traversal(3,3,TraversalMode::ClockwiseSpiral); QCOMPARE(p.size(),size_t(9));QCOMPARE(p.front(),QPoint(0,0));QCOMPARE(p[2],QPoint(2,0));QCOMPARE(p.back(),QPoint(1,1));std::set<std::pair<int,int>> unique;for(const auto&q:p)unique.emplace(q.x(),q.y());QCOMPARE(unique.size(),size_t(9)); }
void TestCore::bmpDepthPreserved() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QImage source(16,8,QImage::Format_Mono);
    source.setColorTable({qRgb(0,0,0),qRgb(255,255,255)}); source.fill(0);
    for(int y=2;y<6;++y) for(int x=4;x<12;++x) source.setPixel(x,y,1);
    const QString input=dir.filePath("input.bmp"); QVERIFY(source.save(input,"BMP"));
    QString error; auto doc=ImageLoader::load(input,1,&error); QVERIFY2(doc,qPrintable(error));
    QCOMPARE(doc->metadata.kind,PixelKind::Binary1);
    const QString output=dir.filePath("output.bmp");
    SegmentationSettings settings{8,4,128,ForegroundPolarity::Dark};
    QVERIFY2(BmpStreamWriter::write(output,doc,4,2,8,4,settings,{},&error),qPrintable(error));
    QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray header=file.read(30);
    QCOMPARE(int(uchar(header[28])|(uchar(header[29])<<8)),1);
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
    auto doc=MosaicBuilder::build(paths,9,token,{},&error); QVERIFY2(doc,qPrintable(error));
    QCOMPARE(doc->metadata.width,qint64(12)); QCOMPARE(doc->metadata.height,qint64(10));
    QImage rendered=VipsSupport::renderRegion(doc,0,0,12,10,1.0,&error); QVERIFY2(!rendered.isNull(),qPrintable(error));
    QCOMPARE(rendered.constScanLine(1)[1],uchar(20)); QCOMPARE(rendered.constScanLine(1)[8],uchar(70));
    QCOMPARE(rendered.constScanLine(7)[1],uchar(140)); QCOMPARE(rendered.constScanLine(7)[8],uchar(220));
    } catch (const std::exception& exception) {
        QFAIL(exception.what());
    }
}

QTEST_GUILESS_MAIN(TestCore)
#include "TestCore.moc"
