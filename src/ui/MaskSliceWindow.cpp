#include "ui/MaskSliceWindow.h"

#include "imaging/MaskSliceGenerator.h"
#include "io/MaskSliceExporter.h"
#include "tasks/TaskManager.h"
#include "ui/MaskStackWidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

MaskSliceWindow::MaskSliceWindow(QWidget* parent)
    : QWidget(parent, Qt::Window), tasks_(new TaskManager(this)),
      analysisTimer_(new QTimer(this)) {
    setObjectName(QStringLiteral("maskSliceWindow"));
    setWindowTitle(QStringLiteral("单元分层切片"));
    resize(1280, 760);

    auto* root = new QVBoxLayout(this);
    elementLabel_ = new QLabel(QStringLiteral("未选择单元"));
    elementLabel_->setObjectName(QStringLiteral("maskSliceElementInfo"));
    elementLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(elementLabel_);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName(QStringLiteral("maskSliceSplitter"));
    auto* stackPanel = new QWidget;
    auto* stackLayout = new QVBoxLayout(stackPanel);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    auto* stackTools = new QHBoxLayout;
    stackMode_ = new QComboBox;
    stackMode_->setObjectName(QStringLiteral("maskStackMode"));
    stackMode_->addItem(QStringLiteral("连续台阶实体"));
    stackMode_->addItem(QStringLiteral("分层爆炸视图"));
    automaticZScale_ = new QCheckBox(QStringLiteral("自动 Z 比例"));
    automaticZScale_->setObjectName(QStringLiteral("maskStackAutoZ"));
    automaticZScale_->setChecked(true);
    stackZScale_ = new QDoubleSpinBox;
    stackZScale_->setObjectName(QStringLiteral("maskStackZScale"));
    stackZScale_->setDecimals(2);
    stackZScale_->setRange(1.0, 100.0);
    stackZScale_->setSingleStep(0.5);
    stackZScale_->setValue(1.0);
    stackZScale_->setSuffix(QStringLiteral("×"));
    stackZScale_->setEnabled(false);
    auto* fitStack = new QPushButton(QStringLiteral("复位"));
    fitStack->setObjectName(QStringLiteral("maskStackFit"));
    auto* topStack = new QPushButton(QStringLiteral("俯视"));
    topStack->setObjectName(QStringLiteral("maskStackTop"));
    auto* frontStack = new QPushButton(QStringLiteral("正视"));
    frontStack->setObjectName(QStringLiteral("maskStackFront"));
    auto* rightStack = new QPushButton(QStringLiteral("侧视"));
    rightStack->setObjectName(QStringLiteral("maskStackRight"));
    stackTools->addWidget(stackMode_);
    stackTools->addWidget(automaticZScale_);
    stackTools->addWidget(stackZScale_);
    stackTools->addStretch();
    stackTools->addWidget(fitStack);
    stackTools->addWidget(topStack);
    stackTools->addWidget(frontStack);
    stackTools->addWidget(rightStack);
    stackLayout->addLayout(stackTools);
    stackView_ = new MaskStackWidget;
    stackLayout->addWidget(stackView_, 1);
    stackLayerLabel_ = new QLabel(QStringLiteral("当前层：--"));
    stackLayerLabel_->setObjectName(QStringLiteral("maskStackLayerInfo"));
    stackSamplingLabel_ = new QLabel(QStringLiteral("三维预览：--"));
    stackSamplingLabel_->setObjectName(QStringLiteral("maskStackSamplingInfo"));
    stackSamplingLabel_->setWordWrap(true);
    stackLayout->addWidget(stackLayerLabel_);
    stackLayout->addWidget(stackSamplingLabel_);
    splitter->addWidget(stackPanel);

    auto* rightPanel = new QWidget;
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    layerHeight_ = new QDoubleSpinBox;
    layerHeight_->setObjectName(QStringLiteral("maskSliceLayerHeight"));
    layerHeight_->setDecimals(6);
    layerHeight_->setRange(0.000001, 1000000.0);
    layerHeight_->setSingleStep(0.1);
    layerHeight_->setValue(0.1);
    layerHeight_->setSuffix(QStringLiteral(" μm"));
    outputWidth_ = new QSpinBox;
    outputWidth_->setObjectName(QStringLiteral("maskSliceOutputWidth"));
    outputWidth_->setRange(1, INT_MAX);
    outputWidth_->setSuffix(QStringLiteral(" px"));
    outputHeight_ = new QSpinBox;
    outputHeight_->setObjectName(QStringLiteral("maskSliceOutputHeight"));
    outputHeight_->setRange(1, INT_MAX);
    outputHeight_->setSuffix(QStringLiteral(" px"));
    lockAspect_ = new QCheckBox(QStringLiteral("锁定原始宽高比"));
    lockAspect_->setObjectName(QStringLiteral("maskSliceLockAspect"));
    lockAspect_->setChecked(true);
    invertMask_ = new QCheckBox(QStringLiteral("黑白反转"));
    invertMask_->setObjectName(QStringLiteral("maskSliceInvertMask"));
    mappingLabel_ = new QLabel(QStringLiteral("--"));
    mappingLabel_->setObjectName(QStringLiteral("maskSliceMappingInfo"));
    mappingLabel_->setWordWrap(true);
    mappingLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("层切高"), layerHeight_);
    form->addRow(QStringLiteral("输出宽度"), outputWidth_);
    form->addRow(QStringLiteral("输出高度"), outputHeight_);
    form->addRow(lockAspect_);
    form->addRow(invertMask_);
    form->addRow(QStringLiteral("尺寸映射"), mappingLabel_);
    rightLayout->addLayout(form);

    auto* listHeader = new QHBoxLayout;
    listHeader->addWidget(new QLabel(QStringLiteral("掩模预览")));
    listHeader->addStretch();
    totalLabel_ = new QLabel(QStringLiteral("掩模总数：0"));
    totalLabel_->setObjectName(QStringLiteral("maskSliceTotal"));
    listHeader->addWidget(totalLabel_);
    rightLayout->addLayout(listHeader);
    layerList_ = new QListWidget;
    layerList_->setObjectName(QStringLiteral("maskSliceList"));
    layerList_->setViewMode(QListView::IconMode);
    layerList_->setResizeMode(QListView::Adjust);
    layerList_->setMovement(QListView::Static);
    layerList_->setIconSize(QSize(128, 128));
    layerList_->setGridSize(QSize(154, 166));
    rightLayout->addWidget(layerList_, 1);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 45);
    splitter->setStretchFactor(1, 55);
    splitter->setSizes({570, 690});
    root->addWidget(splitter, 1);

    auto* actions = new QHBoxLayout;
    statusLabel_ = new QLabel(QStringLiteral("就绪"));
    statusLabel_->setObjectName(QStringLiteral("maskSliceStatus"));
    progressBar_ = new QProgressBar;
    progressBar_->setObjectName(QStringLiteral("maskSliceProgress"));
    progressBar_->setTextVisible(false);
    percentLabel_ = new QLabel;
    percentLabel_->setObjectName(QStringLiteral("maskSlicePercent"));
    percentLabel_->setFixedWidth(48);
    percentLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cancelButton_ = new QPushButton(QStringLiteral("取消任务"));
    cancelButton_->setObjectName(QStringLiteral("maskSliceCancel"));
    exportButton_ = new QPushButton(QStringLiteral("导出全部掩模"));
    exportButton_->setObjectName(QStringLiteral("maskSliceExport"));
    actions->addWidget(statusLabel_);
    actions->addWidget(progressBar_, 1);
    actions->addWidget(percentLabel_);
    actions->addWidget(cancelButton_);
    actions->addWidget(exportButton_);
    root->addLayout(actions);
    progressBar_->hide();
    percentLabel_->hide();

    analysisTimer_->setSingleShot(true);
    analysisTimer_->setInterval(150);
    connect(analysisTimer_, &QTimer::timeout, this, &MaskSliceWindow::startAnalysis);
    connect(layerHeight_, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] { scheduleAnalysis(); });
    connect(invertMask_, &QCheckBox::toggled, this, [this] { scheduleAnalysis(); });
    connect(outputWidth_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int width) {
        if (!updatingDimensions_ && lockAspect_->isChecked() && region_.isValid()) {
            updatingDimensions_ = true;
            const qint64 height = std::clamp<qint64>(
                qRound64(double(width) * region_.height / region_.width), 1, INT_MAX);
            outputHeight_->setValue(static_cast<int>(height));
            updatingDimensions_ = false;
        }
        updateDimensionMapping();
        scheduleAnalysis();
    });
    connect(outputHeight_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int height) {
        if (!updatingDimensions_ && lockAspect_->isChecked() && region_.isValid()) {
            updatingDimensions_ = true;
            const qint64 width = std::clamp<qint64>(
                qRound64(double(height) * region_.width / region_.height), 1, INT_MAX);
            outputWidth_->setValue(static_cast<int>(width));
            updatingDimensions_ = false;
        }
        updateDimensionMapping();
        scheduleAnalysis();
    });
    connect(lockAspect_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && region_.isValid()) {
            updatingDimensions_ = true;
            const qint64 height = std::clamp<qint64>(
                qRound64(double(outputWidth_->value()) * region_.height / region_.width),
                1, INT_MAX);
            outputHeight_->setValue(static_cast<int>(height));
            updatingDimensions_ = false;
        }
        updateDimensionMapping();
        scheduleAnalysis();
    });
    connect(stackMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
        stackView_->setDisplayMode(index == 0 ? MaskStackDisplayMode::Solid
                                               : MaskStackDisplayMode::Exploded);
        updateStackZScale();
        updateStackInformation();
    });
    connect(automaticZScale_, &QCheckBox::toggled, this, [this](bool automatic) {
        stackZScale_->setEnabled(!automatic);
        updateStackZScale();
    });
    connect(stackZScale_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this] { if (!automaticZScale_->isChecked()) updateStackZScale(); });
    connect(layerList_, &QListWidget::currentRowChanged,
            this, &MaskSliceWindow::updateStackLayerSelection);
    connect(fitStack, &QPushButton::clicked, stackView_, &MaskStackWidget::fitAll);
    connect(topStack, &QPushButton::clicked, stackView_, &MaskStackWidget::viewTop);
    connect(frontStack, &QPushButton::clicked, stackView_, &MaskStackWidget::viewFront);
    connect(rightStack, &QPushButton::clicked, stackView_, &MaskStackWidget::viewRight);
    connect(exportButton_, &QPushButton::clicked, this,
            &MaskSliceWindow::chooseExportDirectory);
    connect(cancelButton_, &QPushButton::clicked, tasks_, &TaskManager::cancel);
    connect(tasks_, &TaskManager::taskStarted, this, [this](quint64, const QString&) {
        progressBar_->setRange(0, 0);
        progressBar_->show();
        percentLabel_->setText(QStringLiteral("--"));
        percentLabel_->show();
        updateControls();
    });
    connect(tasks_, &TaskManager::taskProgress, this,
            [this](quint64, const TaskProgress& value) { setProgress(value); });
    connect(tasks_, &TaskManager::taskFinished, this,
            [this](quint64, const QVariant& value) {
        const Action completedAction = action_;
        action_ = Action::None;
        progressBar_->hide();
        percentLabel_->hide();
        if (clearing_) { updateControls(); return; }
        if (completedAction == Action::Analyze) {
            const MaskSliceAnalysis result = value.value<MaskSliceAnalysis>();
            if (!pendingAnalysis_ && result.layerCount > 0) {
                analysis_ = result;
                populateLayers(result);
                statusLabel_->setText(QStringLiteral("生成完成，实际最高 %1 μm")
                    .arg(result.actualMaximumHeight, 0, 'f', 6));
            }
        } else if (completedAction == Action::Export) {
            statusLabel_->setText(QStringLiteral("导出完成"));
            QMessageBox::information(this, QStringLiteral("导出掩模"),
                                     QStringLiteral("全部掩模已成功导出。"));
        }
        updateControls();
        if (pendingAnalysis_) analysisTimer_->start(0);
    });
    connect(tasks_, &TaskManager::taskCancelled, this, [this](quint64) {
        action_ = Action::None;
        progressBar_->hide();
        percentLabel_->hide();
        if (!clearing_) statusLabel_->setText(QStringLiteral("已取消"));
        updateControls();
        if (pendingAnalysis_ && !clearing_) analysisTimer_->start(0);
    });
    connect(tasks_, &TaskManager::taskFailed, this,
            [this](quint64, const QString& error) {
        action_ = Action::None;
        progressBar_->hide();
        percentLabel_->hide();
        if (!clearing_) {
            statusLabel_->setText(QStringLiteral("失败：%1").arg(error));
            totalLabel_->setText(QStringLiteral("掩模总数：0"));
        }
        updateControls();
        if (pendingAnalysis_ && !clearing_) analysisTimer_->start(0);
    });
    updateControls();
}

