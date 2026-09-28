#pragma once

#include "domain/JobModels.h"

#include <QWidget>

class JobPreviewWidget;
class TaskManager;
class QPushButton;
class QComboBox;
class QLabel;
class QProgressBar;
class QTimer;
class QCloseEvent;

class ProcessingPreviewWindow final : public QWidget {
    Q_OBJECT
public:
    explicit ProcessingPreviewWindow(QWidget* parent = nullptr);
    ~ProcessingPreviewWindow() override = default;

    void loadFile(const QString& path);
    void togglePlayback();
    void stepPrevious();
    void stepNext();

    [[nodiscard]] JobDocumentPtr document() const { return document_; }
    [[nodiscard]] qint64 currentIndex() const noexcept { return currentIndex_; }
    [[nodiscard]] qint64 processedCount() const noexcept { return processedCount_; }
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }
    [[nodiscard]] int playbackInterval() const noexcept;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void chooseFile();
    void advancePlayback();
    void pausePlayback();
    void applyPosition(qint64 index);
    void updateControls();
    void setLoading(bool loading);

    TaskManager* tasks_ = nullptr;
    JobPreviewWidget* preview_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QPushButton* playPauseButton_ = nullptr;
    QPushButton* previousButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QProgressBar* loadProgress_ = nullptr;
    QTimer* playbackTimer_ = nullptr;
    JobDocumentPtr document_;
    QString lastDirectory_;
    qint64 currentIndex_ = -1;
    qint64 processedCount_ = 0;
    bool playing_ = false;
    bool loading_ = false;
};

