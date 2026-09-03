#include "ui/LayeredExportDialog.h"

#include "domain/StorageSizeFormatter.h"
#include "imaging/LayeredMaskProcessor.h"
#include "io/LayeredPackageExporter.h"
#include "io/SplitExporter.h"
#include "tasks/TaskManager.h"
#include "ui/MaskStackWidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QSpinBox>
#include <QSplitter>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

LayeredExportDialog::LayeredExportDialog(QWidget* parent)
    : QDialog(parent), tasks_(new TaskManager(this)), previewTasks_(new TaskManager(this)),
      previewTimer_(new QTimer(this)) {
    setObjectName(QStringLiteral("layeredExportDialog"));
    setWindowTitle(QStringLiteral("分层导出设置"));
    setModal(true);
    resize(1180, 760);
    auto* root = new QVBoxLayout(this);
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("layeredExportTabs"));

    auto* settingsPage = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsPage);
    auto* directoryRow = new QHBoxLayout;
    directoryButton_ = new QPushButton(QStringLiteral("选择导出目录"));
    directoryButton_->setObjectName(QStringLiteral("layeredExportChooseDirectory"));
    directoryLabel_ = new QLabel(QStringLiteral("未选择"));
    directoryLabel_->setObjectName(QStringLiteral("layeredExportDirectory"));
    directoryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    directoryLabel_->setMinimumWidth(300);
    directoryRow->addWidget(directoryButton_);
    directoryRow->addWidget(directoryLabel_, 1);
    settingsLayout->addLayout(directoryRow);

    auto* form = new QFormLayout;
    layerHeight_ = new QDoubleSpinBox;
    layerHeight_->setObjectName(QStringLiteral("layeredExportLayerHeight"));
    layerHeight_->setDecimals(6); layerHeight_->setRange(0.000001, 1000000.0);
    layerHeight_->setSingleStep(0.1); layerHeight_->setValue(0.1);
    layerHeight_->setSuffix(QStringLiteral(" μm"));
    maximumHeight_ = new QDoubleSpinBox;
    maximumHeight_->setObjectName(QStringLiteral("layeredExportMaximumHeight"));
    maximumHeight_->setDecimals(6); maximumHeight_->setRange(0.000001, 1000000.0);
    maximumHeight_->setSingleStep(0.5); maximumHeight_->setSuffix(QStringLiteral(" μm"));
    layerStrategy_ = new QComboBox;
    layerStrategy_->setObjectName(QStringLiteral("layeredExportLayerStrategy"));
    layerStrategy_->addItem(QStringLiteral("向上取整"), QVariant::fromValue(LayerCountStrategy::RoundUp));
    layerStrategy_->addItem(QStringLiteral("必须整除"), QVariant::fromValue(LayerCountStrategy::RequireExact));
    outputWidth_ = new QSpinBox;
    outputWidth_->setObjectName(QStringLiteral("layeredExportOutputWidth"));
    outputWidth_->setRange(1, INT_MAX); outputWidth_->setSuffix(QStringLiteral(" px"));
    outputHeight_ = new QSpinBox;
    outputHeight_->setObjectName(QStringLiteral("layeredExportOutputHeight"));
    outputHeight_->setRange(1, INT_MAX); outputHeight_->setSuffix(QStringLiteral(" px"));
    lockAspect_ = new QCheckBox(QStringLiteral("锁定原始宽高比"));
    lockAspect_->setObjectName(QStringLiteral("layeredExportLockAspect")); lockAspect_->setChecked(true);
    invertMask_ = new QCheckBox(QStringLiteral("黑白反转"));
    invertMask_->setObjectName(QStringLiteral("layeredExportInvertMask"));
    reverseY_ = new QCheckBox(QStringLiteral("Y方向相反"));
    reverseY_->setObjectName(QStringLiteral("layeredExportReverseY"));
    attachToCanvas_ = new QCheckBox(QStringLiteral("贴入大图"));
    attachToCanvas_->setObjectName(QStringLiteral("ck_attach")); attachToCanvas_->setChecked(true);
    canvasWidth_ = new QSpinBox;
    canvasWidth_->setObjectName(QStringLiteral("o_w")); canvasWidth_->setRange(1, INT_MAX);
    canvasWidth_->setValue(1920); canvasWidth_->setSuffix(QStringLiteral(" px"));
    canvasHeight_ = new QSpinBox;
    canvasHeight_->setObjectName(QStringLiteral("o_h")); canvasHeight_->setRange(1, INT_MAX);
    canvasHeight_->setValue(1080); canvasHeight_->setSuffix(QStringLiteral(" px"));
    rotationDegrees_ = new QDoubleSpinBox;
    rotationDegrees_->setObjectName(QStringLiteral("rot_ang")); rotationDegrees_->setDecimals(3);
    rotationDegrees_->setRange(-360.0, 360.0); rotationDegrees_->setSuffix(QStringLiteral(" °"));
    scaleX_ = new QDoubleSpinBox;
    scaleX_->setObjectName(QStringLiteral("scale_x")); scaleX_->setDecimals(6);
    scaleX_->setRange(0.000001, 1000.0); scaleX_->setValue(1.0);
    scaleY_ = new QDoubleSpinBox;
    scaleY_->setObjectName(QStringLiteral("scale_y")); scaleY_->setDecimals(6);
    scaleY_->setRange(0.000001, 1000.0); scaleY_->setValue(1.0);
    transformSampling_ = new QComboBox;
    transformSampling_->setObjectName(QStringLiteral("layeredExportTransformSampling"));
    transformSampling_->addItem(QStringLiteral("最近邻"), QVariant::fromValue(BinaryTransformSampling::Nearest));
    transformSampling_->addItem(QStringLiteral("平滑后阈值"), QVariant::fromValue(BinaryTransformSampling::SmoothThreshold));
    form->addRow(QStringLiteral("层切高"), layerHeight_);
    form->addRow(QStringLiteral("最大高度"), maximumHeight_);
    form->addRow(QStringLiteral("层数策略"), layerStrategy_);
    form->addRow(QStringLiteral("标准块输出宽度"), outputWidth_);
    form->addRow(QStringLiteral("标准块输出高度"), outputHeight_);
    form->addRow(lockAspect_); form->addRow(invertMask_); form->addRow(reverseY_);
    form->addRow(attachToCanvas_);
    form->addRow(QStringLiteral("导出底图宽度"), canvasWidth_);
    form->addRow(QStringLiteral("导出底图高度"), canvasHeight_);
    form->addRow(QStringLiteral("旋转角度"), rotationDegrees_);
    form->addRow(QStringLiteral("X 缩放"), scaleX_);
    form->addRow(QStringLiteral("Y 缩放"), scaleY_);
    form->addRow(QStringLiteral("旋转采样"), transformSampling_);
    settingsLayout->addLayout(form);
    summaryLabel_ = new QLabel;
    summaryLabel_->setObjectName(QStringLiteral("layeredExportSummary"));
    summaryLabel_->setWordWrap(true); settingsLayout->addWidget(summaryLabel_);
    settingsLayout->addStretch();
    tabs_->addTab(settingsPage, QStringLiteral("导出设置"));

    auto* previewPage = new QWidget;
    auto* previewLayout = new QVBoxLayout(previewPage);
    previewElementLabel_ = new QLabel(QStringLiteral("预览块：--"));
    previewElementLabel_->setObjectName(QStringLiteral("layeredExportPreviewElement"));
    previewLayout->addWidget(previewElementLabel_);
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName(QStringLiteral("layeredExportPreviewSplitter"));
    auto* stackPanel = new QWidget;
    auto* stackLayout = new QVBoxLayout(stackPanel);
    auto* stackTools = new QHBoxLayout;
    stackMode_ = new QComboBox;
    stackMode_->setObjectName(QStringLiteral("layeredExportStackMode"));
    stackMode_->addItems({QStringLiteral("连续台阶实体"), QStringLiteral("分层爆炸视图")});
    automaticZScale_ = new QCheckBox(QStringLiteral("自动 Z 比例"));
    automaticZScale_->setObjectName(QStringLiteral("layeredExportStackAutoZ")); automaticZScale_->setChecked(true);
    stackZScale_ = new QDoubleSpinBox;
    stackZScale_->setObjectName(QStringLiteral("layeredExportStackZScale"));
    stackZScale_->setRange(1.0, 100.0); stackZScale_->setValue(1.0); stackZScale_->setEnabled(false);
    auto* fit = new QPushButton(QStringLiteral("复位"));
    auto* top = new QPushButton(QStringLiteral("俯视"));
    auto* front = new QPushButton(QStringLiteral("正视"));
    auto* right = new QPushButton(QStringLiteral("侧视"));
    stackTools->addWidget(stackMode_); stackTools->addWidget(automaticZScale_);
    stackTools->addWidget(stackZScale_); stackTools->addStretch();
    stackTools->addWidget(fit); stackTools->addWidget(top); stackTools->addWidget(front); stackTools->addWidget(right);
    stackLayout->addLayout(stackTools);
    stackView_ = new MaskStackWidget;
    stackView_->setObjectName(QStringLiteral("layeredExportStackView"));
    stackLayout->addWidget(stackView_, 1); splitter->addWidget(stackPanel);
    previewLayers_ = new QListWidget;
    previewLayers_->setObjectName(QStringLiteral("layeredExportPreviewLayers"));
    previewLayers_->setViewMode(QListView::IconMode); previewLayers_->setResizeMode(QListView::Adjust);
    previewLayers_->setMovement(QListView::Static); previewLayers_->setIconSize({128, 128});
    previewLayers_->setGridSize({154, 166}); splitter->addWidget(previewLayers_);
    splitter->setSizes({560, 560}); previewLayout->addWidget(splitter, 1);
    previewStatusLabel_ = new QLabel(QStringLiteral("等待生成预览"));
    previewStatusLabel_->setObjectName(QStringLiteral("layeredExportPreviewStatus"));
    previewLayout->addWidget(previewStatusLabel_);
    tabs_->addTab(previewPage, QStringLiteral("导出预览"));
    root->addWidget(tabs_, 1);

    auto* progressRow = new QHBoxLayout;
    statusLabel_ = new QLabel(QStringLiteral("就绪")); statusLabel_->setObjectName(QStringLiteral("layeredExportStatus"));
    progressBar_ = new QProgressBar; progressBar_->setObjectName(QStringLiteral("layeredExportProgress")); progressBar_->setTextVisible(false);
    percentLabel_ = new QLabel; percentLabel_->setObjectName(QStringLiteral("layeredExportPercent")); percentLabel_->setFixedWidth(48);
    cancelTaskButton_ = new QPushButton(QStringLiteral("取消任务")); cancelTaskButton_->setObjectName(QStringLiteral("layeredExportCancelTask"));
    progressRow->addWidget(statusLabel_); progressRow->addWidget(progressBar_, 1); progressRow->addWidget(percentLabel_); progressRow->addWidget(cancelTaskButton_);
    root->addLayout(progressRow);
    auto* actions = new QHBoxLayout; actions->addStretch();
    closeButton_ = new QPushButton(QStringLiteral("关闭")); exportButton_ = new QPushButton(QStringLiteral("导出"));
    exportButton_->setObjectName(QStringLiteral("layeredExportStart")); actions->addWidget(closeButton_); actions->addWidget(exportButton_); root->addLayout(actions);

    connect(directoryButton_, &QPushButton::clicked, this, &LayeredExportDialog::chooseDirectory);
    auto changed = [this] { updateValidation(); schedulePreview(); };
    connect(layerHeight_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double){changed();});
    connect(maximumHeight_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double){changed();});
    connect(layerStrategy_, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed](int){changed();});
    connect(outputWidth_, qOverload<int>(&QSpinBox::valueChanged), this, [this]{ updateAspectFromWidth(); schedulePreview(); });
    connect(outputHeight_, qOverload<int>(&QSpinBox::valueChanged), this, [this]{ updateAspectFromHeight(); schedulePreview(); });
    connect(lockAspect_, &QCheckBox::toggled, this, [this](bool checked){if(checked)updateAspectFromWidth();else updateValidation();schedulePreview();});
    connect(invertMask_, &QCheckBox::toggled, this, [changed](bool){changed();});
    connect(attachToCanvas_, &QCheckBox::toggled, this, [this, changed](bool checked){for(auto* widget:std::initializer_list<QWidget*>{canvasWidth_,canvasHeight_,rotationDegrees_,scaleX_,scaleY_,transformSampling_})widget->setEnabled(checked&&!tasks_->isBusy());changed();});
    for(auto* spin:{canvasWidth_,canvasHeight_})connect(spin,qOverload<int>(&QSpinBox::valueChanged),this,[changed](int){changed();});
    for(auto* spin:{rotationDegrees_,scaleX_,scaleY_})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[changed](double){changed();});
    connect(transformSampling_,qOverload<int>(&QComboBox::currentIndexChanged),this,[changed](int){changed();});
    connect(exportButton_, &QPushButton::clicked, this, &LayeredExportDialog::startPreflight);
    connect(cancelTaskButton_, &QPushButton::clicked, tasks_, &TaskManager::cancel);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(tasks_, &TaskManager::taskStarted, this, [this](quint64,const QString&){previewTasks_->cancel();setBusy(true);});
    connect(tasks_, &TaskManager::taskProgress, this, [this](quint64,const TaskProgress& value){setProgress(value);});
    connect(tasks_, &TaskManager::taskCancelled, this, [this](quint64){action_=Action::None;statusLabel_->setText(QStringLiteral("已取消"));setBusy(false);});
    connect(tasks_, &TaskManager::taskFailed, this, [this](quint64,const QString& error){action_=Action::None;statusLabel_->setText(QStringLiteral("失败"));setBusy(false);QMessageBox::critical(this,QStringLiteral("分层导出失败"),error);});
    connect(tasks_, &TaskManager::taskFinished, this, [this](quint64,const QVariant& value){const Action completed=action_;action_=Action::None;setBusy(false);if(completed==Action::Preflight)showPreflight(value.value<ExportPreflightResult>());else if(completed==Action::Export){statusLabel_->setText(QStringLiteral("导出完成"));QMessageBox::information(this,QStringLiteral("分层导出"),QStringLiteral("分层包已成功导出。"));accept();}});
    previewTimer_->setSingleShot(true); previewTimer_->setInterval(180);
    connect(previewTimer_,&QTimer::timeout,this,&LayeredExportDialog::startPreview);
    connect(previewTasks_,&TaskManager::taskProgress,this,[this](quint64,const TaskProgress& value){previewStatusLabel_->setText(value.phase);});
    connect(previewTasks_,&TaskManager::taskFailed,this,[this](quint64,const QString& error){previewStatusLabel_->setText(error);if(previewPending_){previewPending_=false;previewTimer_->start();}});
    connect(previewTasks_,&TaskManager::taskCancelled,this,[this](quint64){if(previewPending_){previewPending_=false;previewTimer_->start();}});
    connect(previewTasks_,&TaskManager::taskFinished,this,[this](quint64,const QVariant& value){const auto preview=value.value<LayeredMaskPreview>();if(preview.requestGeneration==previewRevision_)applyPreview(preview);else previewTimer_->start();});
    connect(previewLayers_,&QListWidget::currentRowChanged,this,&LayeredExportDialog::updatePreviewLayer);
    connect(stackMode_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){stackView_->setDisplayMode(index==0?MaskStackDisplayMode::Solid:MaskStackDisplayMode::Exploded);updatePreviewZScale();});
    connect(automaticZScale_,&QCheckBox::toggled,this,[this](bool automatic){stackZScale_->setEnabled(!automatic);updatePreviewZScale();});
    connect(stackZScale_,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](double value){if(!automaticZScale_->isChecked())stackView_->setZScale(value);});
    connect(fit,&QPushButton::clicked,stackView_,&MaskStackWidget::fitAll); connect(top,&QPushButton::clicked,stackView_,&MaskStackWidget::viewTop);
    connect(front,&QPushButton::clicked,stackView_,&MaskStackWidget::viewFront); connect(right,&QPushButton::clicked,stackView_,&MaskStackWidget::viewRight);
    setBusy(false); updateValidation();
}

