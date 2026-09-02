#include "ui/LayeredExportDialog.h"

#include "domain/StorageSizeFormatter.h"
#include "io/LayeredPackageExporter.h"
#include "tasks/TaskManager.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

LayeredExportDialog::LayeredExportDialog(QWidget* parent)
    : QDialog(parent), tasks_(new TaskManager(this)) {
    setObjectName(QStringLiteral("layeredExportDialog"));
    setWindowTitle(QStringLiteral("分层导出设置"));
    setModal(true);
    resize(620, 390);

    auto* root = new QVBoxLayout(this);
    auto* directoryRow = new QHBoxLayout;
    directoryButton_ = new QPushButton(QStringLiteral("选择导出目录"));
    directoryButton_->setObjectName(QStringLiteral("layeredExportChooseDirectory"));
    directoryLabel_ = new QLabel(QStringLiteral("未选择"));
    directoryLabel_->setObjectName(QStringLiteral("layeredExportDirectory"));
    directoryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    directoryLabel_->setMinimumWidth(300);
    directoryRow->addWidget(directoryButton_);
    directoryRow->addWidget(directoryLabel_, 1);
    root->addLayout(directoryRow);

    auto* form = new QFormLayout;
    layerHeight_ = new QDoubleSpinBox;
    layerHeight_->setObjectName(QStringLiteral("layeredExportLayerHeight"));
    layerHeight_->setDecimals(6);
    layerHeight_->setRange(0.000001, 1000000.0);
    layerHeight_->setSingleStep(0.1);
    layerHeight_->setValue(0.1);
    layerHeight_->setSuffix(QStringLiteral(" μm"));
    maximumHeight_ = new QDoubleSpinBox;
    maximumHeight_->setObjectName(QStringLiteral("layeredExportMaximumHeight"));
    maximumHeight_->setDecimals(6);
    maximumHeight_->setRange(0.000001, 1000000.0);
    maximumHeight_->setSingleStep(0.5);
    maximumHeight_->setSuffix(QStringLiteral(" μm"));
    layerStrategy_ = new QComboBox;
    layerStrategy_->setObjectName(QStringLiteral("layeredExportLayerStrategy"));
    layerStrategy_->addItem(QStringLiteral("向上取整"),
                            QVariant::fromValue(LayerCountStrategy::RoundUp));
    layerStrategy_->addItem(QStringLiteral("必须整除"),
                            QVariant::fromValue(LayerCountStrategy::RequireExact));
    outputWidth_ = new QSpinBox;
    outputWidth_->setObjectName(QStringLiteral("layeredExportOutputWidth"));
    outputWidth_->setRange(1, INT_MAX);
    outputWidth_->setSuffix(QStringLiteral(" px"));
    outputHeight_ = new QSpinBox;
    outputHeight_->setObjectName(QStringLiteral("layeredExportOutputHeight"));
    outputHeight_->setRange(1, INT_MAX);
    outputHeight_->setSuffix(QStringLiteral(" px"));
    lockAspect_ = new QCheckBox(QStringLiteral("锁定原始宽高比"));
    lockAspect_->setObjectName(QStringLiteral("layeredExportLockAspect"));
    lockAspect_->setChecked(true);
    invertMask_ = new QCheckBox(QStringLiteral("黑白反转"));
    invertMask_->setObjectName(QStringLiteral("layeredExportInvertMask"));
    reverseY_ = new QCheckBox(QStringLiteral("Y方向相反"));
    reverseY_->setObjectName(QStringLiteral("layeredExportReverseY"));
    form->addRow(QStringLiteral("层切高"), layerHeight_);
    form->addRow(QStringLiteral("最大高度"), maximumHeight_);
    form->addRow(QStringLiteral("层数策略"), layerStrategy_);
    form->addRow(QStringLiteral("标准块输出宽度"), outputWidth_);
    form->addRow(QStringLiteral("标准块输出高度"), outputHeight_);
    form->addRow(lockAspect_);
    form->addRow(invertMask_);
    form->addRow(reverseY_);
    root->addLayout(form);

    summaryLabel_ = new QLabel;
    summaryLabel_->setObjectName(QStringLiteral("layeredExportSummary"));
    summaryLabel_->setWordWrap(true);
    root->addWidget(summaryLabel_);
    auto* progressRow = new QHBoxLayout;
    statusLabel_ = new QLabel(QStringLiteral("就绪"));
    statusLabel_->setObjectName(QStringLiteral("layeredExportStatus"));
    progressBar_ = new QProgressBar;
    progressBar_->setObjectName(QStringLiteral("layeredExportProgress"));
    progressBar_->setTextVisible(false);
    percentLabel_ = new QLabel;
    percentLabel_->setObjectName(QStringLiteral("layeredExportPercent"));
    percentLabel_->setFixedWidth(48);
    cancelTaskButton_ = new QPushButton(QStringLiteral("取消任务"));
    cancelTaskButton_->setObjectName(QStringLiteral("layeredExportCancelTask"));
    progressRow->addWidget(statusLabel_);
    progressRow->addWidget(progressBar_, 1);
    progressRow->addWidget(percentLabel_);
    progressRow->addWidget(cancelTaskButton_);
    root->addLayout(progressRow);
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    closeButton_ = new QPushButton(QStringLiteral("关闭"));
    exportButton_ = new QPushButton(QStringLiteral("导出"));
    exportButton_->setObjectName(QStringLiteral("layeredExportStart"));
    actions->addWidget(closeButton_);
    actions->addWidget(exportButton_);
    root->addLayout(actions);

    connect(directoryButton_, &QPushButton::clicked, this,
            &LayeredExportDialog::chooseDirectory);
    connect(layerHeight_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &LayeredExportDialog::updateValidation);
    connect(maximumHeight_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &LayeredExportDialog::updateValidation);
    connect(layerStrategy_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &LayeredExportDialog::updateValidation);
    connect(outputWidth_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { updateAspectFromWidth(); });
    connect(outputHeight_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { updateAspectFromHeight(); });
    connect(lockAspect_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) updateAspectFromWidth();
        else updateValidation();
    });
    connect(exportButton_, &QPushButton::clicked, this,
            &LayeredExportDialog::startPreflight);
    connect(cancelTaskButton_, &QPushButton::clicked, tasks_, &TaskManager::cancel);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(tasks_, &TaskManager::taskStarted, this,
            [this](quint64, const QString&) { setBusy(true); });
    connect(tasks_, &TaskManager::taskProgress, this,
            [this](quint64, const TaskProgress& value) { setProgress(value); });
    connect(tasks_, &TaskManager::taskCancelled, this, [this](quint64) {
        action_ = Action::None;
        statusLabel_->setText(QStringLiteral("已取消"));
        setBusy(false);
    });
    connect(tasks_, &TaskManager::taskFailed, this,
            [this](quint64, const QString& error) {
        action_ = Action::None;
        statusLabel_->setText(QStringLiteral("失败"));
        setBusy(false);
        QMessageBox::critical(this, QStringLiteral("分层导出失败"), error);
    });
    connect(tasks_, &TaskManager::taskFinished, this,
            [this](quint64, const QVariant& value) {
        const Action completed = action_;
        action_ = Action::None;
        setBusy(false);
        if (completed == Action::Preflight) {
            showPreflight(value.value<ExportPreflightResult>());
        } else if (completed == Action::Export) {
            statusLabel_->setText(QStringLiteral("导出完成"));
            QMessageBox::information(this, QStringLiteral("分层导出"),
                                     QStringLiteral("分层包已成功导出。"));
            accept();
        }
    });
    setBusy(false);
    updateValidation();
}

