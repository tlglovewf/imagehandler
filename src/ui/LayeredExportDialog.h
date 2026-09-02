#pragma once

#include "io/LayeredPackageTypes.h"
#include "io/ExportTypes.h"

#include <QDialog>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class TaskManager;

class LayeredExportDialog final : public QDialog {
    Q_OBJECT
public:
    explicit LayeredExportDialog(QWidget* parent = nullptr);

    void configure(const ImageDocumentPtr& document,
                   const SegmentationResult& segmentation,
                   TraversalMode traversalMode, double pixelScale,
                   double maximumHeight, bool invertHeight,
                   bool reverseY, const QString& initialDirectory);
    [[nodiscard]] bool reverseY() const;
    [[nodiscard]] QString outputDirectory() const;
    void setOutputDirectory(const QString& directory);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Action { None, Preflight, Export };

    void chooseDirectory();
    void updateAspectFromWidth();
    void updateAspectFromHeight();
    void updateValidation();
    void startPreflight();
    void showPreflight(const ExportPreflightResult& result);
    void startExport(const ExportExecutionOptions& execution);
    [[nodiscard]] LayeredPackageRequest currentRequest() const;
    void setProgress(const TaskProgress& progress);
    void setBusy(bool busy);

    TaskManager* tasks_ = nullptr;
    QPushButton* directoryButton_ = nullptr;
    QLabel* directoryLabel_ = nullptr;
    QDoubleSpinBox* layerHeight_ = nullptr;
    QDoubleSpinBox* maximumHeight_ = nullptr;
    QComboBox* layerStrategy_ = nullptr;
    QSpinBox* outputWidth_ = nullptr;
    QSpinBox* outputHeight_ = nullptr;
    QCheckBox* lockAspect_ = nullptr;
    QCheckBox* invertMask_ = nullptr;
    QCheckBox* reverseY_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* percentLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelTaskButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    TraversalMode traversalMode_ = TraversalMode::SnakeZ;
    double pixelScale_ = 0.25;
    bool invertHeight_ = true;
    QString directory_;
    Action action_ = Action::None;
    bool updatingDimensions_ = false;
};
