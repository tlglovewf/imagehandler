#include "imaging/VipsSupport.h"
#include "imaging/ImageLoader.h"
#include "imaging/PatternDetector.h"
#include "imaging/MaskStackMeshBuilder.h"
#include "imaging/JobSpatialIndex.h"
#include "imaging/JobStackLod.h"
#include "io/SplitExporter.h"
#include "io/LayeredPackageExporter.h"
#include "ui/LargeImageView.h"
#include "ui/JobPreviewWidget.h"
#include "ui/PointCloudWidget.h"
#include "ui/ElementPointCloudWindow.h"
#include "ui/MaskSliceWindow.h"
#include "ui/LayeredExportDialog.h"
#include "ui/MaskStackWidget.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidget>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTimer>
#include <QWindow>
#include <QtTest>
#include <algorithm>

#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
#include <QVulkanWindow>
#endif

class TestMainWindow final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanupTestCase();
    void listModesAndMergedExportControl();
    void printCanvasFollowsDocument();
    void jobPreviewWorkspaceAndLinking();
    void elementGroupingButtonUpdatesJobPreview();
    void denseJobPointCanBeSelectedAfterOrbit();
    void settingsPersistenceAndReset();
    void multipleImageDropIsSortedAndSelected();
    void preflightDialogRequiresExplicitOverwrite();
    void segmentationStatisticsUpdates();
    void layeredExportMultiplierRoundsDecimalProducts();
    void layeredExportDialogWritesJobCommands();
    void pointCloudControlsAndTab();
    void fixedPointClearAndPhysicalSize();
    void mergeProgressStartsImmediately();
    void listActivationRequiresDoubleClick();
    void elementPointCloudWindowIsReused();
    void maskSliceWindowDefaultsAndReuse();
    void maskSliceMultiSelectionAndStackPlacement();
    void layeredJobStackModesRender();
    void largeLayeredJobFrameCompletes();

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
    QVERIFY(directoryList); QVERIFY(mergedList); QVERIFY(exportButton);QVERIFY(previewButton);
    QVERIFY(taskTitle);QVERIFY(progress);QVERIFY(percent);QVERIFY(gridCount);
    QVERIFY(cellWidth);QVERIFY(cellHeight);QVERIFY(traversal);QVERIFY(pathButton);QVERIFY(splitExportButton);QVERIFY(selectedCell);
    QVERIFY(!window.findChild<QObject*>(QStringLiteral("chk_o_print")));
    QVERIFY(!window.findChild<QObject*>(QStringLiteral("printOptionsContainer")));
    QVERIFY(!window.findChild<QObject*>(QStringLiteral("canvasX")));
    QVERIFY(!window.findChild<QObject*>(QStringLiteral("canvasY")));
    QVERIFY(!window.findChild<QObject*>(QStringLiteral("lock")));
    QVERIFY(!window.findChild<QObject*>(QStringLiteral("repeat_times")));
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
    QVERIFY(qobject_cast<LargeImageView*>(selectedCell->parentWidget()));
    QVERIFY(selectedCell->testAttribute(Qt::WA_TransparentForMouseEvents));
    QCOMPARE(splitExportButton->parentWidget(),previewButton->parentWidget());
    window.show();QTest::qWait(20);
    QVERIFY(splitExportButton->geometry().top()>=previewButton->geometry().bottom());
    const auto* imageView=qobject_cast<LargeImageView*>(selectedCell->parentWidget());
    QVERIFY(std::abs(selectedCell->geometry().center().x()-imageView->rect().center().x())<=2);
    QVERIFY(selectedCell->geometry().bottom()<imageView->rect().bottom());
}

void TestMainWindow::printCanvasFollowsDocument() {
    SegmentationResult segmentation;
    segmentation.settings.cellWidth=4;segmentation.settings.cellHeight=3;
    segmentation.rows=2;segmentation.columns=2;
    QCOMPARE(SplitExporter::physicalPosition(segmentation,QPoint(1,1),{0.25,false}),
             QPointF(1.0,0.75));
    QCOMPARE(SplitExporter::physicalPosition(segmentation,QPoint(1,1),{0.25,true}),
             QPointF(1.0,-0.75));
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
    auto* coordinate=window.findChild<QLabel*>(QStringLiteral("jobSelectedCoordinate"));
    auto* previewButton=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* cellWidth=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* cellHeight=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* imageView=window.findChild<LargeImageView*>();
    QVERIFY(importButton);QVERIFY(layoutMode);QVERIFY(autoJump);QVERIFY(tabs);QVERIFY(jobView);
    QVERIFY(status);QVERIFY(coordinate);QVERIFY(previewButton);QVERIFY(cellWidth);QVERIFY(cellHeight);QVERIFY(imageView);
    QVERIFY(!coordinate->isVisible());
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
        QTRY_VERIFY(!coordinate->isHidden());
        QVERIFY(coordinate->text().contains(QStringLiteral("color:#ff5252")));
        QVERIFY(coordinate->text().contains(QStringLiteral("color:#4caf50")));
        const auto selectedPoint=std::find_if(jobView->document()->points.cbegin(),jobView->document()->points.cend(),
            [](const JobPoint& point){return point.placementId==1;});
        QVERIFY(selectedPoint!=jobView->document()->points.cend());
        QVERIFY(coordinate->text().contains(QString::number(selectedPoint->position.x,'g',15)));
        QVERIFY(coordinate->text().contains(QString::number(selectedPoint->position.y,'g',15)));
        QCOMPARE(tabs->currentIndex(),0);
        QVERIFY(QMetaObject::invokeMethod(jobView,"placeSelected",Qt::DirectConnection,Q_ARG(qint64,2)));
        QCOMPARE(imageView->selectedCell(),QPoint(1,1));
        QCOMPARE(tabs->currentIndex(),0);
        autoJump->setChecked(true);tabs->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(jobView,"placeSelected",Qt::DirectConnection,Q_ARG(qint64,0)));
        QCOMPARE(tabs->currentIndex(),0);
        layoutMode->setCurrentIndex(1);QTRY_VERIFY(imageView->isVisible());QTRY_VERIFY(jobView->isVisible());
        QVERIFY(QMetaObject::invokeMethod(jobView,"placeActivated",Qt::DirectConnection,
                                          Q_ARG(qint64,0)));
        auto* jobStack=window.findChild<MaskSliceWindow*>(QStringLiteral("jobStackWindow"));
        QVERIFY(jobStack);QVERIFY(jobStack->isVisible());
        auto* jobStackStatus=jobStack->findChild<QLabel*>(QStringLiteral("maskStackSamplingInfo"));
        QVERIFY(jobStackStatus);
        QTRY_VERIFY_WITH_TIMEOUT(jobStackStatus->text().contains(QStringLiteral("临时预览")),3000);
    }else{
        QVERIFY(!importButton->isEnabled());
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("jobVulkanError")));
    }
}

