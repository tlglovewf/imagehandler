#include "imaging/VipsSupport.h"
#include "imaging/ImageLoader.h"
#include "ui/LargeImageView.h"
#include "ui/JobPreviewWidget.h"
#include "ui/PointCloudWidget.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTimer>
#include <QWindow>
#include <QtTest>
#include <algorithm>

class TestMainWindow final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanupTestCase();
    void listModesAndMergedExportControl();
    void printCanvasFollowsDocument();
    void jobPreviewWorkspaceAndLinking();
    void denseJobPointCanBeSelectedAfterOrbit();
    void settingsPersistenceAndReset();
    void multipleImageDropIsSortedAndSelected();
    void preflightDialogRequiresExplicitOverwrite();
    void segmentationStatisticsUpdates();
    void pointCloudControlsAndTab();
    void fixedPointClearAndPhysicalSize();
    void mergeProgressStartsImmediately();

private:
    QTemporaryDir settingsDirectory_;
};

void TestMainWindow::initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("ImageHandlerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MainWindowTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
    QString error;
    QVERIFY2(VipsSupport::initialize("MainWindowTests", &error), qPrintable(error));
}

void TestMainWindow::init() { QSettings().clear(); }

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
    QCOMPARE(layoutMode->currentIndex(),0);QVERIFY(!autoJump->isChecked());QCOMPARE(tabs->count(),3);
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

void TestMainWindow::pointCloudControlsAndTab() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(4,4,QImage::Format_Grayscale8);for(int y=0;y<4;++y)for(int x=0;x<4;++x)source.setPixel(x,y,uchar((x+y)*32));
    const QString path=directory.filePath("cloud.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,801,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;auto* scale=window.findChild<QDoubleSpinBox*>(QStringLiteral("cloudPixelScale"));auto* height=window.findChild<QDoubleSpinBox*>(QStringLiteral("cloudMaximumHeight"));auto* invert=window.findChild<QCheckBox*>(QStringLiteral("cloudInvertHeight"));auto* startColor=window.findChild<QPushButton*>(QStringLiteral("cloudStartColorButton"));auto* endColor=window.findChild<QPushButton*>(QStringLiteral("cloudEndColorButton"));auto* generate=window.findChild<QPushButton*>(QStringLiteral("generateCloudButton"));auto* tabs=window.findChild<QTabBar*>(QStringLiteral("previewTabs"));auto* layoutMode=window.findChild<QComboBox*>(QStringLiteral("previewLayoutMode"));auto* cloud=window.findChild<PointCloudWidget*>(QStringLiteral("pointCloudWidget"));auto* job=window.findChild<JobPreviewWidget*>(QStringLiteral("jobPreviewWidget"));auto* imageView=window.findChild<LargeImageView*>();auto* probe=window.findChild<QLabel*>(QStringLiteral("pixelProbeLabel"));auto* resetView=window.findChild<QPushButton*>(QStringLiteral("pointCloudFitAllButton"));QVERIFY(scale);QVERIFY(height);QVERIFY(invert);QVERIFY(startColor);QVERIFY(endColor);QVERIFY(generate);QVERIFY(tabs);QVERIFY(layoutMode);QVERIFY(cloud);QVERIFY(job);QVERIFY(imageView);QVERIFY(probe);QVERIFY(resetView);QVERIFY(window.findChild<QObject*>(QStringLiteral("pointCloudResetShortcut")));QCOMPARE(resetView->text(),QStringLiteral("视角复位 (Space)"));QCOMPARE(scale->value(),0.25);QCOMPARE(height->value(),12.5);QVERIFY(invert->isChecked());QCOMPARE(startColor->property("cloudColor").value<QColor>(),QColor(0,0,255));QCOMPARE(endColor->property("cloudColor").value<QColor>(),QColor(0,255,0));QVERIFY(!generate->isEnabled());QCOMPARE(tabs->count(),3);QCOMPARE(tabs->tabText(2),QStringLiteral("PointCloud"));QCOMPARE(probe->text(),QStringLiteral("X: --  Y: --  灰度: --"));
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));
    if(!cloud->isVulkanAvailable()){QVERIFY(!generate->isEnabled());return;}
    QVERIFY(generate->isEnabled());const QColor beforeCancel=startColor->property("cloudColor").value<QColor>();QTimer::singleShot(0,&window,[&window]{if(auto* dialog=window.findChild<QColorDialog*>(QStringLiteral("cloudColorDialog")))dialog->reject();});QTest::mouseClick(startColor,Qt::LeftButton);QCOMPARE(startColor->property("cloudColor").value<QColor>(),beforeCancel);scale->setValue(0.5);height->setValue(20.0);window.show();QTest::mouseClick(generate,Qt::LeftButton);QCOMPARE(tabs->currentIndex(),2);QTRY_VERIFY_WITH_TIMEOUT(cloud->document()!=nullptr,5000);QCOMPARE(cloud->document()->settings.pixelScale,0.5);QCOMPARE(cloud->document()->settings.maximumHeight,20.0);QVERIFY(cloud->document()->settings.invertHeight);QCOMPARE(cloud->document()->settings.startColor,QColor(0,0,255));QCOMPARE(cloud->document()->settings.endColor,QColor(0,255,0));QCOMPARE(cloud->document()->rootTiles.front()->vertices.size(),size_t(16));invert->setChecked(false);QVERIFY(cloud->document()->settings.invertHeight);QImage frame;QTRY_VERIFY_WITH_TIMEOUT(!(frame=cloud->grabFrame()).isNull(),3000);bool hasBlue=false,hasGreen=false;for(int y=0;y<frame.height();++y)for(int x=0;x<frame.width();++x){const QColor color=frame.pixelColor(x,y);if(color.blue()>color.red()+30&&color.blue()>60)hasBlue=true;if(color.green()>color.red()+30&&color.green()>60)hasGreen=true;}QVERIFY(hasBlue);QVERIFY(hasGreen);layoutMode->setCurrentIndex(1);QTRY_VERIFY(cloud->isVisible());QVERIFY(!imageView->isVisible());QVERIFY(!job->isVisible());
}

