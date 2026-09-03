#include "ui/MainWindow.h"
#include "ui/LargeImageView.h"
#include "ui/JobPreviewWidget.h"
#include "ui/PointCloudWidget.h"
#include "ui/ElementPointCloudWindow.h"
#include "ui/MaskSliceWindow.h"
#include "ui/LayeredExportDialog.h"
#include "tasks/TaskManager.h"
#include "domain/MergeInputSelector.h"
#include "io/DirectoryScanner.h"
#include "io/MergedImageExporter.h"
#include "io/SplitExporter.h"
#include "io/JobFileLoader.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/PatternDetector.h"
#include "imaging/JobPreviewBuilder.h"
#include "imaging/PointCloudBuilder.h"
#include "imaging/PointCloudMath.h"

#include <QComboBox>
#include <QCheckBox>
#include <QApplication>
#include <QAbstractButton>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QItemSelectionModel>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSettings>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool isImagePath(const QString& path) {
    static const QSet<QString> extensions{QStringLiteral("bmp"), QStringLiteral("png"),
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("tif"),
        QStringLiteral("tiff")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

bool isJobPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("json") || suffix == QStringLiteral("aldl");
}

}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), tasks_(new TaskManager(this)) {
    buildUi(); setWindowTitle(QStringLiteral("大图像处理程序")); resize(1440,900);
    restoreSettings(); connectUi(); updateElementPhysicalSize(); setAcceptDrops(true);
    setBusyUi(false); setIdleTaskDisplay();
}