void LayeredExportDialog::configure(const ImageDocumentPtr& document,const SegmentationResult& segmentation,TraversalMode traversalMode,double pixelScale,double maximumHeight,bool invertHeight,bool reverseY,const QString& initialDirectory,const QPoint& activeCell){document_=document;segmentation_=segmentation;traversalMode_=traversalMode;pixelScale_=pixelScale;invertHeight_=invertHeight;maximumHeight_->setValue(maximumHeight);outputWidth_->setValue(std::max(1,segmentation.settings.cellWidth));outputHeight_->setValue(std::max(1,segmentation.settings.cellHeight));layerHeight_->setValue(0.1);layerStrategy_->setCurrentIndex(0);lockAspect_->setChecked(true);invertMask_->setChecked(false);reverseY_->setChecked(reverseY);attachToCanvas_->setChecked(true);canvasWidth_->setValue(1920);canvasHeight_->setValue(1080);rotationDegrees_->setValue(0.0);scaleX_->setValue(1.0);scaleY_->setValue(1.0);transformSampling_->setCurrentIndex(0);const bool valid=activeCell.x()>=0&&activeCell.y()>=0&&activeCell.x()<segmentation.columns&&activeCell.y()<segmentation.rows;if(valid)previewCell_=activeCell;else{const auto order=SplitExporter::traversal(segmentation.rows,segmentation.columns,traversalMode);previewCell_=order.empty()?QPoint(-1,-1):order.front();}const qint64 index=SplitExporter::traversalIndex(segmentation.rows,segmentation.columns,traversalMode,previewCell_.y(),previewCell_.x());previewElementLabel_->setText(QStringLiteral("预览块：%1（行 %2，列 %3）").arg(index).arg(previewCell_.y()+1).arg(previewCell_.x()+1));setOutputDirectory(initialDirectory);statusLabel_->setText(QStringLiteral("就绪"));updateValidation();schedulePreview();}
void LayeredExportDialog::setElementGrouping(const ElementGroupingResult& grouping){grouping_=grouping.matches(document_,segmentation_)?grouping:ElementGroupingResult{};updateValidation();}
bool LayeredExportDialog::reverseY()const{return reverseY_->isChecked();} QString LayeredExportDialog::outputDirectory()const{return directory_;}
void LayeredExportDialog::setOutputDirectory(const QString& directory){directory_=directory.isEmpty()?QString():QFileInfo(directory).absoluteFilePath();directoryLabel_->setText(directory_.isEmpty()?QStringLiteral("未选择"):directory_);directoryLabel_->setToolTip(directory_);updateValidation();}
void LayeredExportDialog::chooseDirectory(){const QString selected=QFileDialog::getExistingDirectory(this,QStringLiteral("选择导出目录"),directory_);if(!selected.isEmpty())setOutputDirectory(selected);}
void LayeredExportDialog::updateAspectFromWidth(){if(updatingDimensions_)return;if(lockAspect_->isChecked()&&segmentation_.settings.cellWidth>0){updatingDimensions_=true;outputHeight_->setValue(static_cast<int>(std::clamp<qint64>(qRound64(double(outputWidth_->value())*segmentation_.settings.cellHeight/segmentation_.settings.cellWidth),1,INT_MAX)));updatingDimensions_=false;}updateValidation();}
void LayeredExportDialog::updateAspectFromHeight(){if(updatingDimensions_)return;if(lockAspect_->isChecked()&&segmentation_.settings.cellHeight>0){updatingDimensions_=true;outputWidth_->setValue(static_cast<int>(std::clamp<qint64>(qRound64(double(outputHeight_->value())*segmentation_.settings.cellWidth/segmentation_.settings.cellHeight),1,INT_MAX)));updatingDimensions_=false;}updateValidation();}
LayeredPackageRequest LayeredExportDialog::currentRequest()const{LayeredPackageOptions options{pixelScale_,maximumHeight_->value(),layerHeight_->value(),outputWidth_->value(),outputHeight_->value(),invertHeight_,invertMask_->isChecked(),reverseY_->isChecked(),layerStrategy_->currentData().value<LayerCountStrategy>()};options.attachToCanvas=attachToCanvas_->isChecked();options.canvasWidth=canvasWidth_->value();options.canvasHeight=canvasHeight_->value();options.rotationDegrees=options.attachToCanvas?rotationDegrees_->value():0.0;options.scaleX=options.attachToCanvas?scaleX_->value():1.0;options.scaleY=options.attachToCanvas?scaleY_->value():1.0;options.transformSampling=options.attachToCanvas?transformSampling_->currentData().value<BinaryTransformSampling>():BinaryTransformSampling::Nearest;return{directory_,document_,segmentation_,traversalMode_,options,true,grouping_};}
LayeredMaskRequest LayeredExportDialog::currentPreviewRequest()const{const auto package=currentRequest();const int layers=LayeredPackageExporter::layerCount(package.options);const ImageRegion region=document_?segmentationCellRegion(segmentation_,document_->metadata.width,document_->metadata.height,previewCell_.y(),previewCell_.x()):ImageRegion{};return{document_,region,{segmentation_.settings.cellWidth,segmentation_.settings.cellHeight},package.options,layers,previewRevision_};}
void LayeredExportDialog::updateValidation(){QString error;const auto request=currentRequest();const int layers=LayeredPackageExporter::layerCount(request.options,&error);const qint64 cells=qint64(segmentation_.rows)*segmentation_.columns;const qint64 groups=request.grouping.matches(document_,segmentation_)?request.grouping.groups.size():cells;if(error.isEmpty()){const bool valid=LayeredMaskProcessor::validate(currentPreviewRequest(),&error);Q_UNUSED(valid);}if(!document_||cells<=0)error=QStringLiteral("没有可导出的分割结果。");else if(directory_.isEmpty())error=QStringLiteral("请选择导出目录。");else if(!QFileInfo(directory_).isDir())error=QStringLiteral("导出目录不存在或不可用。");if(error.isEmpty()){const QSize size=LayeredMaskProcessor::outputSize(request.options);summaryLabel_->setText(QStringLiteral("%1 个分块归为 %2 组 × %3 层 = %4 个 1-bit BMP；输出 %5×%6；pixel2phy %7 μm/px").arg(cells).arg(groups).arg(layers).arg(groups*layers).arg(size.width()).arg(size.height()).arg(pixelScale_,0,'g',12));}else summaryLabel_->setText(error);exportButton_->setEnabled(!tasks_->isBusy()&&error.isEmpty());}
void LayeredExportDialog::schedulePreview(){if(!document_||tasks_->isBusy())return;++previewRevision_;if(previewTasks_->isBusy()){previewPending_=true;previewTasks_->cancel();return;}previewTimer_->start();}
void LayeredExportDialog::startPreview(){if(tasks_->isBusy())return;if(previewTasks_->isBusy()){previewPending_=true;previewTasks_->cancel();return;}const LayeredMaskRequest request=currentPreviewRequest();QString validation;if(!LayeredMaskProcessor::validate(request,&validation)){previewStatusLabel_->setText(validation);previewLayers_->clear();stackView_->clear();return;}previewStatusLabel_->setText(QStringLiteral("生成预览…"));previewTasks_->start(QStringLiteral("生成分层导出预览"),[request](const auto& token,const auto& progress,QString* error){return QVariant::fromValue(LayeredMaskProcessor::preview(request,token,progress,error));});}
void LayeredExportDialog::applyPreview(const LayeredMaskPreview& preview){if(!preview.stackPreview)return;previewLayers_->clear();for(int i=0;i<preview.thumbnails.size();++i)previewLayers_->addItem(new QListWidgetItem(QIcon(QPixmap::fromImage(preview.thumbnails[i])),QStringLiteral("%1.bmp").arg(i+1)));stackView_->setPreview(preview.stackPreview);updatePreviewZScale();if(previewLayers_->count()>0)previewLayers_->setCurrentRow(0);previewStatusLabel_->setText(QStringLiteral("预览就绪：内容 %1×%2，最终输出 %3×%4，共 %5 层").arg(preview.contentSize.width()).arg(preview.contentSize.height()).arg(preview.outputSize.width()).arg(preview.outputSize.height()).arg(preview.thumbnails.size()));}
void LayeredExportDialog::updatePreviewLayer(int row){stackView_->setSelectedLayer(row+1);}
void LayeredExportDialog::updatePreviewZScale(){double scale=stackZScale_->value();const auto preview=stackView_->preview();if(automaticZScale_->isChecked()&&preview){const double horizontal=std::max(preview->physicalSize.width(),preview->physicalSize.height());const double reconstruction=preview->layerCount*preview->layerHeight*(stackMode_->currentIndex()==0?1.0:1.2);scale=reconstruction>0.0?std::clamp(horizontal*0.35/reconstruction,1.0,100.0):1.0;const QSignalBlocker blocker(stackZScale_);stackZScale_->setValue(scale);}stackView_->setZScale(scale);}
void LayeredExportDialog::startPreflight(){if(tasks_->isBusy())return;const auto request=currentRequest();action_=Action::Preflight;statusLabel_->setText(QStringLiteral("准备预检"));if(!tasks_->start(QStringLiteral("分层导出预检"),[request](const auto& token,const auto& progress,QString*){return QVariant::fromValue(LayeredPackageExporter::inspect(request,token,progress));}))action_=Action::None;}
void LayeredExportDialog::showPreflight(const ExportPreflightResult& result){QMessageBox box(this);box.setObjectName(QStringLiteral("layeredExportPreflightDialog"));box.setWindowTitle(result.valid?QStringLiteral("确认分层导出"):QStringLiteral("导出预检未通过"));box.setIcon(result.valid?(result.conflictCount>0?QMessageBox::Warning:QMessageBox::Question):QMessageBox::Critical);box.setText(QStringLiteral("BMP 文件：%1 个\n命名：%2 — %3\n预计输出：%4").arg(result.bmpFileCount).arg(result.firstOutput,result.lastOutput).arg(formatStorageSize(result.estimatedTotalBytes)));QStringList details=result.warnings;details.prepend(QStringLiteral("所需空间（含安全余量）：%1").arg(formatStorageSize(result.requiredBytes)));details<<QStringLiteral("已有受管目标：%1 个").arg(result.conflictCount);if(!result.blockingError.isEmpty())details<<result.blockingError;box.setInformativeText(details.join('\n'));if(!result.conflictSamples.isEmpty())box.setDetailedText(result.conflictSamples.join('\n'));if(!result.valid){box.addButton(QMessageBox::Close);box.exec();return;}auto* start=box.addButton(QStringLiteral("开始导出"),QMessageBox::AcceptRole);box.addButton(QStringLiteral("取消"),QMessageBox::RejectRole);if(result.conflictCount>0){auto* overwrite=new QCheckBox(QStringLiteral("确认覆盖上述 %1 个受管目标").arg(result.conflictCount),&box);overwrite->setObjectName(QStringLiteral("layeredExportOverwriteCheck"));box.setCheckBox(overwrite);start->setEnabled(false);connect(overwrite,&QCheckBox::toggled,start,&QWidget::setEnabled);}box.exec();if(box.clickedButton()==start)startExport({result.conflictCount>0,true,result.targetFingerprint});}
void LayeredExportDialog::startExport(const ExportExecutionOptions& execution){const auto request=currentRequest();action_=Action::Export;statusLabel_->setText(QStringLiteral("准备导出"));if(!tasks_->start(QStringLiteral("导出分层包"),[request,execution](const auto& token,const auto& progress,QString* error){return LayeredPackageExporter::exportPackage(request,execution,token,progress,error)?QVariant(true):QVariant{};}))action_=Action::None;}
void LayeredExportDialog::setProgress(const TaskProgress& progress){statusLabel_->setText(progress.phase);if(progress.total<=0){progressBar_->setRange(0,0);percentLabel_->setText(QStringLiteral("--"));return;}const double ratio=std::clamp(double(progress.completed)/progress.total,0.0,1.0);progressBar_->setRange(0,1000);progressBar_->setValue(static_cast<int>(std::lround(ratio*1000.0)));percentLabel_->setText(QStringLiteral("%1%").arg(qRound(ratio*100.0)));}
void LayeredExportDialog::setBusy(bool busy){directoryButton_->setEnabled(!busy);for(auto* widget:std::initializer_list<QWidget*>{layerHeight_,maximumHeight_,layerStrategy_,outputWidth_,outputHeight_,lockAspect_,invertMask_,reverseY_,attachToCanvas_})widget->setEnabled(!busy);const bool transformEnabled=!busy&&attachToCanvas_->isChecked();for(auto* widget:std::initializer_list<QWidget*>{canvasWidth_,canvasHeight_,rotationDegrees_,scaleX_,scaleY_,transformSampling_})widget->setEnabled(transformEnabled);closeButton_->setEnabled(!busy);cancelTaskButton_->setEnabled(busy);progressBar_->setVisible(busy);percentLabel_->setVisible(busy);updateValidation();}
void LayeredExportDialog::closeEvent(QCloseEvent* event){if(tasks_->isBusy()){tasks_->cancel();event->ignore();return;}previewTasks_->cancel();event->accept();}
