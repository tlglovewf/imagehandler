#pragma once

#include "domain/MaskSliceModels.h"
#include "domain/JobModels.h"

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
    void showElements(const ImageDocumentPtr& source,
                      const QVector<MaskSliceElement>& elements,
                      const QPoint& activeCell, const PointCloudSettings& mapping);
    void setElements(const ImageDocumentPtr& source,
                     const QVector<MaskSliceElement>& elements,
                     const QPoint& activeCell, const PointCloudSettings& mapping);
    void setPointCloudSettings(const PointCloudSettings& mapping);
    void showJobPlacement(const JobDocumentPtr& document, qint64 placementId);
    void cancelAndClear();
    [[nodiscard]] const std::optional<MaskSliceAnalysis>& analysis() const noexcept {
        return analysis_;
    }
    [[nodiscard]] const std::optional<MaskStackGroupAnalysis>& stackAnalysis() const noexcept {
        return stackAnalysis_;
    }

Q_SIGNALS:
    void invertHeightChanged(bool inverted);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Action { None, Analyze, Export };

    void scheduleAnalysis();
    void scheduleActiveAnalysis();
    void scheduleStackAnalysis();
    void startAnalysis();
    void startStackAnalysis();
    void setJobMode(bool enabled);
    void populateJobLayers(const MaskStackPreviewDataPtr& preview,
                           const QVector<QImage>& thumbnails);
    void chooseExportDirectory();
    void startExport(const QString& directory, bool overwrite);
    void updateDimensionMapping();
    void updateControls();
    void populateLayers(const MaskSliceAnalysis& analysis);
    void updateStackLayerSelection(int row);
    void updateStackZScale();
    void updateStackInformation();
    void updateElementInformation();
    void populateElementList();
    void activateElement(int index);
    void setProgress(const TaskProgress& progress);
    [[nodiscard]] MaskSliceRequest currentRequest() const;
    [[nodiscard]] MaskStackGroupRequest currentStackRequest() const;
    [[nodiscard]] int unifiedLayerCount() const;
    [[nodiscard]] int conflictCount(const QString& directory) const;

    TaskManager* tasks_ = nullptr;
    TaskManager* stackTasks_ = nullptr;
    QTimer* analysisTimer_ = nullptr;
    QTimer* stackTimer_ = nullptr;
    QLabel* elementLabel_ = nullptr;
    QLabel* mappingLabel_ = nullptr;
    QLabel* totalLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* percentLabel_ = nullptr;
    QWidget* elementListContainer_ = nullptr;
    QWidget* settingsContainer_ = nullptr;
    QListWidget* elementList_ = nullptr;
    QDoubleSpinBox* layerHeight_ = nullptr;
    QDoubleSpinBox* zOffset_ = nullptr;
    QSpinBox* outputWidth_ = nullptr;
    QSpinBox* outputHeight_ = nullptr;
    QCheckBox* lockAspect_ = nullptr;
    QCheckBox* invertMask_ = nullptr;
    QCheckBox* invertHeight_ = nullptr;
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
    std::optional<MaskStackGroupAnalysis> stackAnalysis_;
    QVector<MaskSliceElement> elements_;
    int activeElementIndex_ = -1;
    int row_ = -1;
    int column_ = -1;
    qint64 traversalIndex_ = -1;
    Action action_ = Action::None;
    bool pendingAnalysis_ = false;
    bool pendingStackAnalysis_ = false;
    bool updatingDimensions_ = false;
    bool clearing_ = false;
    bool jobMode_ = false;
    JobDocumentPtr jobDocument_;
    qint64 jobPlacementId_ = -1;
    quint64 analysisRevision_ = 0;
    quint64 stackRevision_ = 0;
};
