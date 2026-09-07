#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"
#include "ui/LargeImageView.h"

#include <QTemporaryDir>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QtTest>
#include <cmath>

class TestLargeImageView final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void edgeTileAndGridClipping();
    void failedTileIsNotRetriedContinuously();
    void cellSelectionAndTraversalPath();
    void cellMultiSelectionAndSliceShortcut();
    void cellBoxSelectionWithW();
    void groupedCellsShowStableAdaptiveGroupNumbers();
    void gray16PixelProbePreservesRawValue();
    void fixedGrayPointsPreserveRawValuesAndCanBeDeleted();
};

void TestLargeImageView::initTestCase() {
    QString error;
    QVERIFY2(VipsSupport::initialize("LargeImageViewTests", &error), qPrintable(error));
}

void TestLargeImageView::cleanupTestCase() { VipsSupport::shutdown(); }

void TestLargeImageView::edgeTileAndGridClipping() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(300, 300, QImage::Format_Grayscale8);
    source.fill(240);
    const QString path = directory.filePath("edge.bmp");
    QVERIFY(source.save(path, "BMP"));

    QString error;
    const auto document = ImageLoader::load(path, 42, &error);
    QVERIFY2(document, qPrintable(error));

    LargeImageView view;
    view.resize(500, 500);
    view.show();
    view.setDocument(document);
    SegmentationResult grid;
    grid.settings = {50, 50, 128, ForegroundPolarity::Dark};
    grid.rows = 6;
    grid.columns = 6;
    view.setSegmentation(grid);

    QTRY_VERIFY_WITH_TIMEOUT(view.grab().toImage().pixelColor(250, 250).red() > 200, 3000);
    const QImage rendered = view.grab().toImage();

    // The fitted image occupies x/y 10..490. A partial edge tile must not be
    // stretched into the surrounding viewport.
    const QColor outsideImage = rendered.pixelColor(495, 250);
    QVERIFY(outsideImage.red() < 80 && outsideImage.green() < 80 && outsideImage.blue() < 80);

    // A vertical grid line is present inside the image, but must stop at its top edge.
    const QColor outsideGrid = rendered.pixelColor(90, 2);
    QVERIFY(outsideGrid.red() < 80 && outsideGrid.green() < 80 && outsideGrid.blue() < 80);
    QVERIFY(rendered.pixelColor(90, 20).red() > rendered.pixelColor(90, 20).green());
}

void TestLargeImageView::failedTileIsNotRetriedContinuously() {
    auto document = std::make_shared<ImageDocument>();
    document->generation = 99;
    document->metadata.width = 256;
    document->metadata.height = 256;
    document->metadata.kind = PixelKind::Gray8;

    LargeImageView view;
    view.resize(300, 300);
    QSignalSpy errors(&view, &LargeImageView::tileError);
    view.show();
    view.setDocument(document);
    view.grab();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 3000);

    for (int i = 0; i < 5; ++i) {
        view.update();
        view.grab();
        QTest::qWait(30);
    }
    QCOMPARE(errors.count(), 1);
    const QColor failedArea = view.grab().toImage().pixelColor(150, 150);
    QVERIFY(std::abs(failedArea.red() - failedArea.green()) < 8);
    QVERIFY(std::abs(failedArea.green() - failedArea.blue()) < 8);
}

void TestLargeImageView::cellSelectionAndTraversalPath() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(200, 200, QImage::Format_Grayscale8);
    source.fill(240);
    const QString path = directory.filePath("selection.bmp");
    QVERIFY(source.save(path, "BMP"));

    QString error;
    const auto document = ImageLoader::load(path, 100, &error);
    QVERIFY2(document, qPrintable(error));
    LargeImageView view;
    view.resize(220, 220);
    view.show();
    view.setDocument(document);
    SegmentationResult grid;
    grid.settings = {100, 100, 128, ForegroundPolarity::Dark};
    grid.rows = 2;
    grid.columns = 2;
    view.setSegmentation(grid);
    view.setTraversalMode(TraversalMode::SnakeZ);
    view.setTraversalPathVisible(true);
    QSignalSpy selections(&view, &LargeImageView::segmentationCellSelected);
    QSignalSpy activations(&view, &LargeImageView::segmentationCellActivated);
    QSignalSpy sliceActivations(&view, &LargeImageView::segmentationCellSliceActivated);

    QTRY_VERIFY_WITH_TIMEOUT(view.grab().toImage().pixelColor(90, 60).blue() >
                             view.grab().toImage().pixelColor(90, 60).red(), 3000);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(160, 60));
    QCOMPARE(selections.count(), 1);
    QCOMPARE(selections.at(0).at(0).toInt(), 0);
    QCOMPARE(selections.at(0).at(1).toInt(), 1);
    QCOMPARE(activations.count(), 0);
    const QColor selectedColor = view.grab().toImage().pixelColor(170, 70);
    QVERIFY(selectedColor.red() > selectedColor.blue() + 30);

    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(60, 160));
    QTest::mouseMove(&view, QPoint(100, 160));
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, 160));
    QCOMPARE(selections.count(), 1);

    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
    QCOMPARE(selections.count(), 2);
    QCOMPARE(selections.at(1).at(0).toInt(), -1);
    QCOMPARE(selections.at(1).at(1).toInt(), -1);

    view.setSelectedCell(1,0,true);QCoreApplication::processEvents();
    QTest::mouseDClick(&view, Qt::LeftButton, Qt::NoModifier, view.rect().center());
    QCOMPARE(activations.count(), 1);
    QCOMPARE(activations.at(0).at(0).toInt(), 1);
    QCOMPARE(activations.at(0).at(1).toInt(), 0);
    QTest::mouseDClick(&view, Qt::LeftButton, Qt::ShiftModifier, view.rect().center());
    QCOMPARE(activations.count(), 1);
    QCOMPARE(sliceActivations.count(), 1);
    QCOMPARE(sliceActivations.at(0).at(0).toInt(), 1);
    QCOMPARE(sliceActivations.at(0).at(1).toInt(), 0);
    QTest::mouseDClick(&view, Qt::LeftButton, Qt::ControlModifier, view.rect().center());
    QCOMPARE(activations.count(), 1);
    QCOMPARE(sliceActivations.count(), 1);
}