void TestMainWindow::layeredJobStackModesRender() {
    JobPreviewWidget widget;
    auto* status = widget.findChild<QLabel*>(QStringLiteral("jobPreviewStatus"));
    QVERIFY(status);
    QVERIFY(!widget.findChild<QComboBox*>(QStringLiteral("jobStackDisplayMode")));
    if (!widget.isVulkanAvailable()) QSKIP("Vulkan preview unavailable");

    MaskStackPreviewData preview;
    preview.gridSize = {2,2}; preview.requestedOutputSize = {2,2};
    preview.physicalSize = {2.0,2.0}; preview.columnLayers = {1,2,2,1};
    preview.layerHeight = 0.5; preview.layerCount = 2;
    const auto token = std::make_shared<CancellationToken>();
    preview.solidGeometry = MaskStackMeshBuilder::buildSolid(preview, token, {});
    QVERIFY(preview.solidGeometry);

    auto stack = std::make_shared<JobModelStack>();
    stack->sourceSize = preview.requestedOutputSize; stack->gridSize = preview.gridSize;
    stack->physicalSize = preview.physicalSize; stack->layerHeight = preview.layerHeight;
    stack->layerCount = preview.layerCount; stack->solidGeometry = preview.solidGeometry;
    stack->lodLevels = JobStackLod::buildPyramid(preview.gridSize,preview.columnLayers,token);
    QVERIFY(!stack->lodLevels.empty());
    auto document = std::make_shared<JobDocument>();
    document->layeredPackageDetected = true;
    document->models.push_back({0,QStringLiteral("0"),QStringLiteral("0"),
                                JobModelStackState::Loaded,stack,{}});
    document->points.push_back({0,{0,0,0},1,0,{}});
    document->bounds.include({0,0,0});
    document->sceneBounds.include({0,0,0}); document->sceneBounds.include({2,2,1});

    widget.resize(640,480); widget.setDocument(document); widget.show();
    QVERIFY(status->text().contains(QStringLiteral("1 个点")));
    QTest::qWait(100);
    QVERIFY(!widget.grabFrame().isNull());
}

void TestMainWindow::largeLayeredJobFrameCompletes() {
    JobPreviewWidget widget;
    if (!widget.isVulkanAvailable()) QSKIP("Vulkan preview unavailable");

    MaskStackPreviewData preview;
    preview.gridSize={2,2};preview.requestedOutputSize={2,2};
    preview.physicalSize={0.002,0.002};preview.columnLayers={1,2,2,1};
    preview.layerHeight=0.001;preview.layerCount=2;
    const auto token=std::make_shared<CancellationToken>();
    preview.solidGeometry=MaskStackMeshBuilder::buildSolid(preview,token,{});
    auto stack=std::make_shared<JobModelStack>();
    stack->sourceSize=preview.requestedOutputSize;stack->gridSize=preview.gridSize;
    stack->physicalSize=preview.physicalSize;stack->layerHeight=preview.layerHeight;
    stack->layerCount=preview.layerCount;stack->solidGeometry=preview.solidGeometry;
    stack->lodLevels=JobStackLod::buildPyramid(preview.gridSize,preview.columnLayers,token);

    constexpr int side=450;
    auto document=std::make_shared<JobDocument>();
    document->layeredPackageDetected=true;
    document->models.push_back({0,QStringLiteral("0"),QStringLiteral("0"),
                                JobModelStackState::Loaded,stack,{}});
    document->points.reserve(side*side);
    for(int y=0;y<side;++y)for(int x=0;x<side;++x) {
        const qint64 id=static_cast<qint64>(y)*side+x;
        const JobCoordinate position{x*0.002,y*0.002,0.0};
        document->points.push_back({0,position,id,id,{}});
        document->bounds.include(position);
    }
    document->sceneBounds=document->bounds;
    document->sceneBounds.include({0.9,0.9,0.002});
    document->placementSpatialIndex=JobPlacementSpatialIndex::build(*document,token);
    QVERIFY(document->placementSpatialIndex);

    widget.resize(640,480);widget.setDocument(document);widget.show();
    auto* status=widget.findChild<QLabel*>(QStringLiteral("jobPreviewStatus"));
    QVERIFY(status);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("个点")),5000);
    QImage frame;
    QTRY_VERIFY_WITH_TIMEOUT(!(frame=widget.grabFrame()).isNull(),5000);
}

void TestMainWindow::elementGroupingButtonUpdatesJobPreview() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QImage source(4, 2, QImage::Format_Grayscale8);
    for (int y = 0; y < source.height(); ++y) {
        uchar* row = source.scanLine(y);
        row[0] = 0; row[1] = 255; row[2] = 64; row[3] = 128;
    }
    const QString path = directory.filePath(QStringLiteral("group-ui.bmp"));
    QVERIFY(source.save(path, "BMP"));
    QString error;
    const auto document = ImageLoader::load(path, 251, &error);
    QVERIFY2(document, qPrintable(error));
    MainWindow window;
    auto* width = window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* height = window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* preview = window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* grouping = window.findChild<QPushButton*>(QStringLiteral("groupElementsButton"));
    auto* jobView = window.findChild<JobPreviewWidget*>(QStringLiteral("jobPreviewWidget"));
    QVERIFY(width); QVERIFY(height); QVERIFY(preview); QVERIFY(grouping); QVERIFY(jobView);
    QVERIFY(!grouping->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "acceptDocument", Qt::DirectConnection,
                                      Q_ARG(ImageDocumentPtr, document)));
    width->setValue(2); height->setValue(1);
    QTest::mouseClick(preview, Qt::LeftButton);
    QVERIFY(grouping->isEnabled());
    QTest::mouseClick(grouping, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(grouping->text().contains(QStringLiteral("2/4")), 5000);
    if (jobView->isVulkanAvailable()) {
        QTRY_VERIFY_WITH_TIMEOUT(jobView->document() != nullptr, 5000);
        QCOMPARE(jobView->document()->models.size(), size_t(2));
        QCOMPARE(jobView->document()->points.size(), size_t(4));
        QCOMPARE(jobView->document()->points[0].placeId, qint64(0));
        QCOMPARE(jobView->document()->points[2].placeId, qint64(1));
        QVERIFY(jobView->document()->points[0].groupColor.isValid());
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
    merge->click();QVERIFY(progress->isVisible());QVERIFY(percent->isVisible());QCOMPARE(progress->minimum(),0);QCOMPARE(progress->maximum(),1000);QCOMPARE(progress->value(),0);QCOMPARE(percent->text(),QStringLiteral("0%"));
    QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(),5000);
}

