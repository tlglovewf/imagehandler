#include "ui/ElementPointCloudWindow.h"

#include "imaging/PointCloudBuilder.h"
#include "tasks/TaskManager.h"
#include "ui/PointCloudWidget.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

ElementPointCloudWindow::ElementPointCloudWindow(QWidget* parent)
    : QWidget(parent, Qt::Window), tasks_(new TaskManager(this)) {
    setObjectName(QStringLiteral("elementPointCloudWindow"));
    setWindowTitle(QStringLiteral("单要素 PointCloud"));
    resize(960, 720);

    auto* root = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    elementLabel_ = new QLabel(QStringLiteral("未选择要素"));
    elementLabel_->setObjectName(QStringLiteral("elementPointCloudInfo"));
    statusLabel_ = new QLabel(QStringLiteral("就绪"));
    statusLabel_->setObjectName(QStringLiteral("elementPointCloudStatus"));
    header->addWidget(elementLabel_);
    header->addStretch();
    header->addWidget(statusLabel_);
    root->addLayout(header);

    cloudView_ = new PointCloudWidget;
    cloudView_->setObjectName(QStringLiteral("elementPointCloudView"));
    root->addWidget(cloudView_, 1);

    auto* taskRow = new QHBoxLayout;
    progressBar_ = new QProgressBar;
    progressBar_->setObjectName(QStringLiteral("elementPointCloudProgress"));
    progressBar_->setTextVisible(false);
    percentLabel_ = new QLabel;
    percentLabel_->setObjectName(QStringLiteral("elementPointCloudPercent"));
    percentLabel_->setFixedWidth(48);
    percentLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cancelButton_ = new QPushButton(QStringLiteral("取消生成"));
    cancelButton_->setObjectName(QStringLiteral("elementPointCloudCancel"));
    taskRow->addWidget(progressBar_, 1);
    taskRow->addWidget(percentLabel_);
    taskRow->addWidget(cancelButton_);
    root->addLayout(taskRow);
    progressBar_->hide();
    percentLabel_->hide();
    cancelButton_->setEnabled(false);

    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        pendingRequest_.reset();
        tasks_->cancel();
    });
    connect(tasks_, &TaskManager::taskStarted, this,
            [this](quint64, const QString&) {
        statusLabel_->setText(QStringLiteral("准备生成"));
        progressBar_->setRange(0, 0);
        progressBar_->show();
        percentLabel_->setText(QStringLiteral("--"));
        percentLabel_->show();
        cancelButton_->setEnabled(true);
    });
    connect(tasks_, &TaskManager::taskProgress, this,
            [this](quint64, const TaskProgress& progress) { setProgress(progress); });
    connect(tasks_, &TaskManager::taskFinished, this,
            [this](quint64, const QVariant& value) {
        const PointCloudDocumentPtr result = value.value<PointCloudDocumentPtr>();
        activeRequest_.reset();
        if (pendingRequest_) { startPendingRequest(); return; }
        if (clearing_) { finishRequest(QStringLiteral("已清空")); return; }
        if (result) {
            cloudView_->setDocument(result);
            finishRequest(QStringLiteral("生成完成"));
            Q_EMIT documentReady(result);
        } else {
            finishRequest(QStringLiteral("生成失败"));
        }
    });
    connect(tasks_, &TaskManager::taskCancelled, this,
            [this](quint64) {
        activeRequest_.reset();
        if (pendingRequest_) { startPendingRequest(); return; }
        finishRequest(clearing_ ? QStringLiteral("已清空") : QStringLiteral("已取消"));
    });
    connect(tasks_, &TaskManager::taskFailed, this,
            [this](quint64, const QString& error) {
        activeRequest_.reset();
        if (pendingRequest_) { startPendingRequest(); return; }
        finishRequest(QStringLiteral("生成失败：%1").arg(error));
    });
}

bool ElementPointCloudWindow::isVulkanAvailable() const noexcept {
    return cloudView_ && cloudView_->isVulkanAvailable();
}

PointCloudDocumentPtr ElementPointCloudWindow::document() const {
    return cloudView_ ? cloudView_->document() : PointCloudDocumentPtr{};
}

void ElementPointCloudWindow::showElement(
    const ImageDocumentPtr& source, const ImageRegion& region, int row, int column,
    qint64 traversalIndex, const PointCloudSettings& settings, quint64 generation) {
    clearing_ = false;
    Request request{source, region, settings, generation, row, column, traversalIndex};
    pendingRequest_ = request;
    elementLabel_->setText(QStringLiteral("第 %1 行，第 %2 列 | %3.bmp | %4 × %5 像素")
        .arg(row + 1).arg(column + 1).arg(traversalIndex + 1)
        .arg(region.width).arg(region.height));
    setWindowTitle(QStringLiteral("单要素 PointCloud - %1.bmp").arg(traversalIndex + 1));
    show();
    raise();
    activateWindow();
    if (!isVulkanAvailable()) {
        pendingRequest_.reset();
        finishRequest(QStringLiteral("当前环境没有可用的 Vulkan PointCloud 渲染器"));
        return;
    }
    if (tasks_->isBusy()) {
        statusLabel_->setText(QStringLiteral("正在切换要素…"));
        tasks_->cancel();
        return;
    }
    startPendingRequest();
}

void ElementPointCloudWindow::startPendingRequest() {
    if (!pendingRequest_ || tasks_->isBusy()) return;
    activeRequest_ = std::move(pendingRequest_);
    pendingRequest_.reset();
    cloudView_->clear();
    const Request request = *activeRequest_;
    const bool started = tasks_->start(QStringLiteral("生成单要素 PointCloud"),
        [request](const auto& token, const auto& progress, QString* error) {
            return QVariant::fromValue(PointCloudBuilder::buildRegion(
                request.source, request.region, request.settings, request.generation,
                token, progress, error));
        });
    if (!started) {
        pendingRequest_ = request;
        activeRequest_.reset();
    }
}

void ElementPointCloudWindow::setProgress(const TaskProgress& progress) {
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

void ElementPointCloudWindow::finishRequest(const QString& status) {
    clearing_ = false;
    statusLabel_->setText(status);
    progressBar_->hide();
    percentLabel_->hide();
    cancelButton_->setEnabled(false);
}

void ElementPointCloudWindow::cancelAndClear() {
    clearing_ = true;
    pendingRequest_.reset();
    activeRequest_.reset();
    tasks_->cancel();
    cloudView_->clear();
    elementLabel_->setText(QStringLiteral("未选择要素"));
    if (!tasks_->isBusy()) finishRequest(QStringLiteral("已清空"));
    hide();
}

void ElementPointCloudWindow::closeEvent(QCloseEvent* event) {
    cancelAndClear();
    event->accept();
}