void LayeredExportDialog::configure(
    const ImageDocumentPtr& document, const SegmentationResult& segmentation,
    TraversalMode traversalMode, double pixelScale, double maximumHeight,
    bool invertHeight, bool reverseY, const QString& initialDirectory) {
    document_ = document;
    segmentation_ = segmentation;
    traversalMode_ = traversalMode;
    pixelScale_ = pixelScale;
    invertHeight_ = invertHeight;
    maximumHeight_->setValue(maximumHeight);
    outputWidth_->setValue(std::max(1, segmentation.settings.cellWidth));
    outputHeight_->setValue(std::max(1, segmentation.settings.cellHeight));
    layerHeight_->setValue(0.1);
    layerStrategy_->setCurrentIndex(0);
    lockAspect_->setChecked(true);
    invertMask_->setChecked(false);
    reverseY_->setChecked(reverseY);
    setOutputDirectory(initialDirectory);
    statusLabel_->setText(QStringLiteral("就绪"));
    updateValidation();
}

bool LayeredExportDialog::reverseY() const { return reverseY_->isChecked(); }
QString LayeredExportDialog::outputDirectory() const { return directory_; }

void LayeredExportDialog::setOutputDirectory(const QString& directory) {
    directory_ = directory.isEmpty() ? QString() : QFileInfo(directory).absoluteFilePath();
    directoryLabel_->setText(directory_.isEmpty() ? QStringLiteral("未选择") : directory_);
    directoryLabel_->setToolTip(directory_);
    updateValidation();
}