void TestMainWindow::listActivationRequiresDoubleClick() {
    QTemporaryDir directory;QVERIFY(directory.isValid());QStringList paths;
    const QSize sizes[]={{3,2},{4,2},{3,2},{4,2}};
    for(int row=0;row<2;++row)for(int column=0;column<2;++column){QImage image(sizes[row*2+column],QImage::Format_Grayscale8);image.fill(30+row*20+column);const QString path=directory.filePath(QString("tile_%1_%2.bmp").arg(row).arg(column));QVERIFY(image.save(path,"BMP"));paths<<path;}
    MainWindow window;window.show();QMimeData mime;for(const auto& path:paths)mime.setUrls(mime.urls()+QList<QUrl>{QUrl::fromLocalFile(path)});
    QDragEnterEvent drag(QPoint(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&window,&drag);QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&window,&drop);
    auto* files=window.findChild<QListWidget*>(QStringLiteral("directoryImageList"));auto* merged=window.findChild<QListWidget*>(QStringLiteral("mergedImageList"));auto* info=window.findChild<QLabel*>(QStringLiteral("imageInfo"));auto* merge=window.findChild<QPushButton*>(QStringLiteral("mergeImageButton"));
    QVERIFY(files);QVERIFY(merged);QVERIFY(info);QVERIFY(merge);QTRY_VERIFY_WITH_TIMEOUT(info->text().contains(QStringLiteral("3 × 2")),5000);
    QTest::mouseClick(files->viewport(),Qt::LeftButton,Qt::NoModifier,files->visualItemRect(files->item(1)).center());
    QTest::qWait(100);QVERIFY(info->text().contains(QStringLiteral("3 × 2")));
    QTest::mouseDClick(files->viewport(),Qt::LeftButton,Qt::NoModifier,files->visualItemRect(files->item(1)).center());
    QTRY_VERIFY_WITH_TIMEOUT(info->text().contains(QStringLiteral("4 × 2")),5000);

    files->selectAll();QTRY_VERIFY_WITH_TIMEOUT(merge->isEnabled(),5000);merge->click();QTRY_COMPARE_WITH_TIMEOUT(merged->count(),1,5000);QTRY_VERIFY_WITH_TIMEOUT(info->text().contains(QStringLiteral("7 × 4")),5000);
    files->clearSelection();files->item(0)->setSelected(true);files->item(1)->setSelected(true);merge->click();QTRY_COMPARE_WITH_TIMEOUT(merged->count(),2,5000);QTRY_VERIFY_WITH_TIMEOUT(info->text().contains(QStringLiteral("7 × 2")),5000);
    QTest::mouseClick(merged->viewport(),Qt::LeftButton,Qt::NoModifier,merged->visualItemRect(merged->item(0)).center());
    QTest::qWait(100);QVERIFY(info->text().contains(QStringLiteral("7 × 2")));
    QTest::mouseDClick(merged->viewport(),Qt::LeftButton,Qt::NoModifier,merged->visualItemRect(merged->item(0)).center());
    QTRY_VERIFY_WITH_TIMEOUT(info->text().contains(QStringLiteral("7 × 4")),5000);
}

void TestMainWindow::elementPointCloudWindowIsReused() {
    QTemporaryDir directory;QVERIFY(directory.isValid());QImage source(4,4,QImage::Format_Grayscale8);for(int y=0;y<4;++y)for(int x=0;x<4;++x)source.setPixel(x,y,uchar((x+y)*24));
    const QString path=directory.filePath("elements.bmp");QVERIFY(source.save(path,"BMP"));QString error;const auto document=ImageLoader::load(path,850,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));auto* preview=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));auto* imageView=window.findChild<LargeImageView*>();
    QVERIFY(width);QVERIFY(height);QVERIFY(preview);QVERIFY(imageView);QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));width->setValue(2);height->setValue(2);preview->click();
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellActivated",Qt::DirectConnection,Q_ARG(int,0),Q_ARG(int,1)));
    auto* elementWindow=window.findChild<ElementPointCloudWindow*>(QStringLiteral("elementPointCloudWindow"));QVERIFY(elementWindow);QVERIFY(elementWindow->isVisible());
    QWindow* elementRenderWindow=nullptr;
    if(elementWindow->isVulkanAvailable()){QTRY_VERIFY_WITH_TIMEOUT(elementWindow->document()!=nullptr,5000);QCOMPARE(elementWindow->document()->source->metadata.width,qint64(2));QCOMPARE(elementWindow->document()->source->metadata.height,qint64(2));QVERIFY(elementWindow->document()->highestPoint.has_value());QVERIFY(elementWindow->document()->lowestPoint.has_value());for(QWindow* candidate:QGuiApplication::allWindows())if(candidate->objectName()==QStringLiteral("pointCloudVulkanWindow")&&candidate->isVisible()){elementRenderWindow=candidate;break;}QVERIFY(elementRenderWindow);const QPoint renderCenter(elementRenderWindow->width()/2,elementRenderWindow->height()/2);QTest::mousePress(elementRenderWindow,Qt::LeftButton,Qt::NoModifier,renderCenter);elementWindow->close();QTRY_VERIFY(!elementWindow->isVisible());}
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellActivated",Qt::DirectConnection,Q_ARG(int,1),Q_ARG(int,0)));QCOMPARE(window.findChildren<ElementPointCloudWindow*>().size(),1);QTRY_VERIFY(elementWindow->isVisible());
    if(elementRenderWindow)QTest::mouseRelease(elementRenderWindow,Qt::LeftButton,Qt::NoModifier,QPoint(elementRenderWindow->width()/2,elementRenderWindow->height()/2));
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));QVERIFY(!elementWindow->isVisible());
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
    QSignalSpy pointActivation(jobView,&JobPreviewWidget::placeActivated);
    QTest::mouseDClick(renderWindow,Qt::LeftButton,Qt::NoModifier,
                       {qRound(cyanPixel.x()/pixelRatio),qRound(cyanPixel.y()/pixelRatio)});
    QTRY_VERIFY_WITH_TIMEOUT(!pointActivation.isEmpty(),1000);
}