void MaskSliceWindow::showElement(
    const ImageDocumentPtr& source, const ImageRegion& region, int row, int column,
    qint64 traversalIndex, const PointCloudSettings& mapping) {
    clearing_ = false;
    source_ = source;
    region_ = region;
    mapping_ = mapping;
    row_ = row;
    column_ = column;
    traversalIndex_ = traversalIndex;
    analysis_.reset();
    layerList_->clear();
    totalLabel_->setText(QStringLiteral("掩模总数：0"));
    elementLabel_->setText(QStringLiteral("第 %1 行，第 %2 列 | %3.bmp | %4 × %5 像素 | %6 × %7 μm")
        .arg(row + 1).arg(column + 1).arg(traversalIndex + 1)
        .arg(region.width).arg(region.height)
        .arg(region.width * mapping.pixelScale, 0, 'f', 6)
        .arg(region.height * mapping.pixelScale, 0, 'f', 6));
    setWindowTitle(QStringLiteral("单元分层切片 - %1.bmp").arg(traversalIndex + 1));
    updatingDimensions_ = true;
    outputWidth_->setValue(static_cast<int>(std::clamp<qint64>(region.width, 1, INT_MAX)));
    outputHeight_->setValue(static_cast<int>(std::clamp<qint64>(region.height, 1, INT_MAX)));
    updatingDimensions_ = false;
    updateDimensionMapping();
    show();
    raise();
    activateWindow();
    scheduleAnalysis();
}