void LayeredExportDialog::chooseDirectory() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择导出目录"), directory_);
    if (!selected.isEmpty()) setOutputDirectory(selected);
}

void LayeredExportDialog::updateAspectFromWidth() {
    if (updatingDimensions_) return;
    if (lockAspect_->isChecked() && segmentation_.settings.cellWidth > 0) {
        updatingDimensions_ = true;
        outputHeight_->setValue(static_cast<int>(std::clamp<qint64>(
            qRound64(double(outputWidth_->value()) * segmentation_.settings.cellHeight /
                     segmentation_.settings.cellWidth), 1, INT_MAX)));
        updatingDimensions_ = false;
    }
    updateValidation();
}

void LayeredExportDialog::updateAspectFromHeight() {
    if (updatingDimensions_) return;
    if (lockAspect_->isChecked() && segmentation_.settings.cellHeight > 0) {
        updatingDimensions_ = true;
        outputWidth_->setValue(static_cast<int>(std::clamp<qint64>(
            qRound64(double(outputHeight_->value()) * segmentation_.settings.cellWidth /
                     segmentation_.settings.cellHeight), 1, INT_MAX)));
        updatingDimensions_ = false;
    }
    updateValidation();
}

LayeredPackageRequest LayeredExportDialog::currentRequest() const {
    const auto strategy = layerStrategy_->currentData().value<LayerCountStrategy>();
    return {directory_, document_, segmentation_, traversalMode_,
        {pixelScale_, maximumHeight_->value(), layerHeight_->value(),
         outputWidth_->value(), outputHeight_->value(), invertHeight_,
         invertMask_->isChecked(), reverseY_->isChecked(), strategy}, true};
}

void LayeredExportDialog::updateValidation() {
    QString error;
    const int layers = LayeredPackageExporter::layerCount(currentRequest().options, &error);
    const qint64 cells = qint64(segmentation_.rows) * segmentation_.columns;
    if (!document_ || cells <= 0) error = QStringLiteral("没有可导出的分割结果。");
    else if (directory_.isEmpty()) error = QStringLiteral("请选择导出目录。");
    else if (!QFileInfo(directory_).isDir()) error = QStringLiteral("导出目录不存在或不可用。");
    if (error.isEmpty()) {
        summaryLabel_->setText(QStringLiteral(
            "%1 个分块 × %2 层 = %3 个 1-bit BMP；pixel2phy %4 μm/px")
            .arg(cells).arg(layers).arg(cells * layers)
            .arg(pixelScale_, 0, 'g', 12));
    } else {
        summaryLabel_->setText(error);
    }
    exportButton_->setEnabled(!tasks_->isBusy() && error.isEmpty());
}

void LayeredExportDialog::startPreflight() {
    if (tasks_->isBusy()) return;
    const LayeredPackageRequest request = currentRequest();
    action_ = Action::Preflight;
    statusLabel_->setText(QStringLiteral("准备预检"));
    if (!tasks_->start(QStringLiteral("分层导出预检"),
        [request](const auto& token, const auto& progress, QString*) {
            return QVariant::fromValue(LayeredPackageExporter::inspect(request, token, progress));
        })) action_ = Action::None;
}

