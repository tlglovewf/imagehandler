#include "imaging/ImageLoader.h"
#include "imaging/VipsSupport.h"
#include "ui/LargeImageView.h"

#include <QTemporaryDir>
#include <QtTest>

class TestLargeImageView final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void edgeTileAndGridClipping();
    void failedTileIsNotRetriedContinuously();
    void cellSelectionAndTraversalPath();
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

    QTRY_VERIFY_WITH_TIMEOUT(view.grab().toImage().pixelColor(90, 60).blue() >
                             view.grab().toImage().pixelColor(90, 60).red(), 3000);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(160, 60));
    QCOMPARE(selections.count(), 1);
    QCOMPARE(selections.at(0).at(0).toInt(), 0);
    QCOMPARE(selections.at(0).at(1).toInt(), 1);
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
}

QTEST_MAIN(TestLargeImageView)
#include "TestLargeImageView.moc"