void TestMainWindow::settingsPersistenceAndReset() {
    {
        MainWindow window;
        auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
        auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
        auto* threshold=window.findChild<QSpinBox*>(QStringLiteral("segmentationThreshold"));
        auto* traversal=window.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"));
        auto* cloudInvert=window.findChild<QCheckBox*>(QStringLiteral("cloudInvertHeight"));
        QVERIFY(width);QVERIFY(height);QVERIFY(threshold);QVERIFY(traversal);QVERIFY(cloudInvert);
        width->setValue(321);height->setValue(123);threshold->setValue(77);traversal->setCurrentIndex(1);
        cloudInvert->setChecked(false);
        window.show();QTest::qWait(10);window.close();
    }
    MainWindow restored;
    auto* width=restored.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* height=restored.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* threshold=restored.findChild<QSpinBox*>(QStringLiteral("segmentationThreshold"));
    auto* traversal=restored.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"));
    auto* reset=restored.findChild<QPushButton*>(QStringLiteral("resetParametersButton"));
    auto* cloudInvert=restored.findChild<QCheckBox*>(QStringLiteral("cloudInvertHeight"));auto* cloudStart=restored.findChild<QPushButton*>(QStringLiteral("cloudStartColorButton"));auto* cloudEnd=restored.findChild<QPushButton*>(QStringLiteral("cloudEndColorButton"));
    QVERIFY(width);QVERIFY(height);QVERIFY(threshold);QVERIFY(traversal);QVERIFY(reset);QVERIFY(cloudInvert);QVERIFY(cloudStart);QVERIFY(cloudEnd);
    QCOMPARE(width->value(),321);QCOMPARE(height->value(),123);QCOMPARE(threshold->value(),77);
    QCOMPARE(traversal->currentIndex(),1);QVERIFY(!cloudInvert->isChecked());QCOMPARE(cloudStart->property("cloudColor").value<QColor>(),QColor(0,0,255));QCOMPARE(cloudEnd->property("cloudColor").value<QColor>(),QColor(0,255,0));
    const auto restoredLabels=restored.findChildren<QLabel*>();
    QVERIFY(std::any_of(restoredLabels.cbegin(),restoredLabels.cend(),
        [](const QLabel* label){return label->text()==QStringLiteral("未加载图像");}));
    QTest::mouseClick(reset,Qt::LeftButton);
    QCOMPARE(width->value(),220);QCOMPARE(height->value(),220);QCOMPARE(threshold->value(),128);
    QCOMPARE(traversal->currentIndex(),0);

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
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(2,1,QImage::Format_Grayscale8);source.fill(0);
    const QString path=directory.filePath("preflight.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,402,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{1,1,128,ForegroundPolarity::Dark});
    const QString output=directory.filePath("output");QVERIFY(QDir().mkpath(QDir(output).filePath("0")));
    LayeredPackageRequest request{output,document,grid,TraversalMode::SnakeZ,
        {0.25,1.0,0.1,1,1,false,false,false,LayerCountStrategy::RoundUp},true};
    const auto result=LayeredPackageExporter::inspect(request);
    QVERIFY(result.valid);QCOMPARE(result.conflictCount,qint64(1));
    QCOMPARE(result.firstOutput,QStringLiteral("0/1.bmp"));
    QCOMPARE(result.lastOutput,QStringLiteral("1/10.bmp"));
}

void TestMainWindow::segmentationStatisticsUpdates() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(5,3,QImage::Format_Grayscale8);source.fill(42);
    const QString path=directory.filePath("statistics.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,401,&error);QVERIFY2(document,qPrintable(error));
    const auto grid=PatternDetector::makeManualGrid(document,{4,2,128,ForegroundPolarity::Dark});
    LayeredExportDialog dialog;
    dialog.configure(document,grid,TraversalMode::SnakeZ,0.25,1.0,true,true,directory.path());
    auto* layer=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("layeredExportLayerHeight"));
    auto* zOffset=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("layeredExportZOffset"));
    auto* maximum=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("layeredExportMaximumHeight"));
    auto* strategy=dialog.findChild<QComboBox*>(QStringLiteral("layeredExportLayerStrategy"));
    auto* moveCommand=dialog.findChild<QComboBox*>(QStringLiteral("layeredExportJobMoveCommand"));
    auto* baseLayers=dialog.findChild<QSpinBox*>(QStringLiteral("layeredExportBaseLayerCount"));
    auto* multiplier=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("layeredExportMultiplier"));
    auto* width=dialog.findChild<QSpinBox*>(QStringLiteral("layeredExportOutputWidth"));
    auto* height=dialog.findChild<QSpinBox*>(QStringLiteral("layeredExportOutputHeight"));
    auto* invert=dialog.findChild<QCheckBox*>(QStringLiteral("layeredExportInvertMask"));
    auto* reverse=dialog.findChild<QCheckBox*>(QStringLiteral("layeredExportReverseY"));
    auto* summary=dialog.findChild<QLabel*>(QStringLiteral("layeredExportSummary"));
    auto* start=dialog.findChild<QPushButton*>(QStringLiteral("layeredExportStart"));
    auto* mainSplitter=dialog.findChild<QSplitter*>(QStringLiteral("layeredExportMainSplitter"));
    auto* settingsPanel=dialog.findChild<QWidget*>(QStringLiteral("layeredExportSettingsPanel"));
    auto* previewPanel=dialog.findChild<QWidget*>(QStringLiteral("layeredExportPreviewPanel"));
    auto* attach=dialog.findChild<QCheckBox*>(QStringLiteral("ck_attach"));
    auto* canvasWidth=dialog.findChild<QSpinBox*>(QStringLiteral("o_w"));
    auto* canvasHeight=dialog.findChild<QSpinBox*>(QStringLiteral("o_h"));
    auto* canvasBackground=dialog.findChild<QComboBox*>(QStringLiteral("layeredExportCanvasBackground"));
    auto* invertCanvas=dialog.findChild<QCheckBox*>(QStringLiteral("layeredExportInvertCanvas"));
    auto* rotation=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("rot_ang"));
    auto* scaleX=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("scale_x"));
    auto* scaleY=dialog.findChild<QDoubleSpinBox*>(QStringLiteral("scale_y"));
    auto* mirrorX=dialog.findChild<QCheckBox*>(QStringLiteral("layeredExportMirrorX"));
    auto* sampling=dialog.findChild<QComboBox*>(QStringLiteral("layeredExportTransformSampling"));
    auto* previewElement=dialog.findChild<QLabel*>(QStringLiteral("layeredExportPreviewElement"));
    QVERIFY(layer);QVERIFY(zOffset);QVERIFY(maximum);QVERIFY(strategy);QVERIFY(moveCommand);QVERIFY(baseLayers);QVERIFY(multiplier);QVERIFY(width);QVERIFY(height);
    QVERIFY(invert);QVERIFY(reverse);QVERIFY(summary);QVERIFY(start);
    QVERIFY(mainSplitter);QVERIFY(settingsPanel);QVERIFY(previewPanel);
    QVERIFY(attach);QVERIFY(canvasWidth);QVERIFY(canvasHeight);QVERIFY(canvasBackground);QVERIFY(invertCanvas);QVERIFY(rotation);
    QVERIFY(scaleX);QVERIFY(scaleY);QVERIFY(mirrorX);QVERIFY(sampling);QVERIFY(previewElement);
    QCOMPARE(mainSplitter->count(),2);
    QCOMPARE(mainSplitter->widget(0),settingsPanel);QCOMPARE(mainSplitter->widget(1),previewPanel);
    QVERIFY(settingsPanel->maximumWidth()<=400);
    QVERIFY(attach->isChecked());QCOMPARE(canvasWidth->value(),1920);QCOMPARE(canvasHeight->value(),1080);
    QCOMPARE(canvasBackground->currentText(),QStringLiteral("白色"));QVERIFY(!invertCanvas->isChecked());
    QCOMPARE(rotation->value(),0.0);QCOMPARE(scaleX->value(),1.0);QCOMPARE(scaleY->value(),1.0);QVERIFY(!mirrorX->isChecked());
    QCOMPARE(sampling->currentIndex(),0);QVERIFY(previewElement->text().contains(QStringLiteral("预览块：0")));
    QCOMPARE(layer->value(),0.1);QCOMPARE(maximum->value(),1.0);
    QCOMPARE(zOffset->value(),0.001);
    QCOMPARE(moveCommand->count(),2);
    QCOMPARE(moveCommand->itemText(0),QStringLiteral("relmove"));
    QCOMPARE(moveCommand->currentText(),QStringLiteral("absmove"));
    QCOMPARE(baseLayers->value(),0);QCOMPARE(baseLayers->singleStep(),1);
    QCOMPARE(baseLayers->minimum(),0);QCOMPARE(baseLayers->maximum(),kMaximumMaskSliceLayers);
    QCOMPARE(multiplier->value(),13.55);
    QCOMPARE(width->value(),14);QCOMPARE(height->value(),7);QVERIFY(!invert->isChecked());
    multiplier->setValue(2.125);
    QCOMPARE(width->value(),3);QCOMPARE(height->value(),2);
    width->setValue(11);QCOMPARE(width->value(),11);
    multiplier->setValue(13.55);
    QCOMPARE(width->value(),14);QCOMPARE(height->value(),7);
    multiplier->setValue(2000.0);
    QCOMPARE(width->value(),2000);QCOMPARE(height->value(),1000);
    QVERIFY(!start->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("超过导出底图")));
    QCOMPARE(canvasWidth->value(),1920);QCOMPARE(canvasHeight->value(),1080);
    multiplier->setValue(13.55);
    attach->setChecked(false);QVERIFY(!canvasWidth->isEnabled());QVERIFY(!canvasBackground->isEnabled());QVERIFY(!invertCanvas->isEnabled());QVERIFY(!rotation->isEnabled());QVERIFY(!mirrorX->isEnabled());
    attach->setChecked(true);QVERIFY(canvasWidth->isEnabled());QVERIFY(canvasBackground->isEnabled());QVERIFY(invertCanvas->isEnabled());QVERIFY(rotation->isEnabled());QVERIFY(mirrorX->isEnabled());
    mirrorX->setChecked(true);
    canvasWidth->setValue(3);QVERIFY(!start->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("超过导出底图")));
    canvasWidth->setValue(1920);QVERIFY(start->isEnabled());
    QVERIFY(reverse->isChecked());QVERIFY(summary->text().contains(QStringLiteral("10 层")));
    baseLayers->setValue(3);
    QVERIFY(summary->text().contains(QStringLiteral("底座 3 层")));
    QVERIFY(summary->text().contains(QStringLiteral("每组 13 层")));
    baseLayers->setValue(kMaximumMaskSliceLayers);
    QVERIFY(!start->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("总层数")));
    baseLayers->setValue(3);QVERIFY(start->isEnabled());
    maximum->setValue(1.05);QVERIFY(summary->text().contains(QStringLiteral("11 层")));
    strategy->setCurrentIndex(1);QVERIFY(!start->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("整除")));
    dialog.configure(document,grid,TraversalMode::SnakeZ,0.25,1.0,true,true,
                     directory.path(),QPoint(1,1),{QPoint(0,0),QPoint(1,1)});
    QCOMPARE(baseLayers->value(),0);
    QCOMPARE(multiplier->value(),13.55);
    QCOMPARE(width->value(),14);QCOMPARE(height->value(),7);
    QVERIFY(!mirrorX->isChecked());
    QCOMPARE(zOffset->value(),0.001);
    QCOMPARE(moveCommand->currentText(),QStringLiteral("absmove"));
    QVERIFY(previewElement->text().contains(QStringLiteral("预览块：2")));
    QVERIFY(summary->text().contains(QStringLiteral("仅导出已选 2 个分块")));

    auto* stack=dialog.findChild<MaskStackWidget*>(QStringLiteral("layeredExportStackView"));
    QVERIFY(stack);
    dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(stack->preview()!=nullptr,5000);
