#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"
#include "ui/QuadrantSelectionWidget.h"

#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestQuadrantSelectionWidget final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void evenAndOddGridPartition();
    void clicksReportReplaceAndAddModes();
    void newestThumbnailWins();
};

void TestQuadrantSelectionWidget::initTestCase() {
    qRegisterMetaType<ImageQuadrant>();
    QString error;
    QVERIFY2(VipsSupport::initialize("QuadrantSelectionWidgetTests",&error),qPrintable(error));
}

void TestQuadrantSelectionWidget::cleanupTestCase() { VipsSupport::shutdown(); }

void TestQuadrantSelectionWidget::evenAndOddGridPartition() {
    SegmentationResult even;even.rows=4;even.columns=4;
    QSet<QPoint> covered;
    for(int value=0;value<4;++value){
        const auto cells=QuadrantSelectionWidget::cellsForQuadrant(
            even,static_cast<ImageQuadrant>(value));
        QCOMPARE(cells.size(),4);
        for(const QPoint& cell:cells){QVERIFY(!covered.contains(cell));covered.insert(cell);}
    }
    QCOMPARE(covered.size(),16);

    SegmentationResult odd;odd.rows=3;odd.columns=5;
    QCOMPARE(QuadrantSelectionWidget::cellBoundsForQuadrant(
        odd,ImageQuadrant::TopLeft),QRect(0,0,3,2));
    QCOMPARE(QuadrantSelectionWidget::cellBoundsForQuadrant(
        odd,ImageQuadrant::TopRight),QRect(3,0,2,2));
    QCOMPARE(QuadrantSelectionWidget::cellBoundsForQuadrant(
        odd,ImageQuadrant::BottomLeft),QRect(0,2,3,1));
    QCOMPARE(QuadrantSelectionWidget::cellBoundsForQuadrant(
        odd,ImageQuadrant::BottomRight),QRect(3,2,2,1));
    covered.clear();
    for(int value=0;value<4;++value)
        for(const QPoint& cell:QuadrantSelectionWidget::cellsForQuadrant(
                odd,static_cast<ImageQuadrant>(value))){
            QVERIFY(!covered.contains(cell));covered.insert(cell);
        }
    QCOMPARE(covered.size(),15);

    SegmentationResult single;single.rows=1;single.columns=1;
    QCOMPARE(QuadrantSelectionWidget::cellsForQuadrant(
        single,ImageQuadrant::TopLeft),QVector<QPoint>({QPoint(0,0)}));
    QVERIFY(QuadrantSelectionWidget::cellsForQuadrant(
        single,ImageQuadrant::BottomRight).isEmpty());
    QVERIFY(QuadrantSelectionWidget::allCells({}).isEmpty());
}

void TestQuadrantSelectionWidget::clicksReportReplaceAndAddModes() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(400,400,QImage::Format_Grayscale8);source.fill(190);
    const QString path=directory.filePath("quadrants.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,200,&error);QVERIFY2(document,qPrintable(error));
    SegmentationResult grid;grid.settings={100,100,128,ForegroundPolarity::Dark};grid.rows=4;grid.columns=4;
    QuadrantSelectionWidget widget;widget.resize(240,210);widget.setSource(document,grid);widget.show();
    QSignalSpy selections(&widget,&QuadrantSelectionWidget::quadrantSelected);
    QSignalSpy previews(&widget,&QuadrantSelectionWidget::selectionPreviewChanged);
    QSignalSpy all(&widget,&QuadrantSelectionWidget::selectAllRequested);
    QSignalSpy clear(&widget,&QuadrantSelectionWidget::clearRequested);

    QTest::mouseClick(&widget,Qt::LeftButton,Qt::NoModifier,QPoint(80,40));
    QVERIFY(previews.count()>0);
    QCOMPARE(previews.last().at(0).toRect(),QRect(0,0,2,2));
    QCOMPARE(selections.count(),1);
    QCOMPARE(selections.at(0).at(0).value<ImageQuadrant>(),ImageQuadrant::TopLeft);
    QCOMPARE(selections.at(0).at(1).toBool(),false);
    widget.setSelectedCells(QuadrantSelectionWidget::cellsForQuadrant(
        grid,ImageQuadrant::TopLeft));
    QVERIFY(widget.isQuadrantSelected(ImageQuadrant::TopLeft));
    QVERIFY(!widget.isQuadrantSelected(ImageQuadrant::TopRight));
    QTest::mouseMove(&widget,QPoint(220,170));
    QVERIFY(widget.isQuadrantSelected(ImageQuadrant::TopLeft));
    QTest::mouseClick(&widget,Qt::LeftButton,Qt::ControlModifier,QPoint(160,40));
    QCOMPARE(selections.count(),2);
    QCOMPARE(selections.at(1).at(0).value<ImageQuadrant>(),ImageQuadrant::TopRight);
    QCOMPARE(selections.at(1).at(1).toBool(),true);
    QVector<QPoint> combined=QuadrantSelectionWidget::cellsForQuadrant(
        grid,ImageQuadrant::TopLeft);
    combined+=QuadrantSelectionWidget::cellsForQuadrant(grid,ImageQuadrant::TopRight);
    widget.setSelectedCells(combined);
    QVERIFY(widget.isQuadrantSelected(ImageQuadrant::TopLeft));
    QVERIFY(widget.isQuadrantSelected(ImageQuadrant::TopRight));

    QTest::mouseClick(widget.findChild<QPushButton*>(QStringLiteral("quadrantSelectAllButton")),Qt::LeftButton);
    QTest::mouseClick(widget.findChild<QPushButton*>(QStringLiteral("quadrantClearButton")),Qt::LeftButton);
    QCOMPARE(all.count(),1);QCOMPARE(clear.count(),1);
}

void TestQuadrantSelectionWidget::newestThumbnailWins() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(640,480,QImage::Format_Grayscale8);source.fill(140);
    const QString path=directory.filePath("thumbnail.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;
    const auto first=ImageLoader::load(path,210,&error);QVERIFY2(first,qPrintable(error));
    const auto second=ImageLoader::load(path,211,&error);QVERIFY2(second,qPrintable(error));
    SegmentationResult grid;grid.settings={100,100,128,ForegroundPolarity::Dark};grid.rows=5;grid.columns=7;
    QuadrantSelectionWidget widget;widget.setSource(first,grid);widget.setSource(second,grid);
    QSignalSpy ready(&widget,&QuadrantSelectionWidget::thumbnailReady);
    QTRY_COMPARE_WITH_TIMEOUT(widget.displayedThumbnailGeneration(),quint64(211),5000);
    QVERIFY(ready.count()>=1);
    for(const auto& arguments:ready)QCOMPARE(arguments.at(0).toULongLong(),quint64(211));
}

QTEST_MAIN(TestQuadrantSelectionWidget)
#include "TestQuadrantSelectionWidget.moc"
