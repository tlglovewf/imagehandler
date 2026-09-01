#pragma once

#include "domain/Models.h"
#include "domain/PointCloudModels.h"

#include <QWidget>
#include <optional>

class QLabel;
class QProgressBar;
class QPushButton;
class PointCloudWidget;
class TaskManager;
class QCloseEvent;

class ElementPointCloudWindow final : public QWidget {
    Q_OBJECT
public:
    explicit ElementPointCloudWindow(QWidget* parent = nullptr);

    void showElement(const ImageDocumentPtr& source, const ImageRegion& region,
                     int row, int column, qint64 traversalIndex,
                     const PointCloudSettings& settings, quint64 generation);
    void cancelAndClear();
    [[nodiscard]] bool isVulkanAvailable() const noexcept;
    [[nodiscard]] PointCloudDocumentPtr document() const;

Q_SIGNALS:
    void documentReady(const PointCloudDocumentPtr& document);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Request {
        ImageDocumentPtr source;
        ImageRegion region;
        PointCloudSettings settings;
        quint64 generation = 0;
        int row = -1;
        int column = -1;
        qint64 traversalIndex = -1;
    };

    void startPendingRequest();
    void setProgress(const TaskProgress& progress);
    void finishRequest(const QString& status);

    TaskManager* tasks_ = nullptr;
    PointCloudWidget* cloudView_ = nullptr;
    QLabel* elementLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* percentLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    std::optional<Request> pendingRequest_;
    std::optional<Request> activeRequest_;
    bool clearing_ = false;
};