#if IMAGE_HANDLER_HAS_VULKAN_PREVIEW
    if(stack->isVulkanAvailable()){
        QVulkanWindow* renderWindow=nullptr;
        for(QWindow* candidate:QGuiApplication::allWindows()){
            if(candidate->objectName()==QStringLiteral("maskStackVulkanWindow")){
                renderWindow=qobject_cast<QVulkanWindow*>(candidate);
                break;
            }
        }
        QVERIFY(renderWindow);
        QVERIFY(renderWindow->flags().testFlag(QVulkanWindow::PersistentResources));

        QImage frame;
        QTRY_VERIFY_WITH_TIMEOUT(!(frame=stack->grabFrame()).isNull(),3000);
        QTRY_VERIFY_WITH_TIMEOUT(renderWindow->isExposed(),3000);
        bool hasGeometry=false;
        for(int y=0;y<frame.height()&&!hasGeometry;++y){
            for(int x=0;x<frame.width();++x){
                const QColor color=frame.pixelColor(x,y);
                if(color.blue()>60||color.green()>60||color.red()>60){
                    hasGeometry=true;
                    break;
                }
            }
        }
        QVERIFY2(hasGeometry,"Layered export Vulkan preview was blank in the side-by-side layout");
    }
#endif
}

void TestMainWindow::layeredExportMultiplierRoundsDecimalProducts() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QImage source(80, 160, QImage::Format_Grayscale8); source.fill(42);
    const QString path = directory.filePath("multiplier-source.bmp");
    QVERIFY(source.save(path, "BMP"));
    QString error;
    const auto document = ImageLoader::load(path, 402, &error);
    QVERIFY2(document, qPrintable(error));
    const auto grid = PatternDetector::makeManualGrid(document, {80, 160, 128, ForegroundPolarity::Dark});
    LayeredExportDialog dialog;
    dialog.configure(document, grid, TraversalMode::SnakeZ, 0.25, 1.0, false, false,
                     directory.path());
    auto* multiplier = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("layeredExportMultiplier"));
    auto* width = dialog.findChild<QSpinBox*>(QStringLiteral("layeredExportOutputWidth"));
    auto* height = dialog.findChild<QSpinBox*>(QStringLiteral("layeredExportOutputHeight"));
    QVERIFY(multiplier); QVERIFY(width); QVERIFY(height);
    QCOMPARE(width->value(), 271);
    QCOMPARE(height->value(), 542);
    multiplier->setValue(0.05);
    QCOMPARE(width->value(), 1);
    QCOMPARE(height->value(), 2);
    multiplier->setValue(13.550001);
    QCOMPARE(width->value(), 272);
    QCOMPARE(height->value(), 543);
    dialog.configure(document, grid, TraversalMode::SnakeZ, 0.5, 1.0, false, false,
                     directory.path());
    QCOMPARE(width->value(), 542);
    QCOMPARE(height->value(), 1084);
}

