#pragma once

#include "domain/Models.h"
#include <QMainWindow>

class LargeImageView;
class TaskManager;
class QListWidget;
class QPushButton;
class QProgressBar;
class QLabel;
class QSpinBox;
class QComboBox;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    enum class PendingAction { None, Scan, Load, Merge, Detect, Grid, Export };
    void buildUi();
    void connectUi();
    void openImage();
    void openDirectory();
    void loadPath(const QString& path);
    void loadCurrentListItem();
    void navigate(int delta);
    void mergeImages();
    void detectSegmentation();
    void updateGridFromControls();
    void chooseExportDirectory();
    void startExport(bool overwrite);
    void acceptDocument(const ImageDocumentPtr& document);
    void setBusyUi(bool busy);
    QString pixelDescription(const ImageMetadata& metadata) const;

    TaskManager* tasks_ = nullptr;
    LargeImageView* view_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QPushButton *openButton_=nullptr,*directoryButton_=nullptr,*mergeButton_=nullptr;
    QPushButton *detectButton_=nullptr,*exportButton_=nullptr,*previousButton_=nullptr,*nextButton_=nullptr;
    QPushButton *cancelButton_=nullptr;
    QSpinBox *cellWidth_=nullptr,*cellHeight_=nullptr,*threshold_=nullptr;
    QComboBox *polarity_=nullptr,*traversal_=nullptr;
    QLabel *taskTitle_=nullptr,*taskPhase_=nullptr,*imageInfo_=nullptr,*zoomInfo_=nullptr;
    QProgressBar* progress_=nullptr;
    ImageDocumentPtr document_;
    SegmentationResult segmentation_;
    QStringList directoryPaths_;
    QString exportDirectory_;
    PendingAction pendingAction_ = PendingAction::None;
    quint64 generation_ = 1;
};