void MainWindow::buildUi() {
    auto* central=new QWidget(this); auto* root=new QVBoxLayout(central); root->setContentsMargins(6,6,6,6);
    auto* tools=new QHBoxLayout;
    openButton_=new QPushButton(QStringLiteral("打开图片")); directoryButton_=new QPushButton(QStringLiteral("打开目录"));
    mergeButton_=new QPushButton(QStringLiteral("图像合并"));mergeButton_->setObjectName(QStringLiteral("mergeImageButton")); detectButton_=new QPushButton(QStringLiteral("分割预览")); exportButton_=new QPushButton(QStringLiteral("导出分割"));
    auto* fit=new QPushButton(QStringLiteral("适应窗口")); auto* actual=new QPushButton(QStringLiteral("1:1"));
    jobImportButton_=new QPushButton(QStringLiteral("导入 Job"));jobImportButton_->setObjectName(QStringLiteral("jobImportButton"));
    cloudPixelScale_=new QDoubleSpinBox;cloudPixelScale_->setObjectName(QStringLiteral("cloudPixelScale"));cloudPixelScale_->setDecimals(6);cloudPixelScale_->setRange(0.000001,1000000.0);cloudPixelScale_->setValue(0.25);cloudPixelScale_->setSingleStep(0.05);cloudPixelScale_->setPrefix(QStringLiteral("比例 "));
    cloudMaximumHeight_=new QDoubleSpinBox;cloudMaximumHeight_->setObjectName(QStringLiteral("cloudMaximumHeight"));cloudMaximumHeight_->setDecimals(6);cloudMaximumHeight_->setRange(0.000001,1000000.0);cloudMaximumHeight_->setValue(12.5);cloudMaximumHeight_->setSingleStep(0.5);cloudMaximumHeight_->setPrefix(QStringLiteral("最大高度 "));
    cloudInvertHeight_=new QCheckBox(QStringLiteral("倒置高度"));cloudInvertHeight_->setObjectName(QStringLiteral("cloudInvertHeight"));cloudInvertHeight_->setChecked(true);
    cloudStartColorButton_=new QPushButton;cloudStartColorButton_->setObjectName(QStringLiteral("cloudStartColorButton"));cloudEndColorButton_=new QPushButton;cloudEndColorButton_->setObjectName(QStringLiteral("cloudEndColorButton"));updateCloudColorButton(cloudStartColorButton_,cloudStartColor_,QStringLiteral("起色"));updateCloudColorButton(cloudEndColorButton_,cloudEndColor_,QStringLiteral("终色"));
    generateCloudButton_=new QPushButton(QStringLiteral("GenerateCloud"));generateCloudButton_->setObjectName(QStringLiteral("generateCloudButton"));
    previewLayoutMode_=new QComboBox;previewLayoutMode_->setObjectName(QStringLiteral("previewLayoutMode"));previewLayoutMode_->addItems({QStringLiteral("页签"),QStringLiteral("左右并排")});
    autoJumpCheck_=new QCheckBox(QStringLiteral("联动时自动跳转"));autoJumpCheck_->setObjectName(QStringLiteral("jobAutoJumpCheck"));
    exportButton_->setObjectName(QStringLiteral("splitExportButton"));
    for(auto* b:{openButton_,directoryButton_,mergeButton_,detectButton_,jobImportButton_,fit,actual}) tools->addWidget(b);
    tools->addWidget(previewLayoutMode_);tools->addWidget(autoJumpCheck_);
    tools->addStretch(); imageInfo_=new QLabel(QStringLiteral("未加载图像"));imageInfo_->setObjectName(QStringLiteral("imageInfo")); zoomInfo_=new QLabel; tools->addWidget(imageInfo_);tools->addWidget(zoomInfo_); root->addLayout(tools);
    auto* cloudTools=new QHBoxLayout;cloudTools->addWidget(new QLabel(QStringLiteral("PointCloud 参数")));cloudTools->addWidget(cloudPixelScale_);cloudTools->addWidget(cloudMaximumHeight_);cloudTools->addWidget(cloudInvertHeight_);cloudTools->addWidget(cloudStartColorButton_);cloudTools->addWidget(cloudEndColorButton_);cloudTools->addWidget(generateCloudButton_);cloudTools->addStretch();root->addLayout(cloudTools);

    mainSplitter_=new QSplitter;mainSplitter_->setObjectName(QStringLiteral("mainSplitter"));
    auto* listHost=new QWidget;listHost->setMinimumWidth(210);listHost->setMaximumWidth(340);auto* listLayout=new QVBoxLayout(listHost);listLayout->setContentsMargins(0,0,0,0);
    listLayout->addWidget(new QLabel(QStringLiteral("目录图片（Ctrl/Shift 多选）")));
    fileList_=new QListWidget;fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);fileList_->setObjectName(QStringLiteral("directoryImageList"));listLayout->addWidget(fileList_,3);
    listLayout->addWidget(new QLabel(QStringLiteral("合并结果")));
    mergedList_=new QListWidget;mergedList_->setSelectionMode(QAbstractItemView::SingleSelection);mergedList_->setObjectName(QStringLiteral("mergedImageList"));listLayout->addWidget(mergedList_,2);
    mergedExportButton_=new QPushButton(QStringLiteral("导出选中合并图"));mergedExportButton_->setObjectName(QStringLiteral("mergedExportButton"));listLayout->addWidget(mergedExportButton_);mainSplitter_->addWidget(listHost);
    auto* previewWorkspace=new QWidget;auto* previewLayout=new QVBoxLayout(previewWorkspace);previewLayout->setContentsMargins(0,0,0,0);previewLayout->setSpacing(2);
    previewTabs_=new QTabBar;previewTabs_->setObjectName(QStringLiteral("previewTabs"));previewTabs_->addTab(QStringLiteral("图像预览"));previewTabs_->addTab(QStringLiteral("Job 预览"));previewTabs_->addTab(QStringLiteral("PointCloud"));previewTabs_->setExpanding(false);previewLayout->addWidget(previewTabs_);
    previewSplitter_=new QSplitter(Qt::Horizontal);previewSplitter_->setObjectName(QStringLiteral("previewSplitter"));previewLayout->addWidget(previewSplitter_,1);
    imagePreviewHost_=new QWidget;auto* imagePageLayout=new QVBoxLayout(imagePreviewHost_);imagePageLayout->setContentsMargins(0,0,0,0);imagePageLayout->setSpacing(2);auto* probeRow=new QWidget;probeRow->setStyleSheet(QStringLiteral("QWidget { background: #25282b; }"));auto* probeLayout=new QHBoxLayout(probeRow);probeLayout->setContentsMargins(0,0,4,0);probeLayout->setSpacing(4);pixelProbeLabel_=new QLabel(QStringLiteral("X: --  Y: --  灰度: --"));pixelProbeLabel_->setObjectName(QStringLiteral("pixelProbeLabel"));pixelProbeLabel_->setStyleSheet(QStringLiteral("QLabel { color: #d7e8ff; padding: 4px 8px; }"));clearFixedGrayPointsButton_=new QPushButton(QStringLiteral("清空标记"));clearFixedGrayPointsButton_->setObjectName(QStringLiteral("clearFixedGrayPointsButton"));clearFixedGrayPointsButton_->setEnabled(false);probeLayout->addWidget(pixelProbeLabel_);probeLayout->addStretch();probeLayout->addWidget(clearFixedGrayPointsButton_);imagePageLayout->addWidget(probeRow);auto* viewerRow=new QWidget;auto* viewerLayout=new QHBoxLayout(viewerRow);viewerLayout->setContentsMargins(0,0,0,0);
    previousButton_=new QPushButton(QStringLiteral("<")); previousButton_->setFixedWidth(36); nextButton_=new QPushButton(QStringLiteral(">"));nextButton_->setFixedWidth(36);
    view_=new LargeImageView;
    selectedCellLabel_=new QLabel(QStringLiteral("未选择"),view_);selectedCellLabel_->setObjectName(QStringLiteral("selectedSegmentationCell"));selectedCellLabel_->setAlignment(Qt::AlignCenter);selectedCellLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);selectedCellLabel_->setStyleSheet(QStringLiteral("QLabel { color: white; background: rgba(0, 0, 0, 150); border-radius: 4px; padding: 5px 12px; }"));
    auto* selectedCellOverlay=new QVBoxLayout(view_);selectedCellOverlay->setContentsMargins(12,12,12,18);selectedCellOverlay->addStretch();selectedCellOverlay->addWidget(selectedCellLabel_,0,Qt::AlignHCenter|Qt::AlignBottom);
    viewerLayout->addWidget(previousButton_);viewerLayout->addWidget(view_,1);viewerLayout->addWidget(nextButton_);imagePageLayout->addWidget(viewerRow,1);previewSplitter_->addWidget(imagePreviewHost_);
    jobView_=new JobPreviewWidget;previewSplitter_->addWidget(jobView_);pointCloudView_=new PointCloudWidget;previewSplitter_->addWidget(pointCloudView_);previewSplitter_->setStretchFactor(0,1);previewSplitter_->setStretchFactor(1,1);previewSplitter_->setStretchFactor(2,1);mainSplitter_->addWidget(previewWorkspace);
    auto* panel=new QWidget; panel->setMinimumWidth(230); panel->setMaximumWidth(300); auto* panelLayout=new QVBoxLayout(panel);
    auto* form=new QFormLayout;
    cellWidth_=new QSpinBox;cellWidth_->setObjectName(QStringLiteral("segmentationCellWidth"));cellWidth_->setRange(1,INT_MAX);cellWidth_->setValue(220);cellHeight_=new QSpinBox;cellHeight_->setObjectName(QStringLiteral("segmentationCellHeight"));cellHeight_->setRange(1,INT_MAX);cellHeight_->setValue(220);
    threshold_=new QSpinBox;threshold_->setObjectName(QStringLiteral("segmentationThreshold"));threshold_->setRange(0,255);threshold_->setValue(128);
    polarity_=new QComboBox;polarity_->addItems({QStringLiteral("暗色前景"),QStringLiteral("亮色前景")});
    traversal_=new QComboBox;traversal_->setObjectName(QStringLiteral("segmentationTraversal"));traversal_->addItems({QStringLiteral("Z 字形（蛇形）"),QStringLiteral("回字形（顺时针）")});
    form->addRow(QStringLiteral("分割宽度"),cellWidth_);form->addRow(QStringLiteral("分割高度"),cellHeight_);
    gridCountLabel_=new QLabel(QStringLiteral("-- × -- = --"));gridCountLabel_->setObjectName(QStringLiteral("segmentationGridCount"));gridCountLabel_->setStyleSheet(QStringLiteral("QLabel { color: #d32f2f; font-weight: 600; }"));
    form->addRow(QStringLiteral("行列数量"),gridCountLabel_);
    elementPhysicalSizeLabel_=new QLabel;elementPhysicalSizeLabel_->setObjectName(QStringLiteral("elementPhysicalSize"));elementPhysicalSizeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(QStringLiteral("单要素实际尺寸"),elementPhysicalSizeLabel_);
    form->addRow(QStringLiteral("阈值"),threshold_);form->addRow(QStringLiteral("前景极性"),polarity_);
    pathButton_=new QPushButton(QStringLiteral("显示路径"));pathButton_->setObjectName(QStringLiteral("traversalPathButton"));pathButton_->setCheckable(true);pathButton_->setEnabled(false);
    auto* traversalRow=new QHBoxLayout;traversalRow->setContentsMargins(0,0,0,0);traversalRow->addWidget(traversal_,1);traversalRow->addWidget(pathButton_);
    form->addRow(QStringLiteral("导出路径"),traversalRow);
    manualPreviewButton_=new QPushButton(QStringLiteral("应用参数并预览分割线"));
    manualPreviewButton_->setObjectName(QStringLiteral("manualSegmentationPreviewButton"));
    resetParametersButton_=new QPushButton(QStringLiteral("恢复默认参数"));resetParametersButton_->setObjectName(QStringLiteral("resetParametersButton"));
    selectedBlocksExportButton_=new QPushButton(QStringLiteral("导出选中块"));selectedBlocksExportButton_->setObjectName(QStringLiteral("selectedBlocksExportButton"));selectedBlocksExportButton_->setToolTip(QStringLiteral("按块编号导出，并保持原图文件格式、位深和像素内容"));
    panelLayout->addWidget(new QLabel(QStringLiteral("分割参数")));panelLayout->addLayout(form);panelLayout->addWidget(manualPreviewButton_);panelLayout->addWidget(exportButton_);panelLayout->addWidget(selectedBlocksExportButton_);panelLayout->addWidget(resetParametersButton_);panelLayout->addStretch();mainSplitter_->addWidget(panel);mainSplitter_->setStretchFactor(1,1);root->addWidget(mainSplitter_,1);

    auto* taskBar=new QHBoxLayout;taskBar->setContentsMargins(4,2,4,2);taskBar->setSpacing(10);
    taskTitle_=new QLabel;taskTitle_->setObjectName(QStringLiteral("taskTitle"));taskTitle_->setMinimumWidth(120);taskTitle_->setMaximumWidth(240);taskTitle_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);taskTitle_->installEventFilter(this);
    taskPhase_=new QLabel;taskPhase_->setObjectName(QStringLiteral("taskPhase"));taskPhase_->setMinimumWidth(150);taskPhase_->setMaximumWidth(320);taskPhase_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);taskPhase_->installEventFilter(this);
    progress_=new QProgressBar;progress_->setObjectName(QStringLiteral("taskProgress"));progress_->setTextVisible(false);progress_->setMinimumHeight(22);progress_->setMaximumHeight(22);
    progressPercent_=new QLabel;progressPercent_->setObjectName(QStringLiteral("taskProgressPercent"));progressPercent_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);progressPercent_->setFixedWidth(48);
    cancelButton_=new QPushButton(QStringLiteral("取消任务"));cancelButton_->setMinimumWidth(84);cancelButton_->setEnabled(false);
    taskBar->addWidget(taskTitle_);taskBar->addWidget(taskPhase_);taskBar->addWidget(progress_,1);taskBar->addWidget(progressPercent_);taskBar->addWidget(cancelButton_);root->addLayout(taskBar);setCentralWidget(central);
    taskResetTimer_=new QTimer(this);taskResetTimer_->setSingleShot(true);taskResetTimer_->setInterval(1800);
    jobPreviewTimer_=new QTimer(this);jobPreviewTimer_->setSingleShot(true);jobPreviewTimer_->setInterval(150);
    connect(fit,&QPushButton::clicked,view_,&LargeImageView::fitToWindow);connect(actual,&QPushButton::clicked,view_,&LargeImageView::actualSize);
    setPreviewLayoutMode(0);showPreviewTab(0);
}