void TestLargeImageView::cellMultiSelectionAndSliceShortcut() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(200,200,QImage::Format_Grayscale8);source.fill(180);
    const QString path=directory.filePath("multi-selection.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,110,&error);QVERIFY2(document,qPrintable(error));
    LargeImageView view;view.resize(220,220);view.show();view.setDocument(document);
    SegmentationResult grid;grid.settings={100,100,128,ForegroundPolarity::Dark};
    grid.rows=2;grid.columns=2;view.setSegmentation(grid);view.setTraversalMode(TraversalMode::SnakeZ);
    QSignalSpy groupSelections(&view,&LargeImageView::segmentationCellsSelected);
    QSignalSpy groupActivations(&view,&LargeImageView::segmentationCellsSliceActivated);

    QTest::mouseClick(&view,Qt::LeftButton,Qt::NoModifier,QPoint(60,60));
    QTest::keyPress(&view,Qt::Key_Q);
    QTest::mouseClick(&view,Qt::LeftButton,Qt::NoModifier,QPoint(160,60));
    QTest::mouseClick(&view,Qt::LeftButton,Qt::NoModifier,QPoint(60,160));
    QTest::keyRelease(&view,Qt::Key_Q);
    QCOMPARE(view.selectedCells(),QVector<QPoint>({QPoint(0,0),QPoint(1,0),QPoint(0,1)}));
    QCOMPARE(view.selectedCell(),QPoint(0,1));
    QVERIFY(groupSelections.count()>=3);

    QTest::keyClick(&view,Qt::Key_F);
    QCOMPARE(groupActivations.count(),1);
    QCOMPARE(groupActivations.at(0).at(0).value<QVector<QPoint>>(),view.selectedCells());
    QCOMPARE(groupActivations.at(0).at(1).toPoint(),QPoint(0,1));

    QTest::keyPress(&view,Qt::Key_Q);
    QTest::mouseClick(&view,Qt::LeftButton,Qt::NoModifier,QPoint(60,160));
    QTest::keyRelease(&view,Qt::Key_Q);
    QCOMPARE(view.selectedCells(),QVector<QPoint>({QPoint(0,0),QPoint(1,0)}));
    QCOMPARE(view.selectedCell(),QPoint(0,0));

    QTest::mouseDClick(&view,Qt::LeftButton,Qt::ShiftModifier,QPoint(160,60));
    QCOMPARE(view.selectedCells(),QVector<QPoint>({QPoint(0,0),QPoint(1,0)}));
    QCOMPARE(view.selectedCell(),QPoint(1,0));
    QCOMPARE(groupActivations.count(),2);

    QTest::keyPress(&view,Qt::Key_Q);
    QFocusEvent focusOut(QEvent::FocusOut,Qt::OtherFocusReason);
    QApplication::sendEvent(&view,&focusOut);
    view.setFocus();QCoreApplication::processEvents();
    QTest::mouseClick(&view,Qt::LeftButton,Qt::NoModifier,QPoint(160,160));
    QCOMPARE(view.selectedCells(),QVector<QPoint>({QPoint(1,1)}));
}