void TestMainWindow::fixedPointClearAndPhysicalSize() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(1,1,QImage::Format_Grayscale8);source.fill(128);
    const QString path=directory.filePath("fixed-ui.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,810,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;auto* scale=window.findChild<QDoubleSpinBox*>(QStringLiteral("cloudPixelScale"));auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));auto* physical=window.findChild<QLabel*>(QStringLiteral("elementPhysicalSize"));auto* clear=window.findChild<QPushButton*>(QStringLiteral("clearFixedGrayPointsButton"));auto* view=window.findChild<LargeImageView*>();
    QVERIFY(scale);QVERIFY(width);QVERIFY(height);QVERIFY(physical);QVERIFY(clear);QVERIFY(view);QCOMPARE(physical->text(),QStringLiteral("55 : 55"));QVERIFY(!clear->isEnabled());
    width->setValue(3);height->setValue(5);scale->setValue(0.125);QCOMPARE(physical->text(),QStringLiteral("0.375 : 0.625"));
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));window.show();QTRY_VERIFY(view->isVisible());view->fitToWindow();QCoreApplication::processEvents();
    QTest::mouseClick(view,Qt::LeftButton,Qt::ControlModifier,view->rect().center());
    QTRY_COMPARE_WITH_TIMEOUT(view->fixedGrayPoints().size(),1,5000);QTRY_VERIFY(clear->isEnabled());
    QTest::mouseClick(clear,Qt::LeftButton);QCOMPARE(view->fixedGrayPoints().size(),0);QVERIFY(!clear->isEnabled());
}