void MaskSliceWindow::setPointCloudSettings(const PointCloudSettings& mapping) {
    const bool geometryChanged = mapping_.pixelScale != mapping.pixelScale ||
        mapping_.maximumHeight != mapping.maximumHeight ||
        mapping_.invertHeight != mapping.invertHeight;
    const bool colorsChanged = mapping_.startColor != mapping.startColor ||
                               mapping_.endColor != mapping.endColor;
    if (!geometryChanged && !colorsChanged) return;
    mapping_ = mapping;
    if (colorsChanged) stackView_->setColors(mapping_.startColor, mapping_.endColor);
    if (source_ && region_.isValid()) {
        elementLabel_->setText(QStringLiteral("第 %1 行，第 %2 列 | %3.bmp | %4 × %5 像素 | %6 × %7 μm")
            .arg(row_ + 1).arg(column_ + 1).arg(traversalIndex_ + 1)
            .arg(region_.width).arg(region_.height)
            .arg(region_.width * mapping_.pixelScale, 0, 'f', 6)
            .arg(region_.height * mapping_.pixelScale, 0, 'f', 6));
        updateDimensionMapping();
        if (geometryChanged) scheduleAnalysis();
    }
}

MaskSliceRequest MaskSliceWindow::currentRequest() const {
    return {source_, region_, mapping_,
            {layerHeight_->value(), outputWidth_->value(), outputHeight_->value(),
             invertMask_->isChecked()}};
}

