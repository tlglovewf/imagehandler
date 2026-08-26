#include "tasks/TaskManager.h"
#include "imaging/MosaicBuilder.h"

#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <exception>

namespace {
struct WorkResult { QVariant value; QString error; bool cancelled = false; };

struct ProgressDispatchState {
    QMutex mutex;
    TaskProgress latest;
    bool scheduled = false;
    bool active = true;
};
}

TaskManager::TaskManager(QObject* parent) : QObject(parent) {
    qRegisterMetaType<TaskProgress>();
    qRegisterMetaType<ImageDocumentPtr>();
    qRegisterMetaType<SegmentationResult>();
    qRegisterMetaType<MosaicBuildResult>();
}

TaskManager::~TaskManager() {
    cancel();
    QThreadPool::globalInstance()->waitForDone();
}

bool TaskManager::start(const QString& title, Work work) {
    if (busy_) return false;
    busy_ = true;
    activeId_ = nextId_++;
    token_ = std::make_shared<CancellationToken>();
    const auto id = activeId_;
    const auto token = token_;
    const auto progressState = std::make_shared<ProgressDispatchState>();
    Q_EMIT taskStarted(id, title);

    auto* watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcher<WorkResult>::finished, this, [this, watcher, id, progressState] {
        const WorkResult result = watcher->result();
        watcher->deleteLater();
        if (id != activeId_) return;
        {
            QMutexLocker lock(&progressState->mutex);
            progressState->active = false;
        }
        busy_ = false;
        token_.reset();
        if (result.cancelled) Q_EMIT taskCancelled(id);
        else if (!result.error.isEmpty()) Q_EMIT taskFailed(id, result.error);
        else Q_EMIT taskFinished(id, result.value);
    });
    auto progress = [this, id, progressState](const TaskProgress& value) {
        {
            QMutexLocker lock(&progressState->mutex);
            if (!progressState->active) return;
            progressState->latest = value;
            if (progressState->scheduled) return;
            progressState->scheduled = true;
        }
        QMetaObject::invokeMethod(this, [this, id, progressState] {
            TaskProgress latest;
            {
                QMutexLocker lock(&progressState->mutex);
                if (!progressState->active) return;
                latest = progressState->latest;
                progressState->scheduled = false;
            }
            if (id == activeId_) Q_EMIT taskProgress(id, latest);
        }, Qt::QueuedConnection);
    };
    watcher->setFuture(QtConcurrent::run([token, progress, work = std::move(work)] {
        WorkResult result;
        try {
            result.value = work(token, progress, &result.error);
        } catch (const std::exception& exception) {
            result.error = QString::fromUtf8(exception.what());
        } catch (...) {
            result.error = QStringLiteral("后台任务发生未知异常。");
        }
        result.cancelled = token->isCancelled();
        return result;
    }));
    return true;
}

void TaskManager::cancel() {
    if (token_) token_->cancel();
}