void TestLargeImageView::cellBoxSelectionWithW() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(200,200,QImage::Format_Grayscale8);source.fill(200);
    const QString path=directory.filePath("box-selection.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,115,&error);QVERIFY2(document,qPrintable(error));
    LargeImageView view;view.resize(220,220);view.show();view.setDocument(document);
    SegmentationResult grid;grid.settings={100,100,128,ForegroundPolarity::Dark};
    grid.rows=2;grid.columns=2;view.setSegmentation(grid);view.setTraversalMode(TraversalMode::SnakeZ);
    QSignalSpy groupActivations(&view,&LargeImageView::segmentationCellsSliceActivated);

    QTest::keyPress(&view,Qt::Key_W);
    QTest::mousePress(&view,Qt::LeftButton,Qt::NoModifier,QPoint(20,20));
    QTest::mouseMove(&view,QPoint(200,100));
    QTest::mouseRelease(&view,Qt::LeftButton,Qt::NoModifier,QPoint(200,100));
    QTest::keyRelease(&view,Qt::Key_W);
    QCOMPARE(view.selectedCells(),QVector<QPoint>({QPoint(0,0),QPoint(1,0)}));
    QCOMPARE(view.selectedCell(),QPoint(1,0));
    QTest::keyClick(&view,Qt::Key_F);QCOMPARE(groupActivations.count(),1);

    QTest::keyPress(&view,Qt::Key_W);
    QTest::mousePress(&view,Qt::LeftButton,Qt::NoModifier,QPoint(60,60));
    QTest::mouseRelease(&view,Qt::LeftButton,Qt::NoModifier,QPoint(60,60));
    QTest::keyRelease(&view,Qt::Key_W);
    QCOMPARE(view.selectedCells(),QVector<QPoint>({QPoint(0,0)}));
}

void TestLargeImageView::groupedCellsShowStableAdaptiveGroupNumbers() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(200,100,QImage::Format_Grayscale8);source.fill(240);
    const QString path=directory.filePath("group-labels.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,118,&error);QVERIFY2(document,qPrintable(error));
    LargeImageView view;view.resize(420,240);view.show();view.setDocument(document);
    SegmentationResult grid;grid.settings={100,100,128,ForegroundPolarity::Dark};grid.rows=1;grid.columns=2;
    view.setSegmentation(grid);
    ElementGroupingResult grouping;grouping.documentGeneration=document->generation;grouping.settings=grid.settings;
    grouping.rows=1;grouping.columns=2;grouping.groupByCell={0,0};grouping.valid=true;
    const QColor sharedColor(40,130,220);
    grouping.groups={{0,QPoint(0,0),{QPoint(0,0),QPoint(1,0)},sharedColor}};
    view.setElementGrouping(grouping);

    QTRY_VERIFY_WITH_TIMEOUT(view.grab().toImage().pixelColor(view.rect().center()).red()>100,3000);
    const QImage sameGroup=view.grab().toImage();
    const QRect firstBadge(88,88,44,44),secondBadge(288,88,44,44);
    QCOMPARE(sameGroup.copy(firstBadge),sameGroup.copy(secondBadge));

    grouping.groupByCell={0,1};
    grouping.groups={{0,QPoint(0,0),{QPoint(0,0)},sharedColor},
                     {1,QPoint(1,0),{QPoint(1,0)},sharedColor}};
    view.setElementGrouping(grouping);
    const QImage differentGroups=view.grab().toImage();
    QVERIFY(differentGroups.copy(firstBadge)!=differentGroups.copy(secondBadge));

    view.setTraversalMode(TraversalMode::ClockwiseSpiral);
    QCOMPARE(view.grab().toImage().copy(firstBadge),differentGroups.copy(firstBadge));
    QCOMPARE(view.grab().toImage().copy(secondBadge),differentGroups.copy(secondBadge));

    view.clearElementGrouping();
    const QImage cleared=view.grab().toImage();
    QVERIFY(cleared.copy(firstBadge)!=differentGroups.copy(firstBadge));

    grid.settings.cellWidth=10;grid.columns=20;view.setSegmentation(grid);
    grouping.settings=grid.settings;grouping.columns=20;grouping.groupByCell.fill(0,20);
    grouping.groups={{0,QPoint(0,0),{},sharedColor}};
    for(int column=0;column<20;++column)grouping.groups[0].cells.push_back(QPoint(column,0));
    view.setElementGrouping(grouping);
    const QImage compact=view.grab().toImage();
    const QColor compactCenter=compact.pixelColor(20,120);
    for(int y=112;y<=128;++y)for(int x=15;x<=25;++x)
        QCOMPARE(compact.pixelColor(x,y),compactCenter);
}