void MainWindow::connectUi() {
    connect(openButton_,&QPushButton::clicked,this,&MainWindow::openImage);connect(directoryButton_,&QPushButton::clicked,this,&MainWindow::openDirectory);
    connect(jobImportButton_,&QPushButton::clicked,this,&MainWindow::chooseJobFile);
    connect(generateCloudButton_,&QPushButton::clicked,this,&MainWindow::generatePointCloud);
    connect(cloudStartColorButton_,&QPushButton::clicked,this,[this]{chooseCloudColor(true);});connect(cloudEndColorButton_,&QPushButton::clicked,this,[this]{chooseCloudColor(false);});
    connect(mergeButton_,&QPushButton::clicked,this,&MainWindow::mergeImages);connect(detectButton_,&QPushButton::clicked,this,&MainWindow::detectSegmentation);connect(exportButton_,&QPushButton::clicked,this,&MainWindow::chooseExportDirectory);connect(selectedBlocksExportButton_,&QPushButton::clicked,this,&MainWindow::chooseSelectedBlocksExportDirectory);
    connect(mergedExportButton_,&QPushButton::clicked,this,&MainWindow::chooseMergedExportPath);
    connect(previousButton_,&QPushButton::clicked,this,[this]{navigate(-1);});connect(nextButton_,&QPushButton::clicked,this,[this]{navigate(1);});
    connect(fileList_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem* item){if(item)loadPath(item->data(Qt::UserRole).toString());});
    connect(mergedList_,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem* item){if(item)showMergedDocument(mergedList_->row(item));});
    connect(mergedList_,&QListWidget::currentRowChanged,this,[this]{setBusyUi(tasks_->isBusy());});connect(cancelButton_,&QPushButton::clicked,tasks_,&TaskManager::cancel);
    connect(manualPreviewButton_,&QPushButton::clicked,this,&MainWindow::applyManualSegmentation);
    connect(resetParametersButton_,&QPushButton::clicked,this,&MainWindow::resetParameters);
    connect(clearFixedGrayPointsButton_,&QPushButton::clicked,view_,&LargeImageView::clearFixedGrayPoints);
    connect(pathButton_,&QPushButton::toggled,view_,&LargeImageView::setTraversalPathVisible);
    connect(traversal_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{view_->setTraversalMode(selectedTraversalMode());updateSelectedCellLabel();scheduleJobPreview(false);});
    connect(view_,&LargeImageView::segmentationCellSelected,this,[this](int row,int column){selectedRow_=row;selectedColumn_=column;updateSelectedCellLabel();selectJobFromImage(row,column);});
    connect(view_,&LargeImageView::segmentationCellsSelected,this,
            [this](const QVector<QPoint>& cells,const QPoint& activeCell){
        selectedBlocksExportButton_->setEnabled(!tasks_->isBusy()&&!cells.isEmpty()&&
                                                document_&&segmentation_.rows>0&&
                                                segmentation_.columns>0);
        if(maskSliceWindow_&&maskSliceWindow_->isVisible())
            syncMaskSliceSelection(cells,activeCell);
    });
    connect(view_,&LargeImageView::segmentationCellActivated,this,&MainWindow::showElementPointCloud);
    connect(view_,&LargeImageView::segmentationCellSliceActivated,this,
            qOverload<int,int>(&MainWindow::showMaskSlices));
    connect(view_,&LargeImageView::segmentationCellsSliceActivated,this,
            qOverload<const QVector<QPoint>&,const QPoint&>(&MainWindow::showMaskSlices));
    connect(jobView_,&JobPreviewWidget::placeSelected,this,&MainWindow::selectImageFromJob);
    connect(previewTabs_,&QTabBar::currentChanged,this,&MainWindow::showPreviewTab);
    connect(previewLayoutMode_,qOverload<int>(&QComboBox::currentIndexChanged),this,&MainWindow::setPreviewLayoutMode);
    connect(jobPreviewTimer_,&QTimer::timeout,this,&MainWindow::rebuildJobPreview);
    connect(cellWidth_,qOverload<int>(&QSpinBox::valueChanged),this,[this]{updateGridCountLabel();updateElementPhysicalSize();});
    connect(cellHeight_,qOverload<int>(&QSpinBox::valueChanged),this,[this]{updateGridCountLabel();updateElementPhysicalSize();});
    connect(cloudPixelScale_,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{updateElementPhysicalSize();syncMaskSliceMapping();scheduleJobPreview(false);});
    connect(cloudMaximumHeight_,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{syncMaskSliceMapping();});
    connect(cloudInvertHeight_,&QCheckBox::toggled,this,[this]{syncMaskSliceMapping();});
    connect(taskResetTimer_,&QTimer::timeout,this,&MainWindow::setIdleTaskDisplay);
    connect(view_,&LargeImageView::viewStatusChanged,this,[this](double z,qint64,qint64){zoomInfo_->setText(QStringLiteral("%1%").arg(z*100,0,'f',1));});
    connect(view_,&LargeImageView::pixelValueChanged,this,[this](qint64 x,qint64 y,quint32 value,bool valid){pixelProbeLabel_->setText(valid?QStringLiteral("X: %1  Y: %2  灰度: %3").arg(x).arg(y).arg(value):QStringLiteral("X: --  Y: --  灰度: --"));});
    connect(view_,&LargeImageView::fixedGrayPointCountChanged,this,[this](int count){clearFixedGrayPointsButton_->setEnabled(!tasks_->isBusy()&&count>0);});
    connect(view_,&LargeImageView::fixedGrayPointError,this,[this](const QString& error){statusBar()->showMessage(QStringLiteral("固定灰度点读取失败：%1").arg(error),8000);});
    connect(view_,&LargeImageView::tileError,this,[this](const QString& error){
        statusBar()->showMessage(QStringLiteral("图块加载失败：%1").arg(error), 8000);
    });
    connect(view_,&LargeImageView::tileProgress,this,[this](quint64 generation,qint64 completed,qint64 failed,qint64 total){
        if(tasks_->isBusy()||total<=0)return;
        const qint64 done=completed+failed;
        if(generation==mergePreviewGeneration_){
            const qint64 mapped=800+qRound64(200.0*std::clamp(double(done)/double(total),0.0,1.0));
            if(done<total){setTaskDisplay(QStringLiteral("合并图像"),QStringLiteral("准备首屏预览"),mapped,1000);return;}
            lastTilePhase_=QStringLiteral("加载可视图块");lastTileCompleted_=done;lastTileTotal_=total;
            mergePreviewGeneration_=0;
            if(failed>0){showTerminalTaskDisplay(QStringLiteral("合并预览失败"));statusBar()->showMessage(QStringLiteral("合并结果已保留，但首屏有 %1 个图块加载失败。").arg(failed),8000);}
            else showTerminalTaskDisplay(QStringLiteral("合并完成"));
            return;
        }
        const QString phase=QStringLiteral("加载可视图块");
        if(phase==lastTilePhase_&&done==lastTileCompleted_&&total==lastTileTotal_)return;
        lastTilePhase_=phase;lastTileCompleted_=done;lastTileTotal_=total;
        if(done<total)setTaskDisplay(QStringLiteral("后台渲染"),phase,done,total);
        else if(failed>0)showTerminalTaskDisplay(QStringLiteral("可视图块加载失败"));
        else showTerminalTaskDisplay(QStringLiteral("可视图块已加载"));
    });
    connect(tasks_,&TaskManager::taskStarted,this,[this](quint64,const QString& title){setBusyUi(true);setTaskDisplay(title,QStringLiteral("准备中"),0,0);});
    connect(tasks_,&TaskManager::taskProgress,this,[this](quint64,const TaskProgress& p){setTaskDisplay(taskTitle_->property("fullText").toString(),p.phase,p.completed,p.total);});
    connect(tasks_,&TaskManager::taskCancelled,this,[this](quint64){if(pendingAction_==PendingAction::JobPreviewBuild)switchToJobAfterBuild_=false;pendingMergePaths_.clear();pendingAction_=PendingAction::None;setBusyUi(false);showTerminalTaskDisplay(QStringLiteral("已取消"));});
    connect(tasks_,&TaskManager::taskFailed,this,[this](quint64,const QString& error){
        const auto action=pendingAction_;pendingAction_=PendingAction::None;if(action==PendingAction::Merge){pendingMergePaths_.clear();mergePreviewGeneration_=0;}setBusyUi(false);showTerminalTaskDisplay(QStringLiteral("失败"));
        if(action==PendingAction::JobPreviewBuild){switchToJobAfterBuild_=false;statusBar()->showMessage(QStringLiteral("Job 预览生成失败：%1").arg(error),8000);}
        else if(action==PendingAction::PointCloudBuild)statusBar()->showMessage(QStringLiteral("PointCloud 生成失败：%1").arg(error),8000);
        else if(action==PendingAction::JobLoad||action==PendingAction::JobLoadExport)QMessageBox::critical(this,QStringLiteral("Job 加载失败"),error);
        else QMessageBox::critical(this,QStringLiteral("操作失败"),error);
    });
    connect(tasks_,&TaskManager::taskFinished,this,[this](quint64,const QVariant& value){
        const auto action=pendingAction_;pendingAction_=PendingAction::None;setBusyUi(false);if(action!=PendingAction::Merge)showTerminalTaskDisplay(QStringLiteral("完成"));
        if(action==PendingAction::Scan){directoryPaths_=value.toStringList();fileList_->clear();for(const auto& p:directoryPaths_){auto* item=new QListWidgetItem(QFileInfo(p).fileName(),fileList_);item->setData(Qt::UserRole,p);}if(!directoryPaths_.isEmpty()){fileList_->setCurrentRow(0,QItemSelectionModel::ClearAndSelect);loadCurrentListItem();}else QMessageBox::information(this,QStringLiteral("扫描结果"),QStringLiteral("目录中没有支持的图片。"));}
        else if(action==PendingAction::Load)acceptDocument(value.value<ImageDocumentPtr>());
        else if(action==PendingAction::Merge)acceptMosaicResult(value.value<MosaicBuildResult>());
        else if(action==PendingAction::Detect){segmentation_=value.value<SegmentationResult>();for(auto*s:{cellWidth_,cellHeight_,threshold_})s->blockSignals(true);polarity_->blockSignals(true);cellWidth_->setValue(segmentation_.settings.cellWidth);cellHeight_->setValue(segmentation_.settings.cellHeight);threshold_->setValue(segmentation_.settings.threshold);polarity_->setCurrentIndex(segmentation_.settings.polarity==ForegroundPolarity::Dark?0:1);for(auto*s:{cellWidth_,cellHeight_,threshold_})s->blockSignals(false);polarity_->blockSignals(false);updateGridCountLabel();updateElementPhysicalSize();const bool valid=segmentation_.rows>0&&segmentation_.columns>0;if(!valid)pathButton_->setChecked(false);view_->setSegmentation(segmentation_);view_->setTraversalMode(selectedTraversalMode());view_->setTraversalPathVisible(pathButton_->isChecked());exportButton_->setEnabled(valid);pathButton_->setEnabled(valid);if(valid)scheduleJobPreview(true);if(!segmentation_.warning.isEmpty())QMessageBox::warning(this,QStringLiteral("识别提示"),segmentation_.warning);}
        else if(action==PendingAction::SelectedBlocksExport)QMessageBox::information(this,QStringLiteral("导出完成"),QStringLiteral("选中的块已成功导出。"));
        else if(action==PendingAction::JobLoad){acceptJobDocument(value.value<JobDocumentPtr>(),false);showPreviewTab(1);}
        else if(action==PendingAction::JobLoadExport){acceptJobDocument(value.value<JobDocumentPtr>(),true);showPreviewTab(1);}
        else if(action==PendingAction::JobPreviewBuild){acceptJobDocument(value.value<JobDocumentPtr>(),false);if(switchToJobAfterBuild_)showPreviewTab(1);switchToJobAfterBuild_=false;}
        else if(action==PendingAction::PointCloudBuild){pointCloudDocument_=value.value<PointCloudDocumentPtr>();pointCloudView_->setDocument(pointCloudDocument_);showPreviewTab(2);}
        else if(action==PendingAction::MosaicExport)QMessageBox::information(this,QStringLiteral("导出完成"),QStringLiteral("合并图已成功导出为 PNG，并保留原图位深。"));
    });
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        if (watched == taskTitle_) refreshElidedText(taskTitle_);
        else if (watched == taskPhase_) refreshElidedText(taskPhase_);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    if (elementCloudWindow_) elementCloudWindow_->cancelAndClear();
    if (maskSliceWindow_) maskSliceWindow_->cancelAndClear();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (tasks_->isBusy() || !event->mimeData()->hasUrls()) return;
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty() && std::all_of(urls.cbegin(), urls.cend(),
        [](const QUrl& url) { return url.isLocalFile(); })) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (tasks_->isBusy()) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在运行，暂时无法处理拖放。"), 5000);
        return;
    }
    const auto urls = event->mimeData()->urls();
    QStringList paths;
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            statusBar()->showMessage(QStringLiteral("仅支持拖放本地文件或目录。"), 5000);
            return;
        }
        const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
        if (!path.isEmpty()) paths << path;
    }
    if (paths.isEmpty()) return;
    if (paths.size() == 1 && QFileInfo(paths.front()).isDir()) {
        lastDirectoryDirectory_ = QFileInfo(paths.front()).absoluteFilePath();
        scanDirectory(paths.front());
        event->acceptProposedAction();
        return;
    }
    if (paths.size() == 1 && QFileInfo(paths.front()).isFile() && isJobPath(paths.front())) {
        if (!jobView_->isVulkanAvailable()) {
            statusBar()->showMessage(QStringLiteral("当前构建未启用 Vulkan Job 预览。"), 5000);
            return;
        }
        lastJobDirectory_ = QFileInfo(paths.front()).absolutePath();
        loadJobFile(paths.front(), false);
        event->acceptProposedAction();
        return;
    }
    if (std::all_of(paths.cbegin(), paths.cend(), [](const QString& path) {
            return QFileInfo(path).isFile() && isImagePath(path);
        })) {
        if (paths.size() == 1) {
            directoryPaths_.clear();
            fileList_->clear();
            lastImageDirectory_ = QFileInfo(paths.front()).absolutePath();
            loadPath(paths.front());
        } else {
            populateDroppedImages(paths);
        }
        event->acceptProposedAction();
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("拖放内容必须是单个目录、单个 Job，或仅包含受支持图片的文件集合。"), 6000);
}

