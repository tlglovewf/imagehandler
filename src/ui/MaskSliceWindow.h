#pragma once

#include "domain/MaskSliceModels.h"

#include <QWidget>
#include <optional>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class TaskManager;
class MaskStackWidget;

class MaskSliceWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MaskSliceWindow(QWidget* parent = nullptr);

    void showElement(const ImageDocumentPtr& source, const ImageRegion& region,
                     int row, int column, qint64 traversalIndex,
                     const PointCloudSettings& mapping);
    void setPointCloudSettings(const PointCloudSettings& mapping);
    void cancelAndClear();
    [[nodiscard]] const std::optional<MaskSliceAnalysis>& analysis() const noexcept {
        return analysis_;
    }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Action { None, Analyze, Export };

    void scheduleAnalysis();
    void startAnalysis();
    void chooseExportDirectory();
    void startExport(const QString& directory, bool overwrite);
    void updateDimensionMapping();
    void updateControls();
    void populateLayers(const MaskSliceAnalysis& analysis);
    void updateStackLayerSelection(int row);
    void updateStackZScale();
    void updateStackInformation();
    void setProgress(const TaskProgress& progress);
    [[nodiscard]] MaskSliceRequest currentRequest() const;
    [[nodiscard]] int conflictCount(const QString& directory) const;

    TaskManager* tasks_ = nullptr;
    QTimer* analysisTimer_ = nullptr;
    QLabel* elementLabel_ = nullptr;
    QLabel* mappingLabel_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* percentLabel_ = nullptr;
    QDoubleSpinBox* layerHeight_ = nullptr;
    QSpinBox* outputWidth_ = nullptr;
    QSpinBox* outputHeight_ = nullptr;
    QCheckBox* lockAspect_ = nullptr;
    QCheckBox* invertMask_ = nullptr;
    QComboBox* stackMode_ = nullptr;
    QCheckBox* automaticZScale_ = nullptr;
    QDoubleSpinBox* stackZScale_ = nullptr;
    QLabel* stackLayerLabel_ = nullptr;
    QLabel* stackSamplingLabel_ = nullptr;
    MaskStackWidget* stackView_ = nullptr;
    QListWidget* layerList_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    ImageDocumentPtr source_;
    ImageRegion region_;
    PointCloudSettings mapping_;
    std::optional<MaskSliceAnalysis> analysis_;
    int row_ = -1;
    int column_ = -1;
    qint64 traversalIndex_ = -1;
    Action action_ = Action::None;
    bool pendingAnalysis_ = false;
    bool updatingDimensions_ = false;
    bool clearing_ = false;
};