void TestMainWindow::layeredExportDialogWritesJobCommands() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QImage source(4, 1, QImage::Format_Grayscale8); source.fill(255);
    const QString path = directory.filePath("job-command-source.bmp");
    QVERIFY(source.save(path, "BMP"));
    QString error;
    const auto document = ImageLoader::load(path, 405, &error);
    QVERIFY2(document, qPrintable(error));
    const auto grid = PatternDetector::makeManualGrid(
        document, {1, 1, 128, ForegroundPolarity::Light});
    for (int scenario = 0; scenario < 3; ++scenario) {
        const QString output = directory.filePath(QStringLiteral("scenario-%1").arg(scenario));
        QVERIFY(QDir().mkpath(output));
        LayeredExportDialog dialog;
        const QVector<QPoint> selected = scenario < 2
            ? QVector<QPoint>{QPoint(3, 0), QPoint(1, 0)} : QVector<QPoint>{};
        dialog.configure(document, grid, TraversalMode::SnakeZ, 0.25, 1.0,
                         false, false, output, QPoint(1, 0), selected);
        auto* command = dialog.findChild<QComboBox*>(
            QStringLiteral("layeredExportJobMoveCommand"));
        auto* attach = dialog.findChild<QCheckBox*>(QStringLiteral("ck_attach"));
        auto* start = dialog.findChild<QPushButton*>(QStringLiteral("layeredExportStart"));
        QVERIFY(command); QVERIFY(attach); QVERIFY(start);
        attach->setChecked(false);
        command->setCurrentText(scenario == 1 ? QStringLiteral("relmove")
                                              : QStringLiteral("absmove"));
        QCOMPARE(command->currentText(), scenario == 1 ? QStringLiteral("relmove")
                                                       : QStringLiteral("absmove"));
        QVERIFY(start->isEnabled());
        QTimer responder;
        responder.setInterval(10);
        connect(&responder, &QTimer::timeout, &dialog, [&dialog] {
            for (QMessageBox* box : dialog.findChildren<QMessageBox*>()) {
                if (!box->isVisible()) continue;
                if (box->windowTitle() == QStringLiteral("确认分层导出")) {
                    for (QAbstractButton* button : box->buttons()) {
                        if (button->text() == QStringLiteral("开始导出")) {
                            button->click(); return;
                        }
                    }
                } else if (box->windowTitle() == QStringLiteral("分层导出")) {
                    box->accept(); return;
                }
            }
        });
        responder.start();
        bool timedOut = false;
        QTimer::singleShot(15000, &dialog, [&] { timedOut = true; dialog.reject(); });
        QTimer::singleShot(0, start, &QPushButton::click);
        const int result = dialog.exec();
        responder.stop();
        QVERIFY(!timedOut);
        QCOMPARE(result, int(QDialog::Accepted));
        QFile job(QDir(output).filePath(QStringLiteral("job.json")));
        QVERIFY(job.open(QIODevice::ReadOnly));
        const QJsonArray commands = QJsonDocument::fromJson(job.readAll())
            .object().value(QStringLiteral("commands")).toArray();
        QCOMPARE(commands[0].toObject().value(QStringLiteral("command")).toString(),
                 command->currentText());
        QCOMPARE(commands[0].toObject().value(QStringLiteral("by")).toArray(),
                 QJsonArray({0.0, 0.0, 0.0}));
        QCOMPARE(commands[2].toObject().value(QStringLiteral("by")).toArray(),
                 scenario < 2 ? QJsonArray({0.5, 0.0, 0.0})
                              : QJsonArray({0.25, 0.0, 0.0}));
        QFile report(QDir(output).filePath(QStringLiteral("export-report.json")));
        QVERIFY(report.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(report.readAll()).object()
                     .value(QStringLiteral("output")).toObject()
                     .value(QStringLiteral("jobMoveCommand")).toString(),
                 command->currentText());
    }
}

