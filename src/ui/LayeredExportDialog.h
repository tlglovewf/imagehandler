#pragma once

#include "io/LayeredPackageTypes.h"
#include "io/ExportTypes.h"
#include "imaging/LayeredMaskProcessor.h"

#include <QDialog>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QListWidget;
class QTimer;
class TaskManager;
class MaskStackWidget;

class LayeredExportDialog final : public QDialog {
    Q_OBJECT
public:
    explicit LayeredExportDialog(QWidget* parent = nullptr);

    void configure(const ImageDocumentPtr& document,
                   const SegmentationResult& segmentation,
                   TraversalMode traversalMode, double pixelScale,
                   double maximumHeight, bool invertHeight,
                   bool reverseY, const QString& initialDirectory,
                   const QPoint& activeCell = QPoint(-1, -1),
                   const QVector<QPoint>& selectedCells = {});
    void setElementGrouping(const ElementGroupingResult& grouping);
    [[nodiscard]] bool reverseY() const;
    [[nodiscard]] QString outputDirectory() const;
    void setOutputDirectory(const QString& directory);

Q_SIGNALS:
    void invertHeightChanged(bool inverted);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Action { None, Preflight, Export };

    void chooseDirectory();
    void updateAspectFromWidth();
    void updateAspectFromHeight();
    void updateDimensionsFromMultiplier();
    void updateValidation();
    void startPreflight();
    void showPreflight(const ExportPreflightResult& result);
    void startExport(const ExportExecutionOptions& execution);
    [[nodiscard]] LayeredPackageRequest currentRequest() const;
    void setProgress(const TaskProgress& progress);
    void setBusy(bool busy);
    void schedulePreview();
    void startPreview();
    void applyPreview(const LayeredMaskPreview& preview);
    void updatePreviewLayer(int row);
    void updatePreviewZScale();
    void updatePreviewElementLabel();
    void activatePreviewBlock(qint64 traversalIndex);
    [[nodiscard]] LayeredMaskRequest currentPreviewRequest() const;

    TaskManager* tasks_ = nullptr;
    TaskManager* previewTasks_ = nullptr;
    QTimer* previewTimer_ = nullptr;
    QPushButton* directoryButton_ = nullptr;
    QLabel* directoryLabel_ = nullptr;
    QDoubleSpinBox* layerHeight_ = nullptr;
    QDoubleSpinBox* zOffset_ = nullptr;
    QDoubleSpinBox* maximumHeight_ = nullptr;
    QComboBox* layerStrategy_ = nullptr;
    QComboBox* jobMoveCommand_ = nullptr;
    QSpinBox* baseLayerCount_ = nullptr;
    QDoubleSpinBox* exportMultiplier_ = nullptr;
    QSpinBox* outputWidth_ = nullptr;
    QSpinBox* outputHeight_ = nullptr;
    QCheckBox* lockAspect_ = nullptr;
    QCheckBox* invertMask_ = nullptr;
    QCheckBox* invertHeightCheck_ = nullptr;
    QCheckBox* reverseY_ = nullptr;
    QCheckBox* centerOrigin_ = nullptr;
    QCheckBox* attachToCanvas_ = nullptr;
    QSpinBox* canvasWidth_ = nullptr;
    QSpinBox* canvasHeight_ = nullptr;
    QComboBox* canvasBackground_ = nullptr;
    QCheckBox* invertCanvas_ = nullptr;
    QDoubleSpinBox* rotationDegrees_ = nullptr;
    QDoubleSpinBox* scaleX_ = nullptr;
    QDoubleSpinBox* scaleY_ = nullptr;
    QCheckBox* mirrorX_ = nullptr;
    QComboBox* transformSampling_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* percentLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelTaskButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QLabel* previewElementLabel_ = nullptr;
    QLabel* previewStatusLabel_ = nullptr;
    QListWidget* previewLayers_ = nullptr;
    MaskStackWidget* stackView_ = nullptr;
    QComboBox* stackMode_ = nullptr;
    QCheckBox* automaticZScale_ = nullptr;
    QDoubleSpinBox* stackZScale_ = nullptr;
    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    ElementGroupingResult grouping_;
    TraversalMode traversalMode_ = TraversalMode::SnakeZ;
    double pixelScale_ = 0.25;
    QString directory_;
    Action action_ = Action::None;
    bool updatingDimensions_ = false;
    QPoint previewCell_{-1, -1};
    QVector<QPoint> selectedCells_;
    quint64 previewRevision_ = 0;
    bool previewPending_ = false;
};