void MainWindow::restoreSettings() {
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const QByteArray mainState = settings.value(QStringLiteral("window/mainSplitter")).toByteArray();
    if (!mainState.isEmpty()) mainSplitter_->restoreState(mainState);
    const QByteArray previewState = settings.value(QStringLiteral("window/previewSplitter")).toByteArray();

    const auto indexValue = [&settings](const QString& key, int defaultValue, int count) {
        bool ok = false;
        const int value = settings.value(key, defaultValue).toInt(&ok);
        return ok && value >= 0 && value < count ? value : defaultValue;
    };
    const auto boundedValue = [&settings](const QString& key, int defaultValue, int minimum,
                                          int maximum) {
        bool ok = false;
        const qlonglong value = settings.value(key, defaultValue).toLongLong(&ok);
        return ok && value >= minimum && value <= maximum ? static_cast<int>(value) : defaultValue;
    };
    cellWidth_->setValue(boundedValue(QStringLiteral("segmentation/cellWidth"), 220, 1, INT_MAX));
    cellHeight_->setValue(boundedValue(QStringLiteral("segmentation/cellHeight"), 220, 1, INT_MAX));
    threshold_->setValue(boundedValue(QStringLiteral("segmentation/threshold"), 128, 0, 255));
    polarity_->setCurrentIndex(indexValue(QStringLiteral("segmentation/polarity"), 0,
                                           polarity_->count()));
    traversal_->setCurrentIndex(indexValue(QStringLiteral("segmentation/traversal"), 0,
                                            traversal_->count()));
    reverseYSetting_ = settings.value(QStringLiteral("print/reverseY"), false).toBool();
    previewLayoutMode_->setCurrentIndex(indexValue(QStringLiteral("preview/layout"), 0,
                                                    previewLayoutMode_->count()));
    autoJumpCheck_->setChecked(settings.value(QStringLiteral("preview/autoJump"), false).toBool());
    cloudPixelScale_->setValue(settings.value(QStringLiteral("pointCloud/pixelScale"),0.25).toDouble());
    cloudMaximumHeight_->setValue(settings.value(QStringLiteral("pointCloud/maximumHeight"),12.5).toDouble());
    cloudInvertHeight_->setChecked(settings.value(QStringLiteral("pointCloud/invertHeight"),true).toBool());
    const QColor storedStart(settings.value(QStringLiteral("pointCloud/startColor"),QStringLiteral("#0000ff")).toString());const QColor storedEnd(settings.value(QStringLiteral("pointCloud/endColor"),QStringLiteral("#00ff00")).toString());cloudStartColor_=storedStart.isValid()?storedStart:QColor(0,0,255);cloudEndColor_=storedEnd.isValid()?storedEnd:QColor(0,255,0);updateCloudColorButton(cloudStartColorButton_,cloudStartColor_,QStringLiteral("起色"));updateCloudColorButton(cloudEndColorButton_,cloudEndColor_,QStringLiteral("终色"));
    setPreviewLayoutMode(previewLayoutMode_->currentIndex());
    if (!previewState.isEmpty()) previewSplitter_->restoreState(previewState);

    const QString home = QDir::homePath();
    lastImageDirectory_ = settings.value(QStringLiteral("paths/image"), home).toString();
    lastDirectoryDirectory_ = settings.value(QStringLiteral("paths/directory"), home).toString();
    lastJobDirectory_ = settings.value(QStringLiteral("paths/job"), home).toString();
    lastMergedExportDirectory_ = settings.value(QStringLiteral("paths/mergedExport"), home).toString();
    lastSplitExportDirectory_ = settings.value(QStringLiteral("paths/splitExport"), home).toString();
}

