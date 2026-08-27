#pragma once

#include "domain/Models.h"
#include "domain/JobModels.h"
#include <QMainWindow>
#include <QVector>

class LargeImageView;
class JobPreviewWidget;
class TaskManager;
struct MosaicBuildResult;
class QListWidget;
class QPushButton;
class QProgressBar;
class QLabel;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QTimer;
class QEvent;
class QTabBar;
class QSplitter;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class PendingAction { None, Scan, Load, Merge, Detect, Grid, SplitExport, MosaicExport,
                               JobLoad, JobLoadExport, JobPreviewBuild };
    void buildUi();
    void connectUi();
    void openImage();
    void openDirectory();
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
    void chooseExportDirectory();
    void startExport(bool overwrite);
    void chooseJobFile();
    void loadJobFile(const QString& path, bool exported);
    void scheduleJobPreview(bool switchToJob);
    void rebuildJobPreview();
    void acceptJobDocument(const JobDocumentPtr& document, bool preserveLink);
    void setPreviewLayoutMode(int mode);
    void showPreviewTab(int index);
    void selectJobFromImage(int row, int column);
    void selectImageFromJob(qint64 placeId);

private Q_SLOTS:
    void acceptDocument(const ImageDocumentPtr& document);

private:
    void setBusyUi(bool busy);
    void updateGridCountLabel();
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
    QWidget* imagePreviewHost_ = nullptr;
    QSplitter* previewSplitter_ = nullptr;
    QTabBar* previewTabs_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QListWidget* mergedList_ = nullptr;
    QPushButton *openButton_=nullptr,*directoryButton_=nullptr,*mergeButton_=nullptr;
    QPushButton *detectButton_=nullptr,*exportButton_=nullptr,*previousButton_=nullptr,*nextButton_=nullptr;
    QPushButton *manualPreviewButton_=nullptr,*pathButton_=nullptr,*cancelButton_=nullptr;
    QPushButton *mergedExportButton_=nullptr,*jobImportButton_=nullptr;
    QSpinBox *cellWidth_=nullptr,*cellHeight_=nullptr,*threshold_=nullptr;
    QSpinBox *canvasX_=nullptr,*canvasY_=nullptr,*repeatTimes_=nullptr;
    QCheckBox *printModeCheck_=nullptr,*canvasLockCheck_=nullptr,*reverseYCheck_=nullptr;
    QCheckBox* autoJumpCheck_=nullptr;
    QWidget* printOptionsContainer_=nullptr;
    QComboBox *polarity_=nullptr,*traversal_=nullptr,*previewLayoutMode_=nullptr;
    QLabel *gridCountLabel_=nullptr,*selectedCellLabel_=nullptr,*taskTitle_=nullptr,*taskPhase_=nullptr,*progressPercent_=nullptr,*imageInfo_=nullptr,*zoomInfo_=nullptr;
    QProgressBar* progress_=nullptr;
    QTimer* taskResetTimer_=nullptr;
    QTimer* jobPreviewTimer_=nullptr;
    ImageDocumentPtr document_;
    JobDocumentPtr jobDocument_;
    SegmentationResult segmentation_;
    QStringList directoryPaths_;
    QStringList pendingMergePaths_;
    QVector<ImageDocumentPtr> mergedDocuments_;
    QString exportDirectory_;
    PendingAction pendingAction_ = PendingAction::None;
    quint64 generation_ = 1;
    QString lastTilePhase_;
    qint64 lastTileCompleted_ = -1;
    qint64 lastTileTotal_ = -1;
    bool switchToJobAfterBuild_ = false;
    bool lastExportWasPrint_ = false;
    int selectedRow_ = -1;
    int selectedColumn_ = -1;
};
