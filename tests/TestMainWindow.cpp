#include "imaging/VipsSupport.h"
#include "imaging/ImageLoader.h"
#include "ui/LargeImageView.h"
#include "ui/JobPreviewWidget.h"
#include "ui/MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTabBar>
#include <QWindow>
#include <QtTest>
#include <algorithm>

class TestMainWindow final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void listModesAndMergedExportControl();
    void printCanvasFollowsDocument();
    void jobPreviewWorkspaceAndLinking();
    void denseJobPointCanBeSelectedAfterOrbit();
};

void TestMainWindow::initTestCase() {
    QString error;
    QVERIFY2(VipsSupport::initialize("MainWindowTests", &error), qPrintable(error));
}

void TestMainWindow::cleanupTestCase() { VipsSupport::shutdown(); }

void TestMainWindow::listModesAndMergedExportControl() {
    MainWindow window;
    const auto* directoryList = window.findChild<QListWidget*>(QStringLiteral("directoryImageList"));
    const auto* mergedList = window.findChild<QListWidget*>(QStringLiteral("mergedImageList"));
    const auto* exportButton = window.findChild<QPushButton*>(QStringLiteral("mergedExportButton"));
    const auto* previewButton = window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    const auto* taskTitle = window.findChild<QLabel*>(QStringLiteral("taskTitle"));
    const auto* progress = window.findChild<QProgressBar*>(QStringLiteral("taskProgress"));
    const auto* percent = window.findChild<QLabel*>(QStringLiteral("taskProgressPercent"));
    const auto* gridCount = window.findChild<QLabel*>(QStringLiteral("segmentationGridCount"));
    const auto* cellWidth = window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    const auto* cellHeight = window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    const auto* traversal = window.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"));
    const auto* pathButton = window.findChild<QPushButton*>(QStringLiteral("traversalPathButton"));
    const auto* splitExportButton = window.findChild<QPushButton*>(QStringLiteral("splitExportButton"));
    const auto* selectedCell = window.findChild<QLabel*>(QStringLiteral("selectedSegmentationCell"));
    auto* printMode = window.findChild<QCheckBox*>(QStringLiteral("chk_o_print"));
    auto* printOptions = window.findChild<QWidget*>(QStringLiteral("printOptionsContainer"));
    auto* canvasX = window.findChild<QSpinBox*>(QStringLiteral("canvasX"));
    auto* canvasY = window.findChild<QSpinBox*>(QStringLiteral("canvasY"));
    auto* canvasLock = window.findChild<QCheckBox*>(QStringLiteral("lock"));
    auto* reverseY = window.findChild<QCheckBox*>(QStringLiteral("chk_y_reverse"));
    auto* repeatTimes = window.findChild<QSpinBox*>(QStringLiteral("repeat_times"));
    QVERIFY(directoryList); QVERIFY(mergedList); QVERIFY(exportButton);QVERIFY(previewButton);
    QVERIFY(taskTitle);QVERIFY(progress);QVERIFY(percent);QVERIFY(gridCount);
    QVERIFY(cellWidth);QVERIFY(cellHeight);QVERIFY(traversal);QVERIFY(pathButton);QVERIFY(splitExportButton);QVERIFY(selectedCell);
    QVERIFY(printMode);QVERIFY(printOptions);QVERIFY(canvasX);QVERIFY(canvasY);QVERIFY(canvasLock);QVERIFY(reverseY);QVERIFY(repeatTimes);
    QCOMPARE(directoryList->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(mergedList->selectionMode(), QAbstractItemView::SingleSelection);
    QVERIFY(!exportButton->isEnabled());
    QVERIFY(!previewButton->isEnabled());
    QCOMPARE(taskTitle->property("fullText").toString(),QStringLiteral("就绪"));
    QVERIFY(!progress->isVisible());QVERIFY(!percent->isVisible());
    QVERIFY(!progress->isTextVisible());
    QCOMPARE(gridCount->text(),QStringLiteral("-- × -- = --"));
    QVERIFY(gridCount->styleSheet().contains(QStringLiteral("#d32f2f"),Qt::CaseInsensitive));
    QCOMPARE(cellWidth->value(),220);QCOMPARE(cellHeight->value(),220);
    QVERIFY(pathButton->isCheckable());QVERIFY(!pathButton->isEnabled());QVERIFY(!pathButton->isChecked());
    QVERIFY(!splitExportButton->isEnabled());QCOMPARE(selectedCell->text(),QStringLiteral("未选择"));
    QVERIFY(!printMode->isChecked());QVERIFY(printOptions->isHidden());
    QCOMPARE(canvasX->minimum(),1);QCOMPARE(canvasX->maximum(),INT_MAX);QCOMPARE(canvasX->value(),1);
    QCOMPARE(canvasY->minimum(),1);QCOMPARE(canvasY->maximum(),INT_MAX);QCOMPARE(canvasY->value(),1);
    QVERIFY(canvasLock->isChecked());QVERIFY(!reverseY->isChecked());QCOMPARE(repeatTimes->value(),10);QCOMPARE(repeatTimes->singleStep(),1);
    canvasX->setValue(7);QCOMPARE(canvasY->value(),7);
    canvasY->setValue(8);QCOMPARE(canvasX->value(),8);
    canvasLock->setChecked(false);canvasX->setValue(4);canvasY->setValue(9);QCOMPARE(canvasX->value(),4);QCOMPARE(canvasY->value(),9);
    canvasLock->setChecked(true);QCOMPARE(canvasX->value(),9);QCOMPARE(canvasY->value(),9);
    QVERIFY(qobject_cast<LargeImageView*>(selectedCell->parentWidget()));
    QVERIFY(selectedCell->testAttribute(Qt::WA_TransparentForMouseEvents));
    QCOMPARE(splitExportButton->parentWidget(),previewButton->parentWidget());
    window.show();QTest::qWait(20);
    printMode->setChecked(true);QVERIFY(printOptions->isVisible());QTest::qWait(10);
    QVERIFY(printMode->geometry().bottom()<=printOptions->geometry().top());
    QVERIFY(printOptions->geometry().bottom()<=splitExportButton->geometry().top());
    printMode->setChecked(false);QVERIFY(printOptions->isHidden());QCOMPARE(canvasX->value(),9);QCOMPARE(canvasY->value(),9);
    QVERIFY(splitExportButton->geometry().top()>=previewButton->geometry().bottom());
    const auto* imageView=qobject_cast<LargeImageView*>(selectedCell->parentWidget());
    QVERIFY(std::abs(selectedCell->geometry().center().x()-imageView->rect().center().x())<=2);
    QVERIFY(selectedCell->geometry().bottom()<imageView->rect().bottom());
}

void TestMainWindow::printCanvasFollowsDocument() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage square(6,6,QImage::Format_Grayscale8);square.fill(20);
    QImage rectangle(8,4,QImage::Format_Grayscale8);rectangle.fill(30);
    const QString squarePath=directory.filePath("square.bmp");const QString rectanglePath=directory.filePath("rectangle.bmp");
    QVERIFY(square.save(squarePath,"BMP"));QVERIFY(rectangle.save(rectanglePath,"BMP"));
    QString error;const auto squareDocument=ImageLoader::load(squarePath,101,&error);QVERIFY2(squareDocument,qPrintable(error));
    const auto rectangleDocument=ImageLoader::load(rectanglePath,102,&error);QVERIFY2(rectangleDocument,qPrintable(error));
    MainWindow window;
    auto* canvasX=window.findChild<QSpinBox*>(QStringLiteral("canvasX"));auto* canvasY=window.findChild<QSpinBox*>(QStringLiteral("canvasY"));auto* canvasLock=window.findChild<QCheckBox*>(QStringLiteral("lock"));
    QVERIFY(canvasX);QVERIFY(canvasY);QVERIFY(canvasLock);
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,squareDocument)));
    QCOMPARE(canvasX->value(),6);QCOMPARE(canvasY->value(),6);QVERIFY(canvasLock->isChecked());
    canvasX->setValue(99);
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,rectangleDocument)));
    QCOMPARE(canvasX->value(),8);QCOMPARE(canvasY->value(),4);QVERIFY(!canvasLock->isChecked());
}