void TestMainWindow::mergeProgressStartsImmediately() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage image(8,8,QImage::Format_Grayscale8);image.fill(42);
    const QString first=directory.filePath("tile_1_1.bmp");const QString second=directory.filePath("tile_1_2.bmp");
    QVERIFY(image.save(first,"BMP"));QVERIFY(image.save(second,"BMP"));
    MainWindow window;window.show();QMimeData mime;mime.setUrls({QUrl::fromLocalFile(first),QUrl::fromLocalFile(second)});QDragEnterEvent drag(QPoint(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&window,&drag);QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&window,&drop);
    auto* merge=window.findChild<QPushButton*>(QStringLiteral("mergeImageButton"));auto* progress=window.findChild<QProgressBar*>(QStringLiteral("taskProgress"));auto* percent=window.findChild<QLabel*>(QStringLiteral("taskProgressPercent"));
    QVERIFY(merge);QVERIFY(progress);QVERIFY(percent);QTRY_VERIFY_WITH_TIMEOUT(merge->isEnabled(),5000);
    merge->click();QVERIFY(progress->isVisible());QVERIFY(percent->isVisible());QCOMPARE(progress->minimum(),0);QCOMPARE(progress->maximum(),2);QCOMPARE(progress->value(),0);QCOMPARE(percent->text(),QStringLiteral("0%"));
    QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(),5000);
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

