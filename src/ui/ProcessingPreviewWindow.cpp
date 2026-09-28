#include "ui/ProcessingPreviewWindow.h"

#include "io/JobPackageLoader.h"
#include "tasks/TaskManager.h"
#include "ui/JobPreviewWidget.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

ProcessingPreviewWindow::ProcessingPreviewWindow(QWidget* parent)
    : QWidget(parent, Qt::Window), tasks_(new TaskManager(this)),
      playbackTimer_(new QTimer(this)) {
    setObjectName(QStringLiteral("processingPreviewWindow"));
    setWindowTitle(QStringLiteral("加工展示"));
    resize(1180, 760);
    lastDirectory_ = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (lastDirectory_.isEmpty()) lastDirectory_ = QDir::homePath();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);
    auto* controls = new QHBoxLayout;
    importButton_ = new QPushButton(QStringLiteral("导入加工文件"));
    importButton_->setObjectName(QStringLiteral("processingImportButton"));
    playPauseButton_ = new QPushButton(QStringLiteral("播放"));
    playPauseButton_->setObjectName(QStringLiteral("processingPlayPauseButton"));
    previousButton_ = new QPushButton(QStringLiteral("上一个"));
    previousButton_->setObjectName(QStringLiteral("processingPreviousButton"));
    nextButton_ = new QPushButton(QStringLiteral("下一个"));
    nextButton_->setObjectName(QStringLiteral("processingNextButton"));
    speedCombo_ = new QComboBox;
    speedCombo_->setObjectName(QStringLiteral("processingSpeedCombo"));
    for (const int speed : {1, 2, 4, 8})
        speedCombo_->addItem(QStringLiteral("%1x").arg(speed), speed);
    progressLabel_ = new QLabel(QStringLiteral("当前 0/0"));
    progressLabel_->setObjectName(QStringLiteral("processingProgressLabel"));
    cancelButton_ = new QPushButton(QStringLiteral("取消加载"));
    cancelButton_->setObjectName(QStringLiteral("processingCancelButton"));
    cancelButton_->hide();
    controls->addWidget(importButton_);
    controls->addWidget(playPauseButton_);
    controls->addWidget(previousButton_);
    controls->addWidget(nextButton_);
    controls->addWidget(new QLabel(QStringLiteral("倍速")));
    controls->addWidget(speedCombo_);
    controls->addWidget(progressLabel_);
    controls->addStretch();
    controls->addWidget(cancelButton_);
    root->addLayout(controls);

    auto* statusRow = new QHBoxLayout;
    statusLabel_ = new QLabel(QStringLiteral("请导入加工文件"));
    statusLabel_->setObjectName(QStringLiteral("processingStatusLabel"));
    loadProgress_ = new QProgressBar;
    loadProgress_->setObjectName(QStringLiteral("processingLoadProgress"));
    loadProgress_->setTextVisible(false);
    loadProgress_->setFixedWidth(180);
    loadProgress_->hide();
    statusRow->addWidget(statusLabel_);
    statusRow->addStretch();
    statusRow->addWidget(loadProgress_);
    root->addLayout(statusRow);

    preview_ = new JobPreviewWidget;
    preview_->setObjectName(QStringLiteral("processingJobPreviewWidget"));
    preview_->setForceIndividualLabels(true);
    preview_->setVisibleStackPlacementCount(0);
    preview_->setCoordinateOptionsVisible(false);
    preview_->setReverseCoordinates(false);
    preview_->setCenterRelativeCoordinates(false);
    preview_->setXAxisPositiveLeft(true);
    preview_->setYAxisPositiveDown(true);
    preview_->setPreserveCoordinateValues(true);
    root->addWidget(preview_, 1);

    playbackTimer_->setTimerType(Qt::PreciseTimer);
    playbackTimer_->setInterval(1000);
    connect(playbackTimer_, &QTimer::timeout, this, &ProcessingPreviewWindow::advancePlayback);
    connect(importButton_, &QPushButton::clicked, this, &ProcessingPreviewWindow::chooseFile);
    connect(playPauseButton_, &QPushButton::clicked, this, &ProcessingPreviewWindow::togglePlayback);
    connect(previousButton_, &QPushButton::clicked, this, &ProcessingPreviewWindow::stepPrevious);
    connect(nextButton_, &QPushButton::clicked, this, &ProcessingPreviewWindow::stepNext);
    connect(cancelButton_, &QPushButton::clicked, tasks_, &TaskManager::cancel);
    connect(speedCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        playbackTimer_->setInterval(playbackInterval());
    });

    connect(tasks_, &TaskManager::taskStarted, this, [this](quint64, const QString&) {
        setLoading(true);
        statusLabel_->setText(QStringLiteral("正在加载加工文件…"));
    });
    connect(tasks_, &TaskManager::taskProgress, this,
            [this](quint64, const TaskProgress& progress) {
        statusLabel_->setText(progress.phase);
        if (progress.total > 0) {
            loadProgress_->setRange(0, 1000);
            const long double ratio = std::clamp(
                static_cast<long double>(progress.completed) /
                    static_cast<long double>(progress.total),
                0.0L, 1.0L);
            loadProgress_->setValue(static_cast<int>(ratio * 1000.0L));
        } else {
            loadProgress_->setRange(0, 0);
        }
    });
    connect(tasks_, &TaskManager::taskFinished, this,
            [this](quint64, const QVariant& value) {
        setLoading(false);
        const auto loaded = value.value<JobPackageLoader::ProcessingLoadResult>();
        if (!loaded.document) return;
        pausePlayback();
        document_ = loaded.document;
        currentIndex_ = -1;
        processedCount_ = 0;
        preview_->setDocument(document_);
        preview_->setForceIndividualLabels(true);
        preview_->setVisibleStackPlacementCount(0);
        preview_->setReverseCoordinates(false);
        preview_->setCenterRelativeCoordinates(false);
        preview_->setXAxisPositiveLeft(true);
        preview_->setYAxisPositiveDown(true);
        preview_->setPreserveCoordinateValues(true);
        preview_->clearSelection();
        preview_->viewTop();
        statusLabel_->setText(loaded.inferredGeometry
            ? QStringLiteral("已加载 %1 个点；%2")
                  .arg(document_->points.size()).arg(loaded.warning)
            : QStringLiteral("已加载 %1 个点").arg(document_->points.size()));
        statusLabel_->setToolTip(loaded.warning);
        updateControls();
    });
    connect(tasks_, &TaskManager::taskCancelled, this, [this](quint64) {
        setLoading(false);
        statusLabel_->setText(QStringLiteral("已取消加载"));
        updateControls();
    });
    connect(tasks_, &TaskManager::taskFailed, this, [this](quint64, const QString& error) {
        setLoading(false);
        statusLabel_->setText(QStringLiteral("加载失败"));
        updateControls();
        QMessageBox::critical(this, QStringLiteral("加工文件加载失败"), error);
    });

    if (!preview_->isVulkanAvailable())
        statusLabel_->setText(QStringLiteral("当前构建未启用 Vulkan Job 预览。"));
    updateControls();
}