void MainWindow::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/mainSplitter"), mainSplitter_->saveState());
    settings.setValue(QStringLiteral("window/previewSplitter"), previewSplitter_->saveState());
    settings.setValue(QStringLiteral("segmentation/cellWidth"), cellWidth_->value());
    settings.setValue(QStringLiteral("segmentation/cellHeight"), cellHeight_->value());
    settings.setValue(QStringLiteral("segmentation/threshold"), threshold_->value());
    settings.setValue(QStringLiteral("segmentation/polarity"), polarity_->currentIndex());
    settings.setValue(QStringLiteral("segmentation/traversal"), traversal_->currentIndex());
    settings.setValue(QStringLiteral("print/reverseY"), reverseYSetting_);
    settings.setValue(QStringLiteral("preview/layout"), previewLayoutMode_->currentIndex());
    settings.setValue(QStringLiteral("preview/autoJump"), autoJumpCheck_->isChecked());
    settings.setValue(QStringLiteral("pointCloud/pixelScale"),cloudPixelScale_->value());
    settings.setValue(QStringLiteral("pointCloud/maximumHeight"),cloudMaximumHeight_->value());
    settings.setValue(QStringLiteral("pointCloud/invertHeight"),cloudInvertHeight_->isChecked());
    settings.setValue(QStringLiteral("pointCloud/startColor"),cloudStartColor_.name(QColor::HexRgb));settings.setValue(QStringLiteral("pointCloud/endColor"),cloudEndColor_.name(QColor::HexRgb));
    settings.setValue(QStringLiteral("paths/image"), lastImageDirectory_);
    settings.setValue(QStringLiteral("paths/directory"), lastDirectoryDirectory_);
    settings.setValue(QStringLiteral("paths/job"), lastJobDirectory_);
    settings.setValue(QStringLiteral("paths/mergedExport"), lastMergedExportDirectory_);
    settings.setValue(QStringLiteral("paths/splitExport"), lastSplitExportDirectory_);
    settings.sync();
}

void MainWindow::resetParameters() {
    const QSignalBlocker widthBlock(cellWidth_);
    const QSignalBlocker heightBlock(cellHeight_);
    const QSignalBlocker thresholdBlock(threshold_);
    const QSignalBlocker polarityBlock(polarity_);
    const QSignalBlocker traversalBlock(traversal_);
    cellWidth_->setValue(220); cellHeight_->setValue(220); threshold_->setValue(128);
    polarity_->setCurrentIndex(0); traversal_->setCurrentIndex(0);
    reverseYSetting_ = false;
    view_->setTraversalMode(TraversalMode::SnakeZ);
    updateGridCountLabel();
    updateElementPhysicalSize();
    updateSelectedCellLabel();
    scheduleJobPreview(false);
    statusBar()->showMessage(QStringLiteral("操作参数已恢复默认值。"), 3000);
}