void TestMainWindow::jobPreviewWorkspaceAndLinking() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(4,4,QImage::Format_Grayscale8);source.fill(30);
    const QString path=directory.filePath("source.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,201,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;
    auto* importButton=window.findChild<QPushButton*>(QStringLiteral("jobImportButton"));
    auto* layoutMode=window.findChild<QComboBox*>(QStringLiteral("previewLayoutMode"));
    auto* autoJump=window.findChild<QCheckBox*>(QStringLiteral("jobAutoJumpCheck"));
    auto* tabs=window.findChild<QTabBar*>(QStringLiteral("previewTabs"));
    auto* jobView=window.findChild<JobPreviewWidget*>(QStringLiteral("jobPreviewWidget"));
    auto* status=window.findChild<QLabel*>(QStringLiteral("jobPreviewStatus"));
    auto* previewButton=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* cellWidth=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* cellHeight=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* imageView=window.findChild<LargeImageView*>();
    QVERIFY(importButton);QVERIFY(layoutMode);QVERIFY(autoJump);QVERIFY(tabs);QVERIFY(jobView);
    QVERIFY(status);QVERIFY(previewButton);QVERIFY(cellWidth);QVERIFY(cellHeight);QVERIFY(imageView);
    QCOMPARE(layoutMode->currentIndex(),0);QVERIFY(!autoJump->isChecked());QCOMPARE(tabs->count(),2);
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));
    cellWidth->setValue(2);cellHeight->setValue(2);
    window.show();QTest::qWait(30);QTest::mouseClick(previewButton,Qt::LeftButton);
    if(jobView->isVulkanAvailable()){
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(),1,5000);
        QTRY_VERIFY_WITH_TIMEOUT(jobView->document()!=nullptr,5000);
        QCOMPARE(jobView->document()->points.size(),size_t(4));
        QCOMPARE(status->text(),QStringLiteral("4 个点 | 分割联动"));
        const QImage frame=jobView->grabFrame();QVERIFY(!frame.isNull());
        QPoint cyanPixel(-1,-1);for(int y=0;y<frame.height()&&cyanPixel.x()<0;++y)for(int x=0;x<frame.width();++x){const QColor color=frame.pixelColor(x,y);if(color.blue()>color.red()+80&&color.green()>100){cyanPixel={x,y};break;}}
        QVERIFY(cyanPixel.x()>=0);
        QWindow* renderWindow=nullptr;
        for(QWindow* candidate:QGuiApplication::allWindows())if(candidate->objectName()==QStringLiteral("jobVulkanWindow")){renderWindow=candidate;break;}
        QVERIFY(renderWindow);
        const qreal pixelRatio=std::max<qreal>(1.0,frame.devicePixelRatio());
        const QPoint clickPosition(qRound(cyanPixel.x()/pixelRatio),qRound(cyanPixel.y()/pixelRatio));
        QSignalSpy pointSelection(jobView,&JobPreviewWidget::placeSelected);
        QTest::mouseClick(renderWindow,Qt::LeftButton,Qt::NoModifier,clickPosition);
        QTRY_VERIFY_WITH_TIMEOUT(!pointSelection.isEmpty(),1000);
        QVERIFY(pointSelection.constFirst().constFirst().toLongLong()>=0);
        tabs->setCurrentIndex(0);QTRY_VERIFY(imageView->isVisible());
        QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellSelected",Qt::DirectConnection,
                                          Q_ARG(int,0),Q_ARG(int,1)));
        QTRY_COMPARE(jobView->selectedPlaceId(),qint64(1));
        QCOMPARE(tabs->currentIndex(),0);
        QVERIFY(QMetaObject::invokeMethod(jobView,"placeSelected",Qt::DirectConnection,Q_ARG(qint64,2)));
        QCOMPARE(imageView->selectedCell(),QPoint(1,1));
        QCOMPARE(tabs->currentIndex(),0);
        autoJump->setChecked(true);tabs->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(jobView,"placeSelected",Qt::DirectConnection,Q_ARG(qint64,0)));
        QCOMPARE(tabs->currentIndex(),0);
        layoutMode->setCurrentIndex(1);QTRY_VERIFY(imageView->isVisible());QTRY_VERIFY(jobView->isVisible());
    }else{
        QVERIFY(!importButton->isEnabled());
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("jobVulkanError")));
    }
}

