#pragma once

#include "domain/Models.h"
#include "domain/JobModels.h"
#include "domain/PointCloudModels.h"
#include "domain/MaskSliceModels.h"
#include "imaging/ElementGrouping.h"
#include "io/ExportTypes.h"
#include <QMainWindow>
#include <QVector>

class LargeImageView;
class JobPreviewWidget;
class PointCloudWidget;
class ElementPointCloudWindow;
class MaskSliceWindow;
class LayeredExportDialog;
class TaskManager;
struct MosaicBuildResult;
class QListWidget;
class QPushButton;
class QProgressBar;
class QLabel;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QTimer;
class QEvent;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QTabBar;
class QSplitter;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class PendingAction { None, Scan, Load, Merge, Detect, Grid, GroupElements,
                               SelectedBlocksExport, MosaicExport,
                               JobLoad, JobLoadExport, JobPreviewBuild, PointCloudBuild };
    void buildUi();
    void connectUi();
    void openImage();
    void openDirectory();
    void scanDirectory(const QString& directory);
    void loadPath(const QString& path);
    void loadCurrentListItem();
    void navigate(int delta);
    void mergeImages();
    void startMosaicBuild(bool allowMissingTiles);
    void acceptMosaicResult(const MosaicBuildResult& result);
    void showMergedDocument(int row);
    void chooseMergedExportPath();
    void detectSegmentation();
    void applyManualSegmentation();
    void groupElements();
    void clearElementGrouping();
    void chooseExportDirectory();
    void chooseSelectedBlocksExportDirectory();
    void chooseJobFile();
    void loadJobFile(const QString& path, bool exported);
    void scheduleJobPreview(bool switchToJob);
    void rebuildJobPreview();
    void acceptJobDocument(const JobDocumentPtr& document, bool preserveLink);
    void generatePointCloud();
    void showElementPointCloud(int row, int column);
    void showMaskSlices(int row, int column);
    void showMaskSlices(const QVector<QPoint>& cells, const QPoint& activeCell);
    void syncMaskSliceSelection(const QVector<QPoint>& cells, const QPoint& activeCell);
    [[nodiscard]] QVector<MaskSliceElement> maskSliceElements(
        const QVector<QPoint>& cells) const;
    void syncMaskSliceMapping();
    void chooseCloudColor(bool startColor);
    void updateCloudColorButton(QPushButton* button, const QColor& color,
                                const QString& label);
    void setPreviewLayoutMode(int mode);
    void showPreviewTab(int index);
    void selectJobFromImage(int row, int column);
    void selectImageFromJob(qint64 placeId);
    void showJobStack(qint64 placementId);
    void restoreSettings();
    void saveSettings() const;
    void resetParameters();
    void populateDroppedImages(const QStringList& paths);

private Q_SLOTS:
    void acceptDocument(const ImageDocumentPtr& document);

private:
    void setBusyUi(bool busy);
    void updateGridCountLabel();
    void updateElementPhysicalSize();
    void setTaskDisplay(const QString& title, const QString& phase,
                        qint64 completed, qint64 total);
    void setIdleTaskDisplay();
    void showTerminalTaskDisplay(const QString& text);
    void setElidedText(QLabel* label, const QString& text);
    void refreshElidedText(QLabel* label);
    void updateSelectedCellLabel();
    TraversalMode selectedTraversalMode() const;
    QString pixelDescription(const ImageMetadata& metadata) const;

    TaskManager* tasks_ = nullptr;
    LargeImageView* view_ = nullptr;
    JobPreviewWidget* jobView_ = nullptr;
    PointCloudWidget* pointCloudView_ = nullptr;
    ElementPointCloudWindow* elementCloudWindow_ = nullptr;
    MaskSliceWindow* maskSliceWindow_ = nullptr;
    MaskSliceWindow* jobStackWindow_ = nullptr;
    QWidget* imagePreviewHost_ = nullptr;
    QSplitter* previewSplitter_ = nullptr;
    QTabBar* previewTabs_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QListWidget* mergedList_ = nullptr;
    QPushButton *openButton_=nullptr,*directoryButton_=nullptr,*mergeButton_=nullptr;
    QPushButton *detectButton_=nullptr,*exportButton_=nullptr,*previousButton_=nullptr,*nextButton_=nullptr;
    QPushButton *manualPreviewButton_=nullptr,*pathButton_=nullptr,*cancelButton_=nullptr;
    QPushButton* groupElementsButton_ = nullptr;
    QPushButton *mergedExportButton_=nullptr,*jobImportButton_=nullptr,*resetParametersButton_=nullptr;
    QPushButton* selectedBlocksExportButton_ = nullptr;
    QPushButton* clearFixedGrayPointsButton_ = nullptr;
    QPushButton* generateCloudButton_ = nullptr;
    QPushButton *cloudStartColorButton_=nullptr,*cloudEndColorButton_=nullptr;
    QSpinBox *cellWidth_=nullptr,*cellHeight_=nullptr,*threshold_=nullptr;
    QDoubleSpinBox *cloudPixelScale_=nullptr,*cloudMaximumHeight_=nullptr;
    QCheckBox* autoJumpCheck_=nullptr;
    QComboBox *polarity_=nullptr,*traversal_=nullptr,*previewLayoutMode_=nullptr;
    QLabel *gridCountLabel_=nullptr,*selectedCellLabel_=nullptr,*taskTitle_=nullptr,*taskPhase_=nullptr,*progressPercent_=nullptr,*imageInfo_=nullptr,*zoomInfo_=nullptr;
    QLabel* pixelProbeLabel_ = nullptr;
    QLabel* elementPhysicalSizeLabel_ = nullptr;
    QProgressBar* progress_=nullptr;
    QTimer* taskResetTimer_=nullptr;
    QTimer* jobPreviewTimer_=nullptr;
    QSplitter* mainSplitter_=nullptr;
    ImageDocumentPtr document_;
    JobDocumentPtr jobDocument_;
    PointCloudDocumentPtr pointCloudDocument_;
    QColor cloudStartColor_{0,0,255};
    QColor cloudEndColor_{0,255,0};
    bool invertHeight_ = true;
    SegmentationResult segmentation_;
    ElementGroupingResult elementGrouping_;
    QStringList directoryPaths_;
    QStringList pendingMergePaths_;
    QVector<ImageDocumentPtr> mergedDocuments_;
    QString lastImageDirectory_;
    QString lastDirectoryDirectory_;
    QString lastJobDirectory_;
    QString lastMergedExportDirectory_;
    QString lastSplitExportDirectory_;
    PendingAction pendingAction_ = PendingAction::None;
    quint64 generation_ = 1;
    QString lastTilePhase_;
    qint64 lastTileCompleted_ = -1;
    qint64 lastTileTotal_ = -1;
    quint64 mergePreviewGeneration_ = 0;
    bool switchToJobAfterBuild_ = false;
    bool reverseYSetting_ = false;
    int selectedRow_ = -1;
    int selectedColumn_ = -1;
};