void MainWindow::populateDroppedImages(const QStringList& paths) {
    QSet<QString> seen;
    directoryPaths_.clear();
    for (const QString& path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        const QString key = absolute.toCaseFolded();
        if (!seen.contains(key)) { seen.insert(key); directoryPaths_ << absolute; }
    }
    std::sort(directoryPaths_.begin(), directoryPaths_.end(), [](const QString& left,
                                                                 const QString& right) {
        const QString leftName = QFileInfo(left).fileName();
        const QString rightName = QFileInfo(right).fileName();
        if (DirectoryScanner::naturalLessThan(leftName, rightName)) return true;
        if (DirectoryScanner::naturalLessThan(rightName, leftName)) return false;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    fileList_->clear();
    for (const QString& path : directoryPaths_) {
        auto* item = new QListWidgetItem(QFileInfo(path).fileName(), fileList_);
        item->setData(Qt::UserRole, path);
        item->setSelected(true);
    }
    if (!directoryPaths_.isEmpty()) {
        fileList_->setCurrentRow(0, QItemSelectionModel::NoUpdate);
        lastImageDirectory_ = QFileInfo(directoryPaths_.front()).absolutePath();
        loadPath(directoryPaths_.front());
    }
    setBusyUi(tasks_->isBusy());
}

void MainWindow::setElidedText(QLabel* label, const QString& text) {
    if (!label) return;
    label->setProperty("fullText", text);
    label->setToolTip(text);
    refreshElidedText(label);
}

void MainWindow::refreshElidedText(QLabel* label) {
    if (!label) return;
    const QString fullText = label->property("fullText").toString();
    const int availableWidth = std::max(0, label->width() - label->contentsMargins().left() -
                                           label->contentsMargins().right());
    label->setText(label->fontMetrics().elidedText(fullText, Qt::ElideRight, availableWidth));
}

void MainWindow::setTaskDisplay(const QString& title, const QString& phase,
                                qint64 completed, qint64 total) {
    taskResetTimer_->stop();
    setElidedText(taskTitle_, title);
    setElidedText(taskPhase_, phase);
    progress_->show();
    progressPercent_->show();
    if (total <= 0) {
        progress_->setRange(0, 0);
        progressPercent_->setText(QStringLiteral("--"));
        return;
    }
    const double ratio = std::clamp(double(completed) / double(total), 0.0, 1.0);
    progress_->setRange(0, 1000);
    progress_->setValue(int(std::lround(ratio * 1000.0)));
    progressPercent_->setText(QStringLiteral("%1%").arg(int(std::lround(ratio * 100.0))));
}

void MainWindow::setIdleTaskDisplay() {
    taskResetTimer_->stop();
    setElidedText(taskTitle_, QStringLiteral("就绪"));
    setElidedText(taskPhase_, {});
    progress_->hide();
    progressPercent_->hide();
}

void MainWindow::showTerminalTaskDisplay(const QString& text) {
    setElidedText(taskTitle_, text);
    setElidedText(taskPhase_, {});
    progress_->hide();
    progressPercent_->hide();
    taskResetTimer_->start();
}

void MainWindow::updateGridCountLabel() {
    if (!document_ || cellWidth_->value() <= 0 || cellHeight_->value() <= 0) {
        gridCountLabel_->setText(QStringLiteral("-- × -- = --"));
        return;
    }
    const SegmentationSettings settings{cellWidth_->value(), cellHeight_->value(),
                                        threshold_->value(),
                                        polarity_->currentIndex() == 0
                                            ? ForegroundPolarity::Dark
                                            : ForegroundPolarity::Light};
    const SegmentationResult grid = PatternDetector::makeManualGrid(document_, settings);
    if (grid.columns <= 0 || grid.rows <= 0) {
        gridCountLabel_->setText(QStringLiteral("-- × -- = --"));
        return;
    }
    const qint64 total = qint64(grid.columns) * grid.rows;
    gridCountLabel_->setText(QStringLiteral("%1 × %2 = %3")
                                 .arg(grid.columns).arg(grid.rows).arg(total));
}

void MainWindow::updateElementPhysicalSize() {
    if (!elementPhysicalSizeLabel_ || !cellWidth_ || !cellHeight_ || !cloudPixelScale_) return;
    const double width = PointCloudMath::physicalDimension(cellWidth_->value(),
                                                           cloudPixelScale_->value());
    const double height = PointCloudMath::physicalDimension(cellHeight_->value(),
                                                            cloudPixelScale_->value());
    const QString text = QStringLiteral("%1 : %2")
        .arg(PointCloudMath::formatAxisMaximum(width),
             PointCloudMath::formatAxisMaximum(height));
    elementPhysicalSizeLabel_->setText(text);
    elementPhysicalSizeLabel_->setToolTip(
        QStringLiteral("%1 × %2 : %3 × %2")
            .arg(cellWidth_->value())
            .arg(PointCloudMath::formatAxisMaximum(cloudPixelScale_->value()))
            .arg(cellHeight_->value()));
}

void MainWindow::setBusyUi(bool busy) { openButton_->setEnabled(!busy);directoryButton_->setEnabled(!busy);jobImportButton_->setEnabled(!busy&&jobView_->isVulkanAvailable());generateCloudButton_->setEnabled(!busy&&document_&&ImageLoader::isProcessable(document_->metadata)&&pointCloudView_->isVulkanAvailable());cloudPixelScale_->setEnabled(!busy);cloudMaximumHeight_->setEnabled(!busy);cloudInvertHeight_->setEnabled(!busy);cloudStartColorButton_->setEnabled(!busy);cloudEndColorButton_->setEnabled(!busy);mergeButton_->setEnabled(!busy&&!directoryPaths_.isEmpty());detectButton_->setEnabled(!busy&&document_&&ImageLoader::isProcessable(document_->metadata));manualPreviewButton_->setEnabled(!busy&&document_&&ImageLoader::isProcessable(document_->metadata));const bool hasSegmentation=segmentation_.rows>0&&segmentation_.columns>0;exportButton_->setEnabled(!busy&&hasSegmentation);selectedBlocksExportButton_->setEnabled(!busy&&hasSegmentation&&view_&&!view_->selectedCells().isEmpty());pathButton_->setEnabled(!busy&&hasSegmentation);previousButton_->setEnabled(!busy&&!directoryPaths_.isEmpty());nextButton_->setEnabled(!busy&&!directoryPaths_.isEmpty());mergedExportButton_->setEnabled(!busy&&mergedList_->currentRow()>=0);fileList_->setEnabled(!busy);mergedList_->setEnabled(!busy);previewLayoutMode_->setEnabled(!busy);autoJumpCheck_->setEnabled(!busy);resetParametersButton_->setEnabled(!busy);clearFixedGrayPointsButton_->setEnabled(!busy&&!view_->fixedGrayPoints().isEmpty());cancelButton_->setEnabled(busy); }

void MainWindow::openImage(){const QString p=QFileDialog::getOpenFileName(this,QStringLiteral("打开图片"),lastImageDirectory_,QStringLiteral("图片 (*.bmp *.png *.jpg *.jpeg *.tif *.tiff)"));if(!p.isEmpty()){lastImageDirectory_=QFileInfo(p).absolutePath();directoryPaths_.clear();fileList_->clear();loadPath(p);}}
void MainWindow::openDirectory(){const QString d=QFileDialog::getExistingDirectory(this,QStringLiteral("选择图片目录"),lastDirectoryDirectory_);if(d.isEmpty())return;lastDirectoryDirectory_=QFileInfo(d).absoluteFilePath();scanDirectory(d);}
void MainWindow::scanDirectory(const QString& directory){if(directory.isEmpty()||tasks_->isBusy())return;pendingAction_=PendingAction::Scan;tasks_->start(QStringLiteral("扫描目录"),[directory](const auto&t,const auto&p,QString*){return QVariant(DirectoryScanner::scan(directory,t,p));});}
void MainWindow::loadPath(const QString& path){if(tasks_->isBusy())return;pendingAction_=PendingAction::Load;const quint64 gen=generation_++;tasks_->start(QStringLiteral("加载图片"),[path,gen](const auto&t,const auto&p,QString*e){p({QStringLiteral("读取元数据"),0,0});if(t->isCancelled())return QVariant{};auto doc=ImageLoader::load(path,gen,e);p({QStringLiteral("准备首屏预览"),1,1});return QVariant::fromValue(doc);});}
void MainWindow::loadCurrentListItem(){if(auto* item=fileList_->currentItem())loadPath(item->data(Qt::UserRole).toString());}
void MainWindow::navigate(int delta){if(fileList_->count()==0)return;const int row=std::clamp(fileList_->currentRow()+delta,0,fileList_->count()-1);fileList_->setCurrentRow(row,QItemSelectionModel::ClearAndSelect);loadCurrentListItem();}
void MainWindow::mergeImages(){if(directoryPaths_.isEmpty()){QMessageBox::information(this,QStringLiteral("图像合并"),QStringLiteral("请先打开包含合并图块的目录。"));return;}QStringList selectedPaths;for(const auto* item:fileList_->selectedItems())selectedPaths<<item->data(Qt::UserRole).toString();pendingMergePaths_=MergeInputSelector::resolve(directoryPaths_,selectedPaths);if(pendingMergePaths_.isEmpty()){QMessageBox::information(this,QStringLiteral("图像合并"),QStringLiteral("当前目录中没有可合并的图片。"));return;}startMosaicBuild(false);}
void MainWindow::startMosaicBuild(bool allowMissingTiles){if(tasks_->isBusy()||pendingMergePaths_.isEmpty())return;pendingAction_=PendingAction::Merge;mergePreviewGeneration_=0;const auto paths=pendingMergePaths_;const quint64 gen=generation_++;const MosaicBuildOptions options{allowMissingTiles};if(tasks_->start(QStringLiteral("合并图像"),[paths,gen,options](const auto&t,const auto&p,QString*e){return QVariant::fromValue(MosaicBuilder::build(paths,gen,options,t,p,e));})){setTaskDisplay(QStringLiteral("合并图像"),QStringLiteral("准备合并"),0,qint64(paths.size())*5);}else pendingAction_=PendingAction::None;}
void MainWindow::acceptMosaicResult(const MosaicBuildResult& result){if(result.status==MosaicBuildStatus::MissingTiles){QMessageBox warning(this);warning.setIcon(QMessageBox::Warning);warning.setWindowTitle(QStringLiteral("合并图像存在缺块"));QString detail=result.missingCoordinateSamples.join(QStringLiteral("、"));if(result.missingCount>result.missingCoordinateSamples.size())detail+=QStringLiteral(" 等");warning.setText(QStringLiteral("检测到 %1 个缺失坐标：%2\n继续后缺块及未覆盖区域将使用白色填充。").arg(result.missingCount).arg(detail));auto* continueButton=warning.addButton(QStringLiteral("同意并继续"),QMessageBox::AcceptRole);warning.addButton(QStringLiteral("取消"),QMessageBox::RejectRole);warning.exec();if(warning.clickedButton()==continueButton){startMosaicBuild(true);}else{pendingMergePaths_.clear();showTerminalTaskDisplay(QStringLiteral("已取消合并"));setBusyUi(false);}return;}if(!result.document){pendingMergePaths_.clear();return;}auto document=result.document;const int number=mergedDocuments_.size()+1;document->displayName=QStringLiteral("合并结果 %1（%2×%3，%4 张）").arg(number).arg(result.rows).arg(result.columns).arg(result.sourceCount);mergedDocuments_.push_back(document);auto* item=new QListWidgetItem(document->displayName,mergedList_);item->setData(Qt::UserRole,mergedDocuments_.size()-1);pendingMergePaths_.clear();mergedList_->setCurrentRow(mergedList_->count()-1);acceptDocument(document);mergePreviewGeneration_=document->generation;setTaskDisplay(QStringLiteral("合并图像"),QStringLiteral("准备首屏预览"),800,1000);setBusyUi(false);}
void MainWindow::showMergedDocument(int row){if(row<0||row>=mergedDocuments_.size())return;acceptDocument(mergedDocuments_[row]);setBusyUi(tasks_->isBusy());}
void MainWindow::chooseMergedExportPath(){const int row=mergedList_->currentRow();if(row<0||row>=mergedDocuments_.size())return;QString defaultName=QStringLiteral("合并结果_%1.png").arg(row+1);QString path=QFileDialog::getSaveFileName(this,QStringLiteral("导出合并图"),QDir(lastMergedExportDirectory_).filePath(defaultName),QStringLiteral("PNG 图片 (*.png)"));if(path.isEmpty())return;if(QFileInfo(path).suffix().compare(QStringLiteral("png"),Qt::CaseInsensitive)!=0)path+=QStringLiteral(".png");lastMergedExportDirectory_=QFileInfo(path).absolutePath();const auto document=mergedDocuments_[row];pendingAction_=PendingAction::MosaicExport;tasks_->start(QStringLiteral("导出合并图"),[path,document](const auto&t,const auto&p,QString*e){if(!MergedImageExporter::writePng(path,document,t,p,e))return QVariant{};return QVariant(true);});}
void MainWindow::detectSegmentation(){if(!document_){QMessageBox::information(this,QStringLiteral("分割预览"),QStringLiteral("请先加载图像。"));return;}pendingAction_=PendingAction::Detect;const auto doc=document_;tasks_->start(QStringLiteral("自动识别分割"),[doc](const auto&t,const auto&p,QString*e){return QVariant::fromValue(PatternDetector::detect(doc,t,p,e));});}
void MainWindow::applyManualSegmentation(){if(!document_||cellWidth_->value()<=0||cellHeight_->value()<=0)return;const SegmentationSettings settings{cellWidth_->value(),cellHeight_->value(),threshold_->value(),polarity_->currentIndex()==0?ForegroundPolarity::Dark:ForegroundPolarity::Light};const auto grid=PatternDetector::makeManualGrid(document_,settings);updateGridCountLabel();if(grid.rows<=0||grid.columns<=0){segmentation_=grid;pathButton_->setChecked(false);view_->clearSegmentation();exportButton_->setEnabled(false);pathButton_->setEnabled(false);statusBar()->showMessage(grid.warning,8000);return;}segmentation_=grid;view_->setSegmentation(segmentation_);view_->setTraversalMode(selectedTraversalMode());view_->setTraversalPathVisible(pathButton_->isChecked());exportButton_->setEnabled(true);pathButton_->setEnabled(true);scheduleJobPreview(true);if(grid.warning.isEmpty())statusBar()->clearMessage();else statusBar()->showMessage(grid.warning,8000);}
void MainWindow::chooseExportDirectory(){if(!document_||segmentation_.rows<=0||segmentation_.columns<=0){QMessageBox::information(this,QStringLiteral("导出"),QStringLiteral("请先完成分割预览。"));return;}LayeredExportDialog dialog(this);const QPoint active=view_?view_->selectedCell():QPoint(-1,-1);dialog.configure(document_,segmentation_,selectedTraversalMode(),cloudPixelScale_->value(),cloudMaximumHeight_->value(),cloudInvertHeight_->isChecked(),reverseYSetting_,lastSplitExportDirectory_,active);if(dialog.exec()!=QDialog::Accepted)return;reverseYSetting_=dialog.reverseY();lastSplitExportDirectory_=dialog.outputDirectory();scheduleJobPreview(false);loadJobFile(QDir(lastSplitExportDirectory_).filePath(QStringLiteral("job.json")),true);}

void MainWindow::chooseSelectedBlocksExportDirectory() {
    const QVector<QPoint> cells = view_ ? view_->selectedCells() : QVector<QPoint>{};
    if (!document_ || segmentation_.rows <= 0 || segmentation_.columns <= 0 ||
        cells.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出选中块"),
                                 QStringLiteral("请先在图像预览中选择至少一个块。"));
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择选中块输出目录"), lastSplitExportDirectory_);
    if (directory.isEmpty()) return;
    lastSplitExportDirectory_ = QFileInfo(directory).absoluteFilePath();

    QDir output(directory);
    int conflicts = 0;
    for (const QPoint& cell : cells) {
        const qint64 index = SplitExporter::traversalIndex(
            segmentation_.rows, segmentation_.columns, selectedTraversalMode(),
            cell.y(), cell.x());
        if (index < 0) continue;
        const QFileInfo target(output.filePath(
            SplitExporter::selectedTileFileName(document_, index)));
        if (!target.exists() && !target.isSymLink() && !target.isJunction()) continue;
        if (!target.isFile() || target.isSymLink() || target.isJunction()) {
            QMessageBox::critical(
                this, QStringLiteral("无法导出选中块"),
                QStringLiteral("目标路径不是可替换的普通文件：%1").arg(target.fileName()));
            return;
        }
        ++conflicts;
    }
    bool overwrite = false;
    if (conflicts > 0) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("确认替换"),
            QStringLiteral("输出目录中已有 %1 个同名块文件。是否替换这些文件？\n"
                           "目录中的其他文件不会被修改。")
                .arg(conflicts),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        overwrite = true;
    }

    const auto document = document_;
    const auto segmentation = segmentation_;
    const TraversalMode mode = selectedTraversalMode();
    pendingAction_ = PendingAction::SelectedBlocksExport;
    if (!tasks_->start(
            QStringLiteral("导出选中块"),
            [directory, document, segmentation, mode, cells, overwrite]
            (const auto& token, const auto& progress, QString* error) {
                const bool ok = SplitExporter::exportSelectedTiles(
                    directory, document, segmentation, mode, cells, overwrite,
                    token, progress, error);
                return ok ? QVariant(true) : QVariant{};
            })) {
        pendingAction_ = PendingAction::None;
    }
}

void MainWindow::acceptDocument(const ImageDocumentPtr& doc){if(!doc)return;mergePreviewGeneration_=0;if(elementCloudWindow_)elementCloudWindow_->cancelAndClear();if(maskSliceWindow_)maskSliceWindow_->cancelAndClear();document_=doc;segmentation_={};jobDocument_.reset();jobView_->setDocument({});pointCloudDocument_.reset();pointCloudView_->clear();selectedRow_=-1;selectedColumn_=-1;pathButton_->setChecked(false);lastTilePhase_.clear();lastTileCompleted_=-1;lastTileTotal_=-1;view_->setDocument(doc);showPreviewTab(0);updateSelectedCellLabel();imageInfo_->setText(QStringLiteral("%1 × %2 | %3").arg(doc->metadata.width).arg(doc->metadata.height).arg(pixelDescription(doc->metadata)));updateGridCountLabel();setBusyUi(tasks_->isBusy());}

void MainWindow::chooseJobFile(){const QString path=QFileDialog::getOpenFileName(this,QStringLiteral("选择 Job 文件"),lastJobDirectory_,QStringLiteral("Job 文件 (*.json *.aldl);;JSON (*.json);;ALDL (*.aldl)"));if(!path.isEmpty()){lastJobDirectory_=QFileInfo(path).absolutePath();loadJobFile(path,false);}}

void MainWindow::generatePointCloud(){if(tasks_->isBusy()||!document_)return;if(!ImageLoader::isProcessable(document_->metadata)){statusBar()->showMessage(QStringLiteral("PointCloud 仅支持二值、8-bit 和 16-bit 灰度图。"),6000);return;}if(!pointCloudView_->isVulkanAvailable()){statusBar()->showMessage(QStringLiteral("当前环境没有可用的 Vulkan PointCloud 渲染器。"),6000);return;}const auto source=document_;const PointCloudSettings settings{cloudPixelScale_->value(),cloudMaximumHeight_->value(),cloudInvertHeight_->isChecked(),cloudStartColor_,cloudEndColor_};const quint64 cloudGeneration=generation_++;pointCloudDocument_.reset();pointCloudView_->clear();showPreviewTab(2);pendingAction_=PendingAction::PointCloudBuild;tasks_->start(QStringLiteral("生成 PointCloud"),[source,settings,cloudGeneration](const auto&t,const auto&p,QString*e){return QVariant::fromValue(PointCloudBuilder::build(source,settings,cloudGeneration,t,p,e));});}

void MainWindow::showElementPointCloud(int row, int column){if(!document_||segmentation_.rows<=0||segmentation_.columns<=0)return;const ImageRegion region=segmentationCellRegion(segmentation_,document_->metadata.width,document_->metadata.height,row,column);if(!region.isValid())return;if(!ImageLoader::isProcessable(document_->metadata)){statusBar()->showMessage(QStringLiteral("单要素 PointCloud 仅支持二值、8-bit 和 16-bit 灰度图。"),6000);return;}if(!elementCloudWindow_)elementCloudWindow_=new ElementPointCloudWindow(this);if(!elementCloudWindow_->isVulkanAvailable()){elementCloudWindow_->showElement(document_,region,row,column,SplitExporter::traversalIndex(segmentation_.rows,segmentation_.columns,selectedTraversalMode(),row,column),{},generation_++);statusBar()->showMessage(QStringLiteral("当前环境没有可用的 Vulkan PointCloud 渲染器。"),6000);return;}const PointCloudSettings settings{cloudPixelScale_->value(),cloudMaximumHeight_->value(),cloudInvertHeight_->isChecked(),cloudStartColor_,cloudEndColor_};const qint64 index=SplitExporter::traversalIndex(segmentation_.rows,segmentation_.columns,selectedTraversalMode(),row,column);elementCloudWindow_->showElement(document_,region,row,column,index,settings,generation_++);}

void MainWindow::showMaskSlices(int row,int column){
    showMaskSlices({QPoint(column,row)},QPoint(column,row));
}

QVector<MaskSliceElement> MainWindow::maskSliceElements(const QVector<QPoint>& cells) const {
    QVector<MaskSliceElement> result;
    if(!document_||segmentation_.rows<=0||segmentation_.columns<=0)return result;
    result.reserve(cells.size());
    for(const QPoint& cell:cells){
        const ImageRegion region=segmentationCellRegion(segmentation_,document_->metadata.width,
            document_->metadata.height,cell.y(),cell.x());
        if(!region.isValid())continue;
        const qint64 index=SplitExporter::traversalIndex(segmentation_.rows,
            segmentation_.columns,selectedTraversalMode(),cell.y(),cell.x());
        if(index>=0)result.push_back({region,cell.y(),cell.x(),index});
    }
    std::sort(result.begin(),result.end(),[](const auto& left,const auto& right){
        return left.traversalIndex<right.traversalIndex;
    });
    return result;
}

void MainWindow::showMaskSlices(const QVector<QPoint>& cells,const QPoint& activeCell){
    if(!document_||segmentation_.rows<=0||segmentation_.columns<=0||cells.isEmpty())return;
    if(!ImageLoader::isProcessable(document_->metadata)){
        statusBar()->showMessage(QStringLiteral("单元分层切片仅支持二值、8-bit 和 16-bit 灰度图。"),6000);
        return;
    }
    const QVector<MaskSliceElement> elements=maskSliceElements(cells);
    if(elements.isEmpty())return;
    if(!maskSliceWindow_)maskSliceWindow_=new MaskSliceWindow(this);
    const PointCloudSettings mapping{cloudPixelScale_->value(),cloudMaximumHeight_->value(),
        cloudInvertHeight_->isChecked(),cloudStartColor_,cloudEndColor_};
    maskSliceWindow_->showElements(document_,elements,activeCell,mapping);
}

void MainWindow::syncMaskSliceSelection(const QVector<QPoint>& cells,const QPoint& activeCell){
    if(!maskSliceWindow_||!maskSliceWindow_->isVisible())return;
    if(cells.isEmpty()){
        maskSliceWindow_->cancelAndClear();
        return;
    }
    const QVector<MaskSliceElement> elements=maskSliceElements(cells);
    if(elements.isEmpty())return;
    const PointCloudSettings mapping{cloudPixelScale_->value(),cloudMaximumHeight_->value(),
        cloudInvertHeight_->isChecked(),cloudStartColor_,cloudEndColor_};
    maskSliceWindow_->setElements(document_,elements,activeCell,mapping);
}

void MainWindow::syncMaskSliceMapping(){if(!maskSliceWindow_)return;maskSliceWindow_->setPointCloudSettings({cloudPixelScale_->value(),cloudMaximumHeight_->value(),cloudInvertHeight_->isChecked(),cloudStartColor_,cloudEndColor_});}

void MainWindow::chooseCloudColor(bool startColor){QColor& current=startColor?cloudStartColor_:cloudEndColor_;QColorDialog dialog(current,this);dialog.setObjectName(QStringLiteral("cloudColorDialog"));dialog.setWindowTitle(startColor?QStringLiteral("选择 PointCloud 起色"):QStringLiteral("选择 PointCloud 终色"));dialog.setOption(QColorDialog::DontUseNativeDialog);if(dialog.exec()!=QDialog::Accepted)return;const QColor selected=dialog.selectedColor();if(!selected.isValid())return;current=selected;updateCloudColorButton(startColor?cloudStartColorButton_:cloudEndColorButton_,current,startColor?QStringLiteral("起色"):QStringLiteral("终色"));syncMaskSliceMapping();}

void MainWindow::updateCloudColorButton(QPushButton* button,const QColor& color,const QString& label){if(!button)return;button->setText(label);button->setProperty("cloudColor",color);const QString foreground=color.lightness()<128?QStringLiteral("white"):QStringLiteral("black");button->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; border: 1px solid #666; padding: 4px 10px; }").arg(color.name(QColor::HexRgb),foreground));}

void MainWindow::loadJobFile(const QString& path,bool exported){if(path.isEmpty())return;pendingAction_=exported?PendingAction::JobLoadExport:PendingAction::JobLoad;tasks_->start(exported?QStringLiteral("加载导出 Job"):QStringLiteral("导入 Job"),[path](const auto&t,const auto&p,QString*e){return QVariant::fromValue(JobFileLoader::load(path,t,p,e));});}

void MainWindow::scheduleJobPreview(bool switchToJob){if(!jobView_->isVulkanAvailable()||!document_||segmentation_.rows<=0||segmentation_.columns<=0)return;switchToJobAfterBuild_=switchToJobAfterBuild_||switchToJob;if(switchToJob){jobDocument_.reset();jobView_->setDocument({});showPreviewTab(1);}jobPreviewTimer_->start();}

void MainWindow::rebuildJobPreview(){if(!document_||segmentation_.rows<=0||segmentation_.columns<=0)return;if(tasks_->isBusy()){jobPreviewTimer_->start();return;}const auto metadata=document_->metadata;const auto segmentation=segmentation_;const auto mode=selectedTraversalMode();const JobPlacementOptions options{cloudPixelScale_->value(),reverseYSetting_};pendingAction_=PendingAction::JobPreviewBuild;if(!tasks_->start(QStringLiteral("生成 Job 预览"),[metadata,segmentation,mode,options](const auto&t,const auto&p,QString*e){return QVariant::fromValue(JobPreviewBuilder::build(metadata,segmentation,mode,options,t,p,e));})){pendingAction_=PendingAction::None;jobPreviewTimer_->start();}}

void MainWindow::acceptJobDocument(const JobDocumentPtr& document,bool preserveLink){if(!document)return;if(preserveLink){document->source=JobDocumentSource::ExportedFile;if(jobDocument_&&jobDocument_->isLinked()&&jobDocument_->points.size()==document->points.size())document->linkedCells=jobDocument_->linkedCells;else{const auto order=SplitExporter::traversal(segmentation_.rows,segmentation_.columns,selectedTraversalMode());if(order.size()==document->points.size())document->linkedCells=order;}}jobDocument_=document;jobView_->setDocument(jobDocument_);if(jobDocument_->isLinked()&&selectedRow_>=0&&selectedColumn_>=0)selectJobFromImage(selectedRow_,selectedColumn_);else jobView_->clearSelection();}

void MainWindow::setPreviewLayoutMode(int mode){if(!previewSplitter_||!imagePreviewHost_||!jobView_||!pointCloudView_)return;if(previewTabs_&&previewTabs_->currentIndex()==2){imagePreviewHost_->hide();jobView_->hide();pointCloudView_->show();return;}pointCloudView_->hide();if(mode==1){imagePreviewHost_->show();jobView_->show();previewSplitter_->setSizes({1,1,0});}else showPreviewTab(previewTabs_?previewTabs_->currentIndex():0);}

void MainWindow::showPreviewTab(int index){if(!previewTabs_||!imagePreviewHost_||!jobView_||!pointCloudView_)return;index=std::clamp(index,0,2);{const QSignalBlocker blocker(previewTabs_);previewTabs_->setCurrentIndex(index);}if(index==2){imagePreviewHost_->hide();jobView_->hide();pointCloudView_->show();return;}pointCloudView_->hide();if(previewLayoutMode_&&previewLayoutMode_->currentIndex()==1){imagePreviewHost_->show();jobView_->show();return;}imagePreviewHost_->setVisible(index==0);jobView_->setVisible(index==1);}

void MainWindow::selectJobFromImage(int row,int column){if(!jobDocument_||!jobDocument_->isLinked()){if(row<0||column<0)jobView_->clearSelection();return;}const qint64 id=SplitExporter::traversalIndex(segmentation_.rows,segmentation_.columns,selectedTraversalMode(),row,column);const bool jump=autoJumpCheck_->isChecked();jobView_->setSelectedPlaceId(id,jump,true);if(jump&&previewLayoutMode_->currentIndex()==0)showPreviewTab(1);}

void MainWindow::selectImageFromJob(qint64 placeId){jobView_->setSelectedPlaceId(placeId,false);if(!jobDocument_||!jobDocument_->isLinked()||placeId<0||placeId>=static_cast<qint64>(jobDocument_->linkedCells.size()))return;const QPoint cell=jobDocument_->linkedCells[static_cast<size_t>(placeId)];selectedRow_=cell.y();selectedColumn_=cell.x();const bool jump=autoJumpCheck_->isChecked();view_->setSelectedCell(selectedRow_,selectedColumn_,jump);updateSelectedCellLabel();if(jump&&previewLayoutMode_->currentIndex()==0)showPreviewTab(0);}

TraversalMode MainWindow::selectedTraversalMode() const{return traversal_->currentIndex()==0?TraversalMode::SnakeZ:TraversalMode::ClockwiseSpiral;}
void MainWindow::updateSelectedCellLabel(){if(!selectedCellLabel_)return;if(selectedRow_<0||selectedColumn_<0||segmentation_.rows<=0||segmentation_.columns<=0){selectedCellLabel_->setText(QStringLiteral("未选择"));return;}const qint64 index=SplitExporter::traversalIndex(segmentation_.rows,segmentation_.columns,selectedTraversalMode(),selectedRow_,selectedColumn_);if(index<0){selectedCellLabel_->setText(QStringLiteral("未选择"));return;}const int count=view_?view_->selectedCells().size():1;selectedCellLabel_->setText(count>1?QStringLiteral("已选 %1 块 | 当前 %2x%3 : %4.bmp").arg(count).arg(selectedRow_+1).arg(selectedColumn_+1).arg(index+1):QStringLiteral("%1x%2 : %3.bmp").arg(selectedRow_+1).arg(selectedColumn_+1).arg(index+1));}
QString MainWindow::pixelDescription(const ImageMetadata& m)const{switch(m.kind){case PixelKind::Binary1:return QStringLiteral("1-bit 二值");case PixelKind::Gray8:return QStringLiteral("8-bit 灰度");case PixelKind::Gray16:return QStringLiteral("16-bit 灰度");case PixelKind::Color:return QStringLiteral("彩色");default:return QStringLiteral("不支持");}}