void TestMainWindow::maskSliceWindowDefaultsAndReuse() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(4,4,QImage::Format_Grayscale8);
    for(int y=0;y<4;++y)for(int x=0;x<4;++x)source.setPixel(x,y,uchar((x+y)*32));
    const QString path=directory.filePath("slice-ui.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,970,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;
    auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* preview=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* selectedExport=window.findChild<QPushButton*>(QStringLiteral("selectedBlocksExportButton"));
    auto* imageView=window.findChild<LargeImageView*>();
    QVERIFY(width);QVERIFY(height);QVERIFY(preview);QVERIFY(selectedExport);QVERIFY(imageView);
    QVERIFY(!selectedExport->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));
    width->setValue(2);height->setValue(2);preview->click();
    QVERIFY(!selectedExport->isEnabled());
    imageView->setSelectedCell(0,1);
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellsSelected",Qt::DirectConnection,
        Q_ARG(QVector<QPoint>,imageView->selectedCells()),Q_ARG(QPoint,QPoint(1,0))));
    QVERIFY(selectedExport->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellSliceActivated",Qt::DirectConnection,Q_ARG(int,0),Q_ARG(int,1)));
    auto* sliceWindow=window.findChild<MaskSliceWindow*>(QStringLiteral("maskSliceWindow"));QVERIFY(sliceWindow);QVERIFY(sliceWindow->isVisible());
    auto* layerHeight=sliceWindow->findChild<QDoubleSpinBox*>(QStringLiteral("maskSliceLayerHeight"));
    auto* zOffset=sliceWindow->findChild<QDoubleSpinBox*>(QStringLiteral("maskSliceZOffset"));
    auto* outputWidth=sliceWindow->findChild<QSpinBox*>(QStringLiteral("maskSliceOutputWidth"));
    auto* outputHeight=sliceWindow->findChild<QSpinBox*>(QStringLiteral("maskSliceOutputHeight"));
    auto* lock=sliceWindow->findChild<QCheckBox*>(QStringLiteral("maskSliceLockAspect"));
    auto* invert=sliceWindow->findChild<QCheckBox*>(QStringLiteral("maskSliceInvertMask"));
    auto* list=sliceWindow->findChild<QListWidget*>(QStringLiteral("maskSliceList"));
    auto* total=sliceWindow->findChild<QLabel*>(QStringLiteral("maskSliceTotal"));
    auto* splitter=sliceWindow->findChild<QSplitter*>(QStringLiteral("maskSliceSplitter"));
    auto* stack=sliceWindow->findChild<MaskStackWidget*>(QStringLiteral("maskStackWidget"));
    auto* stackMode=sliceWindow->findChild<QComboBox*>(QStringLiteral("maskStackMode"));
    auto* autoZ=sliceWindow->findChild<QCheckBox*>(QStringLiteral("maskStackAutoZ"));
    auto* zScale=sliceWindow->findChild<QDoubleSpinBox*>(QStringLiteral("maskStackZScale"));
    auto* layerInfo=sliceWindow->findChild<QLabel*>(QStringLiteral("maskStackLayerInfo"));
    auto* samplingInfo=sliceWindow->findChild<QLabel*>(QStringLiteral("maskStackSamplingInfo"));
    QVERIFY(layerHeight);QVERIFY(zOffset);QVERIFY(outputWidth);QVERIFY(outputHeight);QVERIFY(lock);QVERIFY(invert);QVERIFY(list);QVERIFY(total);
    QVERIFY(splitter);QCOMPARE(splitter->count(),2);QVERIFY(stack);QVERIFY(stackMode);QVERIFY(autoZ);QVERIFY(zScale);QVERIFY(layerInfo);QVERIFY(samplingInfo);
    QCOMPARE(stackMode->currentIndex(),0);QVERIFY(autoZ->isChecked());QVERIFY(!zScale->isEnabled());
    QCOMPARE(layerHeight->value(),0.1);QCOMPARE(layerHeight->singleStep(),0.1);
    QCOMPARE(zOffset->value(),0.001);
    QCOMPARE(outputWidth->value(),2);QCOMPARE(outputHeight->value(),2);QVERIFY(lock->isChecked());QVERIFY(!invert->isChecked());
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->analysis().has_value(),5000);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->stackAnalysis().has_value(),5000);
    QVERIFY(sliceWindow->stackAnalysis()->preview);QVERIFY(stack->preview());
    QCOMPARE(list->count(),sliceWindow->analysis()->layerCount);
    QCOMPARE(total->text(),QStringLiteral("掩模总数：%1").arg(list->count()));
    QCOMPARE(list->currentRow(),0);QCOMPARE(stack->selectedLayer(),1);
    QVERIFY(layerInfo->text().contains(QStringLiteral("第 1 层")));
    QVERIFY(layerInfo->text().contains(QStringLiteral("> 0.001000")));
    QVERIFY(samplingInfo->text().contains(QStringLiteral("重建高度")));
    if(stack->isVulkanAvailable()){
        QTest::qWait(80);const QImage frame=stack->grabFrame();QVERIFY(!frame.isNull());
    }
    if(list->count()>1){list->setCurrentRow(1);QCOMPARE(stack->selectedLayer(),2);QVERIFY(layerInfo->text().contains(QStringLiteral("第 2 层")));}
    stackMode->setCurrentIndex(1);QCOMPARE(stack->displayMode(),MaskStackDisplayMode::Exploded);
    autoZ->setChecked(false);QVERIFY(zScale->isEnabled());zScale->setValue(3.0);QCOMPARE(stack->zScale(),3.0);
    zOffset->setValue(20.0);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->analysis().has_value() &&
        sliceWindow->analysis()->thumbnails.front().pixelColor(0,0)==QColor(Qt::black),5000);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->stackAnalysis().has_value(),5000);
    QVERIFY(layerInfo->text().contains(QStringLiteral("> 20.000000")));
    outputWidth->setValue(4);QCOMPARE(outputHeight->value(),4);
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellSliceActivated",Qt::DirectConnection,Q_ARG(int,1),Q_ARG(int,0)));
    QCOMPARE(window.findChildren<MaskSliceWindow*>().size(),1);QVERIFY(sliceWindow->isVisible());
    QCOMPARE(outputWidth->value(),4);QCOMPARE(outputHeight->value(),4);
}