void MaskSliceWindow::scheduleAnalysis() {
    if (!source_ || !region_.isValid() || clearing_) return;
    pendingAnalysis_ = true;
    analysis_.reset();
    layerList_->clear();
    stackView_->clear();
    stackLayerLabel_->setText(QStringLiteral("当前层：--"));
    stackSamplingLabel_->setText(QStringLiteral("三维预览：正在重新生成"));
    exportButton_->setEnabled(false);
    totalLabel_->setText(QStringLiteral("掩模总数：0"));
    if (tasks_->isBusy()) {
        if (action_ == Action::Analyze) tasks_->cancel();
        return;
    }
    analysisTimer_->start();
}

void MaskSliceWindow::startAnalysis() {
    if (!pendingAnalysis_ || tasks_->isBusy() || !source_ || !region_.isValid()) return;
    pendingAnalysis_ = false;
    const MaskSliceRequest request = currentRequest();
    action_ = Action::Analyze;
    analysis_.reset();
    layerList_->clear();
    statusLabel_->setText(QStringLiteral("准备生成掩模"));
    if (!tasks_->start(QStringLiteral("生成分层掩模"),
        [request](const auto& token, const auto& progress, QString* error) {
            return QVariant::fromValue(MaskSliceGenerator::analyze(
                request, token, progress, error));
        })) {
        action_ = Action::None;
        pendingAnalysis_ = true;
    }
}