void TestMainWindow::denseJobPointCanBeSelectedAfterOrbit() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(160,160,QImage::Format_Grayscale8);source.fill(30);
    const QString path=directory.filePath("dense.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,301,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;
    auto* tabs=window.findChild<QTabBar*>(QStringLiteral("previewTabs"));
    auto* jobView=window.findChild<JobPreviewWidget*>(QStringLiteral("jobPreviewWidget"));
    auto* previewButton=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* cellWidth=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* cellHeight=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    QVERIFY(tabs);QVERIFY(jobView);QVERIFY(previewButton);QVERIFY(cellWidth);QVERIFY(cellHeight);
    if(!jobView->isVulkanAvailable())QSKIP("Vulkan preview unavailable");
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));
    cellWidth->setValue(2);cellHeight->setValue(2);
    window.show();QTest::qWait(30);QTest::mouseClick(previewButton,Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(),1,5000);
    QTRY_VERIFY_WITH_TIMEOUT(jobView->document()!=nullptr,5000);
    QCOMPARE(jobView->document()->points.size(),size_t(6400));
    QWindow* renderWindow=nullptr;
    for(QWindow* candidate:QGuiApplication::allWindows())if(candidate->objectName()==QStringLiteral("jobVulkanWindow")){renderWindow=candidate;break;}
    QVERIFY(renderWindow);
    const QPoint center(renderWindow->width()/2,renderWindow->height()/2);
    QTest::mousePress(renderWindow,Qt::LeftButton,Qt::NoModifier,center);
    QTest::mouseMove(renderWindow,center+QPoint(60,35),30);
    QTest::mouseRelease(renderWindow,Qt::LeftButton,Qt::NoModifier,center+QPoint(60,35));
    QTest::qWait(50);
    const QImage frame=jobView->grabFrame();QVERIFY(!frame.isNull());
    QPoint cyanPixel(-1,-1);for(int y=0;y<frame.height()&&cyanPixel.x()<0;++y)for(int x=0;x<frame.width();++x){const QColor color=frame.pixelColor(x,y);if(color.blue()>color.red()+80&&color.green()>100){cyanPixel={x,y};break;}}
    QVERIFY(cyanPixel.x()>=0);
    const qreal pixelRatio=std::max<qreal>(1.0,frame.devicePixelRatio());
    QSignalSpy pointSelection(jobView,&JobPreviewWidget::placeSelected);
    QTest::mouseClick(renderWindow,Qt::LeftButton,Qt::NoModifier,
                      {qRound(cyanPixel.x()/pixelRatio),qRound(cyanPixel.y()/pixelRatio)});
    QTRY_VERIFY_WITH_TIMEOUT(!pointSelection.isEmpty(),1000);
    QVERIFY2(pointSelection.constFirst().constFirst().toLongLong()>=0,"Dense Vulkan point pick missed");
    QTRY_VERIFY_WITH_TIMEOUT(jobView->selectedPlaceId()>=0,1000);
    QTest::qWait(50);
    const QImage selectedFrame=jobView->grabFrame();QVERIFY(!selectedFrame.isNull());
    bool hasYellow=false;for(int y=0;y<selectedFrame.height()&&!hasYellow;++y)for(int x=0;x<selectedFrame.width();++x){const QColor color=selectedFrame.pixelColor(x,y);if(color.red()>180&&color.green()>140&&color.blue()<100){hasYellow=true;break;}}
    QVERIFY2(hasYellow,"Selected Vulkan point was not highlighted yellow");
}

QTEST_MAIN(TestMainWindow)
#include "TestMainWindow.moc"