void TestMainWindow::maskSliceMultiSelectionAndStackPlacement() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QImage source(6,2,QImage::Format_Grayscale8);source.fill(0);
    for(int y=0;y<2;++y){source.scanLine(y)[0]=255;source.scanLine(y)[1]=255;
        source.scanLine(y)[4]=128;source.scanLine(y)[5]=128;}
    const QString path=directory.filePath("slice-multi-ui.bmp");QVERIFY(source.save(path,"BMP"));
    QString error;const auto document=ImageLoader::load(path,990,&error);QVERIFY2(document,qPrintable(error));
    MainWindow window;
    auto* width=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    auto* height=window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    auto* preview=window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    auto* maximumHeight=window.findChild<QDoubleSpinBox*>(QStringLiteral("cloudMaximumHeight"));
    auto* invertHeight=window.findChild<QCheckBox*>(QStringLiteral("cloudInvertHeight"));
    auto* imageView=window.findChild<LargeImageView*>();QVERIFY(width);QVERIFY(height);QVERIFY(preview);QVERIFY(imageView);
    QVERIFY(maximumHeight);QVERIFY(invertHeight);maximumHeight->setValue(1.0);invertHeight->setChecked(false);
    QVERIFY(QMetaObject::invokeMethod(&window,"acceptDocument",Qt::DirectConnection,Q_ARG(ImageDocumentPtr,document)));
    width->setValue(2);height->setValue(2);preview->click();
    const QVector<QPoint> cells{QPoint(0,0),QPoint(2,0)};
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellsSliceActivated",Qt::DirectConnection,
        Q_ARG(QVector<QPoint>,cells),Q_ARG(QPoint,QPoint(2,0))));
    auto* sliceWindow=window.findChild<MaskSliceWindow*>(QStringLiteral("maskSliceWindow"));
    QVERIFY(sliceWindow);QVERIFY(sliceWindow->isVisible());
    auto* elementList=sliceWindow->findChild<QListWidget*>(QStringLiteral("maskSliceElementList"));
    auto* elementContainer=sliceWindow->findChild<QWidget*>(QStringLiteral("maskSliceElementListContainer"));
    auto* outputWidth=sliceWindow->findChild<QSpinBox*>(QStringLiteral("maskSliceOutputWidth"));
    auto* outputHeight=sliceWindow->findChild<QSpinBox*>(QStringLiteral("maskSliceOutputHeight"));
    auto* elementInfo=sliceWindow->findChild<QLabel*>(QStringLiteral("maskSliceElementInfo"));
    auto* stack=sliceWindow->findChild<MaskStackWidget*>(QStringLiteral("maskStackWidget"));
    auto* layerList=sliceWindow->findChild<QListWidget*>(QStringLiteral("maskSliceList"));
    auto* total=sliceWindow->findChild<QLabel*>(QStringLiteral("maskSliceTotal"));
    auto* invert=sliceWindow->findChild<QCheckBox*>(QStringLiteral("maskSliceInvertMask"));
    QVERIFY(elementList);QVERIFY(elementContainer);QVERIFY(outputWidth);QVERIFY(outputHeight);
    QVERIFY(elementInfo);QVERIFY(stack);QVERIFY(layerList);QVERIFY(total);QVERIFY(invert);
    QVERIFY(elementContainer->isVisible());QCOMPARE(elementList->count(),2);
    QCOMPARE(elementList->item(0)->text(),QStringLiteral("1.bmp"));
    QCOMPARE(elementList->item(1)->text(),QStringLiteral("3.bmp"));
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->analysis().has_value(),5000);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->stackAnalysis().has_value(),5000);
    const auto groupPreview=sliceWindow->stackAnalysis()->preview;QVERIFY(groupPreview);
    QCOMPARE(sliceWindow->stackAnalysis()->commonLayerCount,10);
    QCOMPARE(sliceWindow->stackAnalysis()->elements.size(),2);
    QCOMPARE(sliceWindow->stackAnalysis()->elements[0].layerCount,10);
    QCOMPARE(sliceWindow->stackAnalysis()->elements[1].layerCount,6);
    QTRY_COMPARE_WITH_TIMEOUT(layerList->count(),10,5000);
    QVERIFY(total->text().contains(QStringLiteral("统一总层数：10")));
    QVERIFY(total->text().contains(QStringLiteral("本块计算：6")));
    QVERIFY(total->text().contains(QStringLiteral("补齐：4")));
    QVERIFY(layerList->item(6)->toolTip().contains(QStringLiteral("补齐底色层")));
    const QImage blackPadding=layerList->item(6)->icon().pixmap(16,16).toImage();
    QVERIFY(!blackPadding.isNull());QCOMPARE(blackPadding.pixelColor(
        blackPadding.width()/2,blackPadding.height()/2).rgb(),qRgb(0,0,0));
    invert->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->analysis().has_value(),5000);
    QTRY_COMPARE_WITH_TIMEOUT(layerList->count(),10,5000);
    const QImage whitePadding=layerList->item(6)->icon().pixmap(16,16).toImage();
    QVERIFY(!whitePadding.isNull());QCOMPARE(whitePadding.pixelColor(
        whitePadding.width()/2,whitePadding.height()/2).rgb(),qRgb(255,255,255));
    invert->setChecked(false);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->analysis().has_value(),5000);
    QCOMPARE(groupPreview->blocks.size(),2);QCOMPARE(groupPreview->physicalSize,QSizeF(1.5,0.5));
    QCOMPARE(groupPreview->blocks[0].physicalRect,QRectF(0,0,0.5,0.5));
    QCOMPARE(groupPreview->blocks[1].physicalRect,QRectF(1.0,0,0.5,0.5));

    outputWidth->setValue(4);QCOMPARE(outputHeight->value(),4);
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->stackAnalysis().has_value(),5000);
    const auto stackBeforeSwitch=stack->preview();QVERIFY(stackBeforeSwitch);
    Q_EMIT elementList->itemDoubleClicked(elementList->item(0));
    QCOMPARE(outputWidth->value(),4);QCOMPARE(outputHeight->value(),4);
    QCOMPARE(stack->preview(),stackBeforeSwitch);QVERIFY(elementInfo->text().contains(QStringLiteral("1.bmp")));
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->analysis().has_value(),5000);
    QTRY_COMPARE_WITH_TIMEOUT(layerList->count(),10,5000);
    QVERIFY(total->text().contains(QStringLiteral("本块计算：10")));
    QVERIFY(total->text().contains(QStringLiteral("补齐：0")));

    const QVector<QPoint> oneCell{QPoint(0,0)};
    QVERIFY(QMetaObject::invokeMethod(imageView,"segmentationCellsSelected",Qt::DirectConnection,
        Q_ARG(QVector<QPoint>,oneCell),Q_ARG(QPoint,QPoint(0,0))));
    QVERIFY(!elementContainer->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(sliceWindow->stackAnalysis().has_value(),5000);
    QCOMPARE(sliceWindow->stackAnalysis()->preview->blocks.size(),1);
}

QTEST_MAIN(TestMainWindow)
#include "TestMainWindow.moc"