void TestLargeImageView::gray16PixelProbePreservesRawValue() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(1,1,QImage::Format_Grayscale16);
    reinterpret_cast<quint16*>(source.scanLine(0))[0]=65535;
    const QString path=directory.filePath("probe16.png");QVERIFY(source.save(path,"PNG"));
    QString error;const auto document=ImageLoader::load(path,120,&error);QVERIFY2(document,qPrintable(error));
    LargeImageView view;view.resize(320,240);view.show();QSignalSpy values(&view,&LargeImageView::pixelValueChanged);view.setDocument(document);
    QMouseEvent move(QEvent::MouseMove,QPointF(160,120),QPointF(160,120),QPointF(160,120),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(&view,&move);
    QTRY_VERIFY_WITH_TIMEOUT(([&values]{for(const auto& row:values)if(row.size()==4&&row[3].toBool()&&row[0].toLongLong()==0&&row[2].toUInt()==65535U)return true;return false;})(),5000);
}

void TestLargeImageView::fixedGrayPointsPreserveRawValuesAndCanBeDeleted() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(2,1,QImage::Format_Grayscale16);
    auto* row=reinterpret_cast<quint16*>(source.scanLine(0));row[0]=32768;row[1]=65535;
    const QString path=directory.filePath("fixed-points16.png");QVERIFY(source.save(path,"PNG"));
    QString error;const auto document=ImageLoader::load(path,130,&error);QVERIFY2(document,qPrintable(error));
    LargeImageView view;view.resize(320,240);view.show();view.setDocument(document);
    QSignalSpy counts(&view,&LargeImageView::fixedGrayPointCountChanged);

    QTest::mouseClick(&view,Qt::LeftButton,Qt::ControlModifier,QPoint(128,120));
    QCOMPARE(view.fixedGrayPoints().size(),1);
    QTest::mouseClick(&view,Qt::LeftButton,Qt::ControlModifier,QPoint(128,120));
    QCOMPARE(view.fixedGrayPoints().size(),1);
    QTest::mouseClick(&view,Qt::LeftButton,Qt::ControlModifier,QPoint(192,120));
    QCOMPARE(view.fixedGrayPoints().size(),2);
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(view.fixedGrayPoints().cbegin(),view.fixedGrayPoints().cend(),
        [](const FixedGrayPoint& point){return point.state==FixedGrayPointState::Ready;}),5000);
    QCOMPARE(view.fixedGrayPoints()[0].x,qint64(0));QCOMPARE(view.fixedGrayPoints()[0].y,qint64(0));
    QCOMPARE(view.fixedGrayPoints()[0].value,32768U);QCOMPARE(view.fixedGrayPoints()[1].value,65535U);

    QTest::mouseClick(&view,Qt::LeftButton,Qt::NoModifier,QPoint(128,120));
    QTest::keyClick(&view,Qt::Key_Delete);
    QCOMPARE(view.fixedGrayPoints().size(),1);QCOMPARE(view.fixedGrayPoints().front().value,65535U);
    view.clearFixedGrayPoints();QCOMPARE(view.fixedGrayPoints().size(),0);QVERIFY(counts.count()>=4);

    QTest::mouseClick(&view,Qt::LeftButton,Qt::ControlModifier,QPoint(192,120));
    QCOMPARE(view.fixedGrayPoints().size(),1);
    auto replacement=std::make_shared<ImageDocument>(*document);replacement->generation=131;
    view.setDocument(replacement);QCOMPARE(view.fixedGrayPoints().size(),0);

    QImage gray8(1,1,QImage::Format_Grayscale8);gray8.fill(128);
    const QString gray8Path=directory.filePath("fixed-point8.bmp");QVERIFY(gray8.save(gray8Path,"BMP"));
    const auto gray8Document=ImageLoader::load(gray8Path,132,&error);QVERIFY2(gray8Document,qPrintable(error));
    view.setDocument(gray8Document);QTest::mouseClick(&view,Qt::LeftButton,Qt::ControlModifier,QPoint(160,120));
    QTRY_VERIFY_WITH_TIMEOUT(!view.fixedGrayPoints().isEmpty()&&view.fixedGrayPoints().front().state==FixedGrayPointState::Ready,5000);
    QCOMPARE(view.fixedGrayPoints().front().value,128U);

    QImage binary(1,1,QImage::Format_Mono);binary.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});binary.setPixel(0,0,1);
    const QString binaryPath=directory.filePath("fixed-point1.bmp");QVERIFY(binary.save(binaryPath,"BMP"));
    const auto binaryDocument=ImageLoader::load(binaryPath,133,&error);QVERIFY2(binaryDocument,qPrintable(error));
    QCOMPARE(binaryDocument->metadata.kind,PixelKind::Binary1);
    view.setDocument(binaryDocument);QTest::mouseClick(&view,Qt::LeftButton,Qt::ControlModifier,QPoint(160,120));
    QTRY_VERIFY_WITH_TIMEOUT(!view.fixedGrayPoints().isEmpty()&&view.fixedGrayPoints().front().state==FixedGrayPointState::Ready,5000);
    QCOMPARE(view.fixedGrayPoints().front().value,1U);
}

QTEST_MAIN(TestLargeImageView)
#include "TestLargeImageView.moc"