void TestMainWindow::settingsPersistenceAndReset() {
    {
        MainWindow window;
        auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
        auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
        auto* threshold=window.findChild<QSpinBox*>(QStringLiteral("segmentationThreshold"));
        auto* traversal=window.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"));
        auto* printMode=window.findChild<QCheckBox*>(QStringLiteral("chk_o_print"));
        auto* reverseY=window.findChild<QCheckBox*>(QStringLiteral("chk_y_reverse"));
        auto* repeat=window.findChild<QSpinBox*>(QStringLiteral("repeat_times"));
        auto* cloudInvert=window.findChild<QCheckBox*>(QStringLiteral("cloudInvertHeight"));
        QVERIFY(width);QVERIFY(height);QVERIFY(threshold);QVERIFY(traversal);QVERIFY(printMode);QVERIFY(reverseY);QVERIFY(repeat);QVERIFY(cloudInvert);
        width->setValue(321);height->setValue(123);threshold->setValue(77);traversal->setCurrentIndex(1);
        printMode->setChecked(true);reverseY->setChecked(true);repeat->setValue(4);cloudInvert->setChecked(false);
        window.show();QTest::qWait(10);window.close();
    }
    MainWindow restored;
    auto* width=restored.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* height=restored.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* threshold=restored.findChild<QSpinBox*>(QStringLiteral("segmentationThreshold"));
    auto* traversal=restored.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"));
    auto* printMode=restored.findChild<QCheckBox*>(QStringLiteral("chk_o_print"));
    auto* reverseY=restored.findChild<QCheckBox*>(QStringLiteral("chk_y_reverse"));
    auto* repeat=restored.findChild<QSpinBox*>(QStringLiteral("repeat_times"));
    auto* reset=restored.findChild<QPushButton*>(QStringLiteral("resetParametersButton"));
    auto* canvasX=restored.findChild<QSpinBox*>(QStringLiteral("canvasX"));
    auto* cloudInvert=restored.findChild<QCheckBox*>(QStringLiteral("cloudInvertHeight"));auto* cloudStart=restored.findChild<QPushButton*>(QStringLiteral("cloudStartColorButton"));auto* cloudEnd=restored.findChild<QPushButton*>(QStringLiteral("cloudEndColorButton"));
    QVERIFY(width);QVERIFY(height);QVERIFY(threshold);QVERIFY(traversal);QVERIFY(printMode);QVERIFY(reverseY);QVERIFY(repeat);QVERIFY(reset);QVERIFY(canvasX);QVERIFY(cloudInvert);QVERIFY(cloudStart);QVERIFY(cloudEnd);
    QCOMPARE(width->value(),321);QCOMPARE(height->value(),123);QCOMPARE(threshold->value(),77);
    QCOMPARE(traversal->currentIndex(),1);QVERIFY(printMode->isChecked());QVERIFY(reverseY->isChecked());
    QCOMPARE(repeat->value(),4);QCOMPARE(canvasX->value(),1);QVERIFY(!cloudInvert->isChecked());QCOMPARE(cloudStart->property("cloudColor").value<QColor>(),QColor(0,0,255));QCOMPARE(cloudEnd->property("cloudColor").value<QColor>(),QColor(0,255,0));
    const auto restoredLabels=restored.findChildren<QLabel*>();
    QVERIFY(std::any_of(restoredLabels.cbegin(),restoredLabels.cend(),
        [](const QLabel* label){return label->text()==QStringLiteral("未加载图像");}));
    QTest::mouseClick(reset,Qt::LeftButton);
    QCOMPARE(width->value(),220);QCOMPARE(height->value(),220);QCOMPARE(threshold->value(),128);
    QCOMPARE(traversal->currentIndex(),0);QVERIFY(!printMode->isChecked());QVERIFY(!reverseY->isChecked());
    QCOMPARE(repeat->value(),10);

    QSettings settings;settings.setValue(QStringLiteral("segmentation/cellWidth"),-5);
    settings.setValue(QStringLiteral("segmentation/threshold"),999);
    settings.setValue(QStringLiteral("segmentation/traversal"),99);settings.setValue(QStringLiteral("pointCloud/startColor"),QStringLiteral("invalid"));settings.setValue(QStringLiteral("pointCloud/endColor"),QStringLiteral("#not-a-color"));settings.sync();
    MainWindow invalid;
    QCOMPARE(invalid.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"))->value(),220);
    QCOMPARE(invalid.findChild<QSpinBox*>(QStringLiteral("segmentationThreshold"))->value(),128);
    QCOMPARE(invalid.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"))->currentIndex(),0);
    QCOMPARE(invalid.findChild<QPushButton*>(QStringLiteral("cloudStartColorButton"))->property("cloudColor").value<QColor>(),QColor(0,0,255));QCOMPARE(invalid.findChild<QPushButton*>(QStringLiteral("cloudEndColorButton"))->property("cloudColor").value<QColor>(),QColor(0,255,0));
}

void TestMainWindow::multipleImageDropIsSortedAndSelected() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage image(3,2,QImage::Format_Grayscale8);image.fill(42);
    const QString ten=directory.filePath("10.bmp");const QString two=directory.filePath("2.bmp");
    QVERIFY(image.save(ten,"BMP"));QVERIFY(image.save(two,"BMP"));
    MainWindow window;window.show();QTest::qWait(10);
    QMimeData mime;mime.setUrls({QUrl::fromLocalFile(ten),QUrl::fromLocalFile(two),QUrl::fromLocalFile(two)});
    QDragEnterEvent drag(QPoint(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&window,&drag);QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&window,&drop);
    auto* list=window.findChild<QListWidget*>(QStringLiteral("directoryImageList"));
    QVERIFY(list);QCOMPARE(list->count(),2);QCOMPARE(list->item(0)->text(),QStringLiteral("2.bmp"));
    QCOMPARE(list->item(1)->text(),QStringLiteral("10.bmp"));QCOMPARE(list->selectedItems().size(),2);
    QTRY_VERIFY_WITH_TIMEOUT(([&window]{const auto labels=window.findChildren<QLabel*>();return std::any_of(
        labels.cbegin(),labels.cend(),[](const QLabel* label){return label->text().contains(QStringLiteral("3 × 2"));});})(),5000);
}

void TestMainWindow::preflightDialogRequiresExplicitOverwrite() {
    MainWindow window;
    ExportPreflightResult result;result.valid=true;result.bmpFileCount=4;
    result.estimatedBmpBytes=4096;result.requiredBytes=16*1024*1024;
    result.spaceKnown=true;result.availableBytes=100*1024*1024;
    result.conflictCount=1;result.conflictSamples={QStringLiteral("1.bmp")};
    result.firstOutput=QStringLiteral("1.bmp");result.lastOutput=QStringLiteral("4.bmp");
    result.targetFingerprint=QStringLiteral("fingerprint");
    bool dialogSeen=false;bool initiallyDisabled=false;bool enabledAfterCheck=false;
    QTimer::singleShot(0,&window,[&]{
        auto* box=window.findChild<QMessageBox*>(QStringLiteral("exportPreflightDialog"));
        if(!box)return;dialogSeen=true;
        auto* accept=box->findChild<QPushButton*>(QStringLiteral("exportPreflightAcceptButton"));
        auto* check=box->findChild<QCheckBox*>(QStringLiteral("exportOverwriteCheck"));
        initiallyDisabled=accept&&!accept->isEnabled();
        if(check)check->setChecked(true);
        enabledAfterCheck=accept&&accept->isEnabled();
        for(QAbstractButton* button:box->buttons())if(box->buttonRole(button)==QMessageBox::RejectRole){button->click();break;}
    });
    QVERIFY(QMetaObject::invokeMethod(&window,"showExportPreflight",Qt::DirectConnection,
                                      Q_ARG(ExportPreflightResult,result)));
    QVERIFY(dialogSeen);QVERIFY(initiallyDisabled);QVERIFY(enabledAfterCheck);
}

void TestMainWindow::segmentationStatisticsUpdates() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(5,3,QImage::Format_Grayscale8);source.fill(42);
    const QString path=directory.filePath("statistics.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,401,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;
    auto* status=window.findChild<QLabel*>(QStringLiteral("segmentationStatisticsStatus"));
    auto* mode=window.findChild<QLabel*>(QStringLiteral("segmentationStatisticsMode"));
    auto* grid=window.findChild<QLabel*>(QStringLiteral("segmentationStatisticsGrid"));
    auto* files=window.findChild<QLabel*>(QStringLiteral("segmentationStatisticsFileCount"));
    auto* output=window.findChild<QLabel*>(QStringLiteral("segmentationStatisticsOutputSize"));
    auto* required=window.findChild<QLabel*>(QStringLiteral("segmentationStatisticsRequiredSpace"));
    auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* preview=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* printMode=window.findChild<QCheckBox*>(QStringLiteral("chk_o_print"));
    auto* repeat=window.findChild<QSpinBox*>(QStringLiteral("repeat_times"));
    QVERIFY(status);QVERIFY(mode);QVERIFY(grid);QVERIFY(files);QVERIFY(output);QVERIFY(required);
    QVERIFY(width);QVERIFY(height);QVERIFY(preview);QVERIFY(printMode);QVERIFY(repeat);
    QCOMPARE(status->text(),QStringLiteral("请先完成分割计算。"));
    QCOMPARE(output->text(),QStringLiteral("--"));

    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,
                                      Q_ARG(ImageDocumentPtr,document)));
    QCOMPARE(status->text(),QStringLiteral("请先完成分割计算。"));
    width->setValue(4);height->setValue(2);preview->click();
    QCOMPARE(status->text(),QStringLiteral("已按当前导出选项计算。"));
    QCOMPARE(mode->text(),QStringLiteral("普通分割"));QCOMPARE(grid->text(),QStringLiteral("2 × 2"));
    QCOMPARE(files->text(),QStringLiteral("4 个"));QCOMPARE(output->text(),QStringLiteral("12.23 KB"));
    QCOMPARE(required->text(),QStringLiteral("16.01 MB"));

    width->setValue(3);
    QVERIFY(status->text().contains(QStringLiteral("请重新应用参数")));
    QCOMPARE(output->text(),QStringLiteral("--"));
    width->setValue(4);printMode->setChecked(true);repeat->setValue(3);
    QCOMPARE(mode->text(),QStringLiteral("打印包"));QCOMPARE(files->text(),QStringLiteral("12 个"));
    QCOMPARE(output->text(),QStringLiteral("21.56 KB"));QCOMPARE(required->text(),QStringLiteral("16.02 MB"));

    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,
                                      Q_ARG(ImageDocumentPtr,document)));
    QCOMPARE(status->text(),QStringLiteral("请先完成分割计算。"));
    QCOMPARE(files->text(),QStringLiteral("--"));QCOMPARE(output->text(),QStringLiteral("--"));
}

QTEST_MAIN(TestMainWindow)
#include "TestMainWindow.moc"