int ProcessingPreviewWindow::playbackInterval() const noexcept {
    const int speed = speedCombo_ ? speedCombo_->currentData().toInt() : 1;
    return 1000 / std::max(1, speed);
}

void ProcessingPreviewWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择加工文件"), lastDirectory_,
        QStringLiteral("Job 文件 (*.json *.aldl);;JSON (*.json);;ALDL (*.aldl)"));
    if (!path.isEmpty()) loadFile(path);
}

void ProcessingPreviewWindow::loadFile(const QString& path) {
    if (path.isEmpty() || tasks_->isBusy() || !preview_->isVulkanAvailable()) return;
    pausePlayback();
    lastDirectory_ = QFileInfo(path).absolutePath();
    tasks_->start(QStringLiteral("加载加工文件"),
        [path](const auto& token, const auto& progress, QString* error) {
            return QVariant::fromValue(JobPackageLoader::loadProcessingPackage(
                path, token, progress, error));
        });
}

void ProcessingPreviewWindow::togglePlayback() {
    if (!document_ || document_->points.empty() || loading_) return;
    if (playing_) {
        pausePlayback();
        return;
    }
    if (currentIndex_ >= static_cast<qint64>(document_->points.size()) - 1) {
        currentIndex_ = -1;
        processedCount_ = 0;
        preview_->setVisibleStackPlacementCount(0);
        preview_->clearSelection();
    }
    playing_ = true;
    playPauseButton_->setText(QStringLiteral("暂停"));
    advancePlayback();
    if (playing_) playbackTimer_->start(playbackInterval());
    updateControls();
}

void ProcessingPreviewWindow::advancePlayback() {
    if (!document_ || document_->points.empty()) {
        pausePlayback();
        return;
    }
    const qint64 last = static_cast<qint64>(document_->points.size()) - 1;
    if (currentIndex_ < last) applyPosition(currentIndex_ + 1);
    if (currentIndex_ >= last) pausePlayback();
}

void ProcessingPreviewWindow::pausePlayback() {
    playbackTimer_->stop();
    playing_ = false;
    if (playPauseButton_) playPauseButton_->setText(QStringLiteral("播放"));
    updateControls();
}

void ProcessingPreviewWindow::stepPrevious() {
    pausePlayback();
    if (document_ && currentIndex_ > 0) applyPosition(currentIndex_ - 1);
}

void ProcessingPreviewWindow::stepNext() {
    pausePlayback();
    if (!document_ || document_->points.empty()) return;
    const qint64 last = static_cast<qint64>(document_->points.size()) - 1;
    if (currentIndex_ < last) applyPosition(currentIndex_ + 1);
}

void ProcessingPreviewWindow::applyPosition(qint64 index) {
    if (!document_ || index < 0 ||
        index >= static_cast<qint64>(document_->points.size())) return;
    currentIndex_ = index;
    processedCount_ = index + 1;
    preview_->setVisibleStackPlacementCount(processedCount_);
    preview_->setSelectedPlaceId(document_->points[static_cast<std::size_t>(index)].placementId,
                                 false, true);
    updateControls();
}

void ProcessingPreviewWindow::setLoading(bool loading) {
    loading_ = loading;
    loadProgress_->setVisible(loading);
    cancelButton_->setVisible(loading);
    if (loading) {
        loadProgress_->setRange(0, 0);
        loadProgress_->setValue(0);
    }
    updateControls();
}

void ProcessingPreviewWindow::updateControls() {
    const bool available = preview_ && preview_->isVulkanAvailable();
    const qint64 total = document_ ? static_cast<qint64>(document_->points.size()) : 0;
    importButton_->setEnabled(available && !loading_);
    playPauseButton_->setEnabled(available && !loading_ && total > 0);
    previousButton_->setEnabled(available && !loading_ && currentIndex_ > 0);
    nextButton_->setEnabled(available && !loading_ && total > 0 && currentIndex_ < total - 1);
    speedCombo_->setEnabled(available && !loading_ && total > 0);
    progressLabel_->setText(QStringLiteral("当前 %1/%2").arg(processedCount_).arg(total));
}

void ProcessingPreviewWindow::closeEvent(QCloseEvent* event) {
    pausePlayback();
    if (tasks_->isBusy()) tasks_->cancel();
    QWidget::closeEvent(event);
}