void MaskSliceWindow::populateLayers(const MaskSliceAnalysis& analysis) {
    layerList_->clear();
    for (int index = 0; index < analysis.thumbnails.size(); ++index) {
        auto* item = new QListWidgetItem(
            QIcon(QPixmap::fromImage(analysis.thumbnails[index])),
            QString::number(index + 1) + QStringLiteral(".bmp"), layerList_);
        item->setTextAlignment(Qt::AlignHCenter);
    }
    totalLabel_->setText(QStringLiteral("掩模总数：%1").arg(analysis.layerCount));
    stackView_->setColors(mapping_.startColor, mapping_.endColor);
    stackView_->setPreview(analysis.stackPreview);
    if (layerList_->count() > 0) layerList_->setCurrentRow(0);
    updateStackZScale();
    updateStackInformation();
}

void MaskSliceWindow::updateStackLayerSelection(int row) {
    if (!analysis_ || row < 0 || row >= analysis_->layerCount) {
        stackLayerLabel_->setText(QStringLiteral("当前层：--"));
        return;
    }
    const int layer = row + 1;
    const double lower = row * layerHeight_->value();
    const double upper = layer * layerHeight_->value();
    stackView_->setSelectedLayer(layer);
    stackLayerLabel_->setText(QStringLiteral("当前第 %1 层 | 判断高度 > %2 μm | 重建区间 (%2, %3] μm")
        .arg(layer).arg(lower, 0, 'f', 6).arg(upper, 0, 'f', 6));
}

void MaskSliceWindow::updateStackZScale() {
    double scale = stackZScale_->value();
    if (automaticZScale_->isChecked() && analysis_ && analysis_->stackPreview) {
        const auto& preview = *analysis_->stackPreview;
        const double horizontal = std::max(preview.physicalSize.width(),
                                           preview.physicalSize.height());
        const double reconstruction = preview.layerCount * preview.layerHeight *
            (stackMode_->currentIndex() == 0 ? 1.0 : 1.2);
        scale = reconstruction > 0.0
            ? std::clamp(horizontal * 0.35 / reconstruction, 1.0, 100.0) : 1.0;
        const QSignalBlocker blocker(stackZScale_);
        stackZScale_->setValue(scale);
    }
    stackView_->setZScale(scale);
    updateStackInformation();
}

void MaskSliceWindow::updateStackInformation() {
    if (!analysis_ || !analysis_->stackPreview) {
        stackSamplingLabel_->setText(QStringLiteral("三维预览：--"));
        return;
    }
    const auto& preview = *analysis_->stackPreview;
    const QString sampling = preview.downsampled
        ? QStringLiteral("（由 %1 × %2 降采样）")
              .arg(preview.requestedOutputSize.width())
              .arg(preview.requestedOutputSize.height())
        : QStringLiteral("（与输出尺寸一致）");
    const int stride = stackMode_->currentIndex() == 1 && preview.explodedGeometry
        ? preview.explodedGeometry->layerStride : 1;
    const QString layerSampling = stride > 1
        ? QStringLiteral("；爆炸视图每 %1 层显示一层").arg(stride) : QString();
    stackSamplingLabel_->setText(
        QStringLiteral("三维预览 %1 × %2 %3 | X %4 μm，Y %5 μm | 重建高度 %6 μm，实际最高 %7 μm | Z %8×%9")
            .arg(preview.gridSize.width()).arg(preview.gridSize.height()).arg(sampling)
            .arg(preview.physicalSize.width(),0,'f',6)
            .arg(preview.physicalSize.height(),0,'f',6)
            .arg(preview.layerCount*preview.layerHeight,0,'f',6)
            .arg(analysis_->actualMaximumHeight,0,'f',6)
            .arg(stackZScale_->value(),0,'f',2).arg(layerSampling));
}

