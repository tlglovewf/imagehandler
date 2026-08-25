#pragma once

#include "domain/Models.h"
#include <QObject>
#include <QVariant>
#include <functional>

class QFutureWatcherBase;

class TaskManager final : public QObject {
    Q_OBJECT
public:
    using Progress = std::function<void(const TaskProgress&)>;
    using Work = std::function<QVariant(const CancellationTokenPtr&, const Progress&, QString*)>;

    explicit TaskManager(QObject* parent = nullptr);
    ~TaskManager() override;
    bool start(const QString& title, Work work);
    bool isBusy() const { return busy_; }

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void taskStarted(quint64 id, const QString& title);
    void taskProgress(quint64 id, const TaskProgress& progress);
    void taskFinished(quint64 id, const QVariant& result);
    void taskCancelled(quint64 id);
    void taskFailed(quint64 id, const QString& error);

private:
    quint64 nextId_ = 1;
    quint64 activeId_ = 0;
    bool busy_ = false;
    CancellationTokenPtr token_;
};

Q_DECLARE_METATYPE(TaskProgress)