void LayeredExportDialog::showPreflight(const ExportPreflightResult& result) {
    QMessageBox box(this);
    box.setObjectName(QStringLiteral("layeredExportPreflightDialog"));
    box.setWindowTitle(result.valid ? QStringLiteral("确认分层导出")
                                    : QStringLiteral("导出预检未通过"));
    box.setIcon(result.valid ? (result.conflictCount > 0 ? QMessageBox::Warning
                                                         : QMessageBox::Question)
                             : QMessageBox::Critical);
    box.setText(QStringLiteral("BMP 文件：%1 个\n命名：%2 — %3\n预计输出：%4")
        .arg(result.bmpFileCount).arg(result.firstOutput, result.lastOutput)
        .arg(formatStorageSize(result.estimatedTotalBytes)));
    QStringList details = result.warnings;
    details.prepend(QStringLiteral("所需空间（含安全余量）：%1")
                        .arg(formatStorageSize(result.requiredBytes)));
    details << QStringLiteral("已有受管目标：%1 个").arg(result.conflictCount);
    if (!result.blockingError.isEmpty()) details << result.blockingError;
    box.setInformativeText(details.join(QLatin1Char('\n')));
    if (!result.conflictSamples.isEmpty()) box.setDetailedText(
        result.conflictSamples.join(QLatin1Char('\n')));
    if (!result.valid) {
        box.addButton(QMessageBox::Close);
        box.exec();
        return;
    }
    auto* start = box.addButton(QStringLiteral("开始导出"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    QCheckBox* overwrite = nullptr;
    if (result.conflictCount > 0) {
        overwrite = new QCheckBox(
            QStringLiteral("确认覆盖上述 %1 个受管目标").arg(result.conflictCount), &box);
        overwrite->setObjectName(QStringLiteral("layeredExportOverwriteCheck"));
        box.setCheckBox(overwrite);
        start->setEnabled(false);
        connect(overwrite, &QCheckBox::toggled, start, &QWidget::setEnabled);
    }
    box.exec();
    if (box.clickedButton() == start)
        startExport({result.conflictCount > 0, true, result.targetFingerprint});
}

void LayeredExportDialog::startExport(const ExportExecutionOptions& execution) {
    const LayeredPackageRequest request = currentRequest();
    action_ = Action::Export;
    statusLabel_->setText(QStringLiteral("准备导出"));
    if (!tasks_->start(QStringLiteral("导出分层包"),
        [request, execution](const auto& token, const auto& progress, QString* error) {
            return LayeredPackageExporter::exportPackage(request, execution, token, progress,
                                                         error) ? QVariant(true) : QVariant{};
        })) action_ = Action::None;
}

void LayeredExportDialog::setProgress(const TaskProgress& progress) {
    statusLabel_->setText(progress.phase);
    if (progress.total <= 0) {
        progressBar_->setRange(0, 0);
        percentLabel_->setText(QStringLiteral("--"));
        return;
    }
    const double ratio = std::clamp(double(progress.completed) / progress.total, 0.0, 1.0);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(static_cast<int>(std::lround(ratio * 1000.0)));
    percentLabel_->setText(QStringLiteral("%1%").arg(qRound(ratio * 100.0)));
}

void LayeredExportDialog::setBusy(bool busy) {
    directoryButton_->setEnabled(!busy);
    layerHeight_->setEnabled(!busy);
    maximumHeight_->setEnabled(!busy);
    layerStrategy_->setEnabled(!busy);
    outputWidth_->setEnabled(!busy);
    outputHeight_->setEnabled(!busy);
    lockAspect_->setEnabled(!busy);
    invertMask_->setEnabled(!busy);
    reverseY_->setEnabled(!busy);
    closeButton_->setEnabled(!busy);
    cancelTaskButton_->setEnabled(busy);
    progressBar_->setVisible(busy);
    percentLabel_->setVisible(busy);
    updateValidation();
}

void LayeredExportDialog::closeEvent(QCloseEvent* event) {
    if (tasks_->isBusy()) {
        tasks_->cancel();
        event->ignore();
        return;
    }
    event->accept();
}