void MaskSliceWindow::updateDimensionMapping() {
    if (!region_.isValid() || outputWidth_->value() <= 0 || outputHeight_->value() <= 0) {
        mappingLabel_->setText(QStringLiteral("--"));
        return;
    }
    const double scaleX = double(outputWidth_->value()) / region_.width;
    const double scaleY = double(outputHeight_->value()) / region_.height;
    const double pitchX = region_.width * mapping_.pixelScale / outputWidth_->value();
    const double pitchY = region_.height * mapping_.pixelScale / outputHeight_->value();
    mappingLabel_->setText(QStringLiteral("缩放 X %1，Y %2；输出像素间距 X %3 μm，Y %4 μm；最大高度 %5 μm%6")
        .arg(scaleX, 0, 'f', 6).arg(scaleY, 0, 'f', 6)
        .arg(pitchX, 0, 'f', 6).arg(pitchY, 0, 'f', 6)
        .arg(mapping_.maximumHeight, 0, 'f', 6)
        .arg(mapping_.invertHeight ? QStringLiteral("（倒置）") : QString()));
}

void MaskSliceWindow::setProgress(const TaskProgress& progress) {
    statusLabel_->setText(progress.phase);
    if (progress.total <= 0) {
        progressBar_->setRange(0, 0);
        percentLabel_->setText(QStringLiteral("--"));
        return;
    }
    const double ratio = std::clamp(double(progress.completed) / progress.total, 0.0, 1.0);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(static_cast<int>(std::lround(ratio * 1000.0)));
    percentLabel_->setText(QStringLiteral("%1%").arg(static_cast<int>(std::lround(ratio * 100.0))));
}

void MaskSliceWindow::updateControls() {
    const bool busy = tasks_->isBusy();
    const bool exporting = busy && action_ == Action::Export;
    layerHeight_->setEnabled(!exporting);
    outputWidth_->setEnabled(!exporting);
    outputHeight_->setEnabled(!exporting);
    lockAspect_->setEnabled(!exporting);
    invertMask_->setEnabled(!exporting);
    cancelButton_->setEnabled(busy);
    exportButton_->setEnabled(!busy && analysis_ && analysis_->layerCount > 0 && !pendingAnalysis_);
}

int MaskSliceWindow::conflictCount(const QString& directory) const {
    if (!analysis_) return 0;
    QDir output(directory);
    int count = 0;
    for (int layer = 1; layer <= analysis_->layerCount; ++layer) {
        const QFileInfo info(output.filePath(QString::number(layer) + QStringLiteral(".bmp")));
        if (info.exists() || info.isSymLink()) ++count;
    }
    return count;
}

void MaskSliceWindow::chooseExportDirectory() {
    if (!analysis_ || analysis_->layerCount <= 0 || tasks_->isBusy()) return;
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择掩模输出目录"));
    if (directory.isEmpty()) return;
    const int conflicts = conflictCount(directory);
    bool overwrite = false;
    if (conflicts > 0) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("同名掩模确认"),
            QStringLiteral("输出目录中已有 %1 个本次同名 BMP。是否替换这些文件？\n其他文件不会被修改。")
                .arg(conflicts),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        overwrite = true;
    }
    startExport(directory, overwrite);
}

void MaskSliceWindow::startExport(const QString& directory, bool overwrite) {
    if (!analysis_ || tasks_->isBusy()) return;
    const MaskSliceRequest request = currentRequest();
    const int layers = analysis_->layerCount;
    action_ = Action::Export;
    statusLabel_->setText(QStringLiteral("准备导出"));
    if (!tasks_->start(QStringLiteral("导出分层掩模"),
        [directory, request, layers, overwrite](const auto& token, const auto& progress,
                                                QString* error) {
            return MaskSliceExporter::exportAll(directory, request, layers, overwrite,
                                                token, progress, error)
                       ? QVariant(true) : QVariant{};
        })) {
        action_ = Action::None;
    }
}

void MaskSliceWindow::cancelAndClear() {
    clearing_ = true;
    analysisTimer_->stop();
    pendingAnalysis_ = false;
    tasks_->cancel();
    source_.reset();
    region_ = {};
    analysis_.reset();
    layerList_->clear();
    stackView_->clear();
    stackLayerLabel_->setText(QStringLiteral("当前层：--"));
    stackSamplingLabel_->setText(QStringLiteral("三维预览：--"));
    totalLabel_->setText(QStringLiteral("掩模总数：0"));
    elementLabel_->setText(QStringLiteral("未选择单元"));
    statusLabel_->setText(QStringLiteral("已清空"));
    hide();
}

void MaskSliceWindow::closeEvent(QCloseEvent* event) {
    cancelAndClear();
    event->accept();
}
