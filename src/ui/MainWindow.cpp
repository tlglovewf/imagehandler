#include "ui/MainWindow.h"
#include "ui/LargeImageView.h"
#include "tasks/TaskManager.h"
#include "domain/MergeInputSelector.h"
#include "io/DirectoryScanner.h"
#include "io/MergedImageExporter.h"
#include "io/SplitExporter.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/PatternDetector.h"

#include <QComboBox>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QItemSelectionModel>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), tasks_(new TaskManager(this)) {
    buildUi(); connectUi(); setBusyUi(false); setIdleTaskDisplay();
    setWindowTitle(QStringLiteral("大图像处理程序")); resize(1440,900);
}

void MainWindow::buildUi() {
    auto* central=new QWidget(this); auto* root=new QVBoxLayout(central); root->setContentsMargins(6,6,6,6);
    auto* tools=new QHBoxLayout;
    openButton_=new QPushButton(QStringLiteral("打开图片")); directoryButton_=new QPushButton(QStringLiteral("打开目录"));
    mergeButton_=new QPushButton(QStringLiteral("图像合并")); detectButton_=new QPushButton(QStringLiteral("分割预览")); exportButton_=new QPushButton(QStringLiteral("导出分割"));
    auto* fit=new QPushButton(QStringLiteral("适应窗口")); auto* actual=new QPushButton(QStringLiteral("1:1"));
    exportButton_->setObjectName(QStringLiteral("splitExportButton"));
    for(auto* b:{openButton_,directoryButton_,mergeButton_,detectButton_,fit,actual}) tools->addWidget(b);
    tools->addStretch(); imageInfo_=new QLabel(QStringLiteral("未加载图像")); zoomInfo_=new QLabel; tools->addWidget(imageInfo_);tools->addWidget(zoomInfo_); root->addLayout(tools);

    auto* splitter=new QSplitter;
    auto* listHost=new QWidget;listHost->setMinimumWidth(210);listHost->setMaximumWidth(340);auto* listLayout=new QVBoxLayout(listHost);listLayout->setContentsMargins(0,0,0,0);
    listLayout->addWidget(new QLabel(QStringLiteral("目录图片（Ctrl/Shift 多选）")));
    fileList_=new QListWidget;fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);fileList_->setObjectName(QStringLiteral("directoryImageList"));listLayout->addWidget(fileList_,3);
    listLayout->addWidget(new QLabel(QStringLiteral("合并结果")));
    mergedList_=new QListWidget;mergedList_->setSelectionMode(QAbstractItemView::SingleSelection);mergedList_->setObjectName(QStringLiteral("mergedImageList"));listLayout->addWidget(mergedList_,2);
    mergedExportButton_=new QPushButton(QStringLiteral("导出选中合并图"));mergedExportButton_->setObjectName(QStringLiteral("mergedExportButton"));listLayout->addWidget(mergedExportButton_);splitter->addWidget(listHost);
    auto* viewerHost=new QWidget; auto* viewerLayout=new QHBoxLayout(viewerHost); viewerLayout->setContentsMargins(0,0,0,0);
    previousButton_=new QPushButton(QStringLiteral("<")); previousButton_->setFixedWidth(36); nextButton_=new QPushButton(QStringLiteral(">"));nextButton_->setFixedWidth(36);
    view_=new LargeImageView;
    selectedCellLabel_=new QLabel(QStringLiteral("未选择"),view_);selectedCellLabel_->setObjectName(QStringLiteral("selectedSegmentationCell"));selectedCellLabel_->setAlignment(Qt::AlignCenter);selectedCellLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);selectedCellLabel_->setStyleSheet(QStringLiteral("QLabel { color: white; background: rgba(0, 0, 0, 150); border-radius: 4px; padding: 5px 12px; }"));
    auto* selectedCellOverlay=new QVBoxLayout(view_);selectedCellOverlay->setContentsMargins(12,12,12,18);selectedCellOverlay->addStretch();selectedCellOverlay->addWidget(selectedCellLabel_,0,Qt::AlignHCenter|Qt::AlignBottom);
    viewerLayout->addWidget(previousButton_);viewerLayout->addWidget(view_,1);viewerLayout->addWidget(nextButton_);splitter->addWidget(viewerHost);
    auto* panel=new QWidget; panel->setMinimumWidth(230); panel->setMaximumWidth(300); auto* panelLayout=new QVBoxLayout(panel);
    auto* form=new QFormLayout;
    cellWidth_=new QSpinBox;cellWidth_->setObjectName(QStringLiteral("segmentationCellWidth"));cellWidth_->setRange(1,INT_MAX);cellWidth_->setValue(220);cellHeight_=new QSpinBox;cellHeight_->setObjectName(QStringLiteral("segmentationCellHeight"));cellHeight_->setRange(1,INT_MAX);cellHeight_->setValue(220);
    threshold_=new QSpinBox;threshold_->setObjectName(QStringLiteral("segmentationThreshold"));threshold_->setRange(0,255);threshold_->setValue(128);
    polarity_=new QComboBox;polarity_->addItems({QStringLiteral("暗色前景"),QStringLiteral("亮色前景")});
    traversal_=new QComboBox;traversal_->setObjectName(QStringLiteral("segmentationTraversal"));traversal_->addItems({QStringLiteral("Z 字形（蛇形）"),QStringLiteral("回字形（顺时针）")});
    form->addRow(QStringLiteral("分割宽度"),cellWidth_);form->addRow(QStringLiteral("分割高度"),cellHeight_);
    gridCountLabel_=new QLabel(QStringLiteral("-- × -- = --"));gridCountLabel_->setObjectName(QStringLiteral("segmentationGridCount"));gridCountLabel_->setStyleSheet(QStringLiteral("QLabel { color: #d32f2f; font-weight: 600; }"));
    form->addRow(QStringLiteral("行列数量"),gridCountLabel_);
    form->addRow(QStringLiteral("阈值"),threshold_);form->addRow(QStringLiteral("前景极性"),polarity_);
    pathButton_=new QPushButton(QStringLiteral("显示路径"));pathButton_->setObjectName(QStringLiteral("traversalPathButton"));pathButton_->setCheckable(true);pathButton_->setEnabled(false);
    auto* traversalRow=new QHBoxLayout;traversalRow->setContentsMargins(0,0,0,0);traversalRow->addWidget(traversal_,1);traversalRow->addWidget(pathButton_);
    form->addRow(QStringLiteral("导出路径"),traversalRow);
    manualPreviewButton_=new QPushButton(QStringLiteral("应用参数并预览分割线"));
    manualPreviewButton_->setObjectName(QStringLiteral("manualSegmentationPreviewButton"));
    panelLayout->addWidget(new QLabel(QStringLiteral("分割参数")));panelLayout->addLayout(form);panelLayout->addWidget(manualPreviewButton_);panelLayout->addWidget(exportButton_);panelLayout->addStretch();splitter->addWidget(panel);splitter->setStretchFactor(1,1);root->addWidget(splitter,1);

    auto* taskBar=new QHBoxLayout;taskBar->setContentsMargins(4,2,4,2);taskBar->setSpacing(10);
    taskTitle_=new QLabel;taskTitle_->setObjectName(QStringLiteral("taskTitle"));taskTitle_->setMinimumWidth(120);taskTitle_->setMaximumWidth(240);taskTitle_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);taskTitle_->installEventFilter(this);
    taskPhase_=new QLabel;taskPhase_->setObjectName(QStringLiteral("taskPhase"));taskPhase_->setMinimumWidth(150);taskPhase_->setMaximumWidth(320);taskPhase_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);taskPhase_->installEventFilter(this);
    progress_=new QProgressBar;progress_->setObjectName(QStringLiteral("taskProgress"));progress_->setTextVisible(false);progress_->setMinimumHeight(22);progress_->setMaximumHeight(22);
    progressPercent_=new QLabel;progressPercent_->setObjectName(QStringLiteral("taskProgressPercent"));progressPercent_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);progressPercent_->setFixedWidth(48);
    cancelButton_=new QPushButton(QStringLiteral("取消任务"));cancelButton_->setMinimumWidth(84);cancelButton_->setEnabled(false);
    taskBar->addWidget(taskTitle_);taskBar->addWidget(taskPhase_);taskBar->addWidget(progress_,1);taskBar->addWidget(progressPercent_);taskBar->addWidget(cancelButton_);root->addLayout(taskBar);setCentralWidget(central);
    taskResetTimer_=new QTimer(this);taskResetTimer_->setSingleShot(true);taskResetTimer_->setInterval(1800);
    connect(fit,&QPushButton::clicked,view_,&LargeImageView::fitToWindow);connect(actual,&QPushButton::clicked,view_,&LargeImageView::actualSize);
}

void MainWindow::connectUi() {
    connect(openButton_,&QPushButton::clicked,this,&MainWindow::openImage);connect(directoryButton_,&QPushButton::clicked,this,&MainWindow::openDirectory);
    connect(mergeButton_,&QPushButton::clicked,this,&MainWindow::mergeImages);connect(detectButton_,&QPushButton::clicked,this,&MainWindow::detectSegmentation);connect(exportButton_,&QPushButton::clicked,this,&MainWindow::chooseExportDirectory);
    connect(mergedExportButton_,&QPushButton::clicked,this,&MainWindow::chooseMergedExportPath);
    connect(previousButton_,&QPushButton::clicked,this,[this]{navigate(-1);});connect(nextButton_,&QPushButton::clicked,this,[this]{navigate(1);});
    connect(fileList_,&QListWidget::itemClicked,this,[this](QListWidgetItem* item){const auto modifiers=QApplication::keyboardModifiers();if(!(modifiers&(Qt::ControlModifier|Qt::ShiftModifier))&&item)loadPath(item->data(Qt::UserRole).toString());});
    connect(mergedList_,&QListWidget::currentRowChanged,this,&MainWindow::showMergedDocument);connect(cancelButton_,&QPushButton::clicked,tasks_,&TaskManager::cancel);
    connect(manualPreviewButton_,&QPushButton::clicked,this,&MainWindow::applyManualSegmentation);
    connect(pathButton_,&QPushButton::toggled,view_,&LargeImageView::setTraversalPathVisible);
    connect(traversal_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{view_->setTraversalMode(selectedTraversalMode());updateSelectedCellLabel();});
    connect(view_,&LargeImageView::segmentationCellSelected,this,[this](int row,int column){selectedRow_=row;selectedColumn_=column;updateSelectedCellLabel();});
    connect(cellWidth_,qOverload<int>(&QSpinBox::valueChanged),this,[this]{updateGridCountLabel();});
    connect(cellHeight_,qOverload<int>(&QSpinBox::valueChanged),this,[this]{updateGridCountLabel();});
    connect(taskResetTimer_,&QTimer::timeout,this,&MainWindow::setIdleTaskDisplay);
    connect(view_,&LargeImageView::viewStatusChanged,this,[this](double z,qint64,qint64){zoomInfo_->setText(QStringLiteral("%1%").arg(z*100,0,'f',1));});
    connect(view_,&LargeImageView::tileError,this,[this](const QString& error){
        statusBar()->showMessage(QStringLiteral("图块加载失败：%1").arg(error), 8000);
    });
    connect(view_,&LargeImageView::tileProgress,this,[this](const QString& phase,qint64 done,qint64 total){
        if(tasks_->isBusy()||total<=0)return;
        if(phase==lastTilePhase_&&done==lastTileCompleted_&&total==lastTileTotal_)return;
        lastTilePhase_=phase;lastTileCompleted_=done;lastTileTotal_=total;
        if(done<total)setTaskDisplay(QStringLiteral("后台渲染"),phase,done,total);
        else showTerminalTaskDisplay(QStringLiteral("可视图块已加载"));
    });
    connect(tasks_,&TaskManager::taskStarted,this,[this](quint64,const QString& title){setBusyUi(true);setTaskDisplay(title,QStringLiteral("准备中"),0,0);});
    connect(tasks_,&TaskManager::taskProgress,this,[this](quint64,const TaskProgress& p){setTaskDisplay(taskTitle_->property("fullText").toString(),p.phase,p.completed,p.total);});
    connect(tasks_,&TaskManager::taskCancelled,this,[this](quint64){pendingMergePaths_.clear();pendingAction_=PendingAction::None;setBusyUi(false);showTerminalTaskDisplay(QStringLiteral("已取消"));});
    connect(tasks_,&TaskManager::taskFailed,this,[this](quint64,const QString& error){
        const auto action=pendingAction_;pendingAction_=PendingAction::None;if(action==PendingAction::Merge)pendingMergePaths_.clear();setBusyUi(false);showTerminalTaskDisplay(QStringLiteral("失败"));
        if(action==PendingAction::SplitExport&&error.startsWith("TARGETS_EXIST:")) { const qint64 count=error.mid(14).toLongLong(); if(QMessageBox::question(this,QStringLiteral("确认覆盖"),QStringLiteral("目标目录存在 %1 个同名文件，是否覆盖？").arg(count))==QMessageBox::Yes)startExport(true); }
        else QMessageBox::critical(this,QStringLiteral("操作失败"),error);
    });
    connect(tasks_,&TaskManager::taskFinished,this,[this](quint64,const QVariant& value){
        const auto action=pendingAction_;pendingAction_=PendingAction::None;setBusyUi(false);showTerminalTaskDisplay(QStringLiteral("完成"));
        if(action==PendingAction::Scan){directoryPaths_=value.toStringList();fileList_->clear();for(const auto& p:directoryPaths_){auto* item=new QListWidgetItem(QFileInfo(p).fileName(),fileList_);item->setData(Qt::UserRole,p);}if(!directoryPaths_.isEmpty()){fileList_->setCurrentRow(0,QItemSelectionModel::ClearAndSelect);loadCurrentListItem();}else QMessageBox::information(this,QStringLiteral("扫描结果"),QStringLiteral("目录中没有支持的图片。"));}
        else if(action==PendingAction::Load)acceptDocument(value.value<ImageDocumentPtr>());
        else if(action==PendingAction::Merge)acceptMosaicResult(value.value<MosaicBuildResult>());
        else if(action==PendingAction::Detect){segmentation_=value.value<SegmentationResult>();for(auto*s:{cellWidth_,cellHeight_,threshold_})s->blockSignals(true);polarity_->blockSignals(true);cellWidth_->setValue(segmentation_.settings.cellWidth);cellHeight_->setValue(segmentation_.settings.cellHeight);threshold_->setValue(segmentation_.settings.threshold);polarity_->setCurrentIndex(segmentation_.settings.polarity==ForegroundPolarity::Dark?0:1);for(auto*s:{cellWidth_,cellHeight_,threshold_})s->blockSignals(false);polarity_->blockSignals(false);updateGridCountLabel();const bool valid=segmentation_.rows>0&&segmentation_.columns>0;if(!valid)pathButton_->setChecked(false);view_->setSegmentation(segmentation_);view_->setTraversalMode(selectedTraversalMode());view_->setTraversalPathVisible(pathButton_->isChecked());exportButton_->setEnabled(valid);pathButton_->setEnabled(valid);if(!segmentation_.warning.isEmpty())QMessageBox::warning(this,QStringLiteral("识别提示"),segmentation_.warning);}
        else if(action==PendingAction::SplitExport)QMessageBox::information(this,QStringLiteral("导出完成"),QStringLiteral("所有分割图片已成功导出。"));
        else if(action==PendingAction::MosaicExport)QMessageBox::information(this,QStringLiteral("导出完成"),QStringLiteral("合并图已成功导出为 8-bit BMP。"));
    });
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        if (watched == taskTitle_) refreshElidedText(taskTitle_);
        else if (watched == taskPhase_) refreshElidedText(taskPhase_);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setElidedText(QLabel* label, const QString& text) {
    if (!label) return;
    label->setProperty("fullText", text);
    label->setToolTip(text);
    refreshElidedText(label);
}

void MainWindow::refreshElidedText(QLabel* label) {
    if (!label) return;
    const QString fullText = label->property("fullText").toString();
    const int availableWidth = std::max(0, label->width() - label->contentsMargins().left() -
                                           label->contentsMargins().right());
    label->setText(label->fontMetrics().elidedText(fullText, Qt::ElideRight, availableWidth));
}

void MainWindow::setTaskDisplay(const QString& title, const QString& phase,
                                qint64 completed, qint64 total) {
    taskResetTimer_->stop();
    setElidedText(taskTitle_, title);
    setElidedText(taskPhase_, phase);
    progress_->show();
    progressPercent_->show();
    if (total <= 0) {
        progress_->setRange(0, 0);
        progressPercent_->setText(QStringLiteral("--"));
        return;
    }
    const double ratio = std::clamp(double(completed) / double(total), 0.0, 1.0);
    progress_->setRange(0, 1000);
    progress_->setValue(int(std::lround(ratio * 1000.0)));
    progressPercent_->setText(QStringLiteral("%1%").arg(int(std::lround(ratio * 100.0))));
}

void MainWindow::setIdleTaskDisplay() {
    taskResetTimer_->stop();
    setElidedText(taskTitle_, QStringLiteral("就绪"));
    setElidedText(taskPhase_, {});
    progress_->hide();
    progressPercent_->hide();
}

void MainWindow::showTerminalTaskDisplay(const QString& text) {
    setElidedText(taskTitle_, text);
    setElidedText(taskPhase_, {});
    progress_->hide();
    progressPercent_->hide();
    taskResetTimer_->start();
}

void MainWindow::updateGridCountLabel() {
    if (!document_ || cellWidth_->value() <= 0 || cellHeight_->value() <= 0) {
        gridCountLabel_->setText(QStringLiteral("-- × -- = --"));
        return;
    }
    const SegmentationSettings settings{cellWidth_->value(), cellHeight_->value(),
                                        threshold_->value(),
                                        polarity_->currentIndex() == 0
                                            ? ForegroundPolarity::Dark
                                            : ForegroundPolarity::Light};
    const SegmentationResult grid = PatternDetector::makeManualGrid(document_, settings);
    if (grid.columns <= 0 || grid.rows <= 0) {
        gridCountLabel_->setText(QStringLiteral("-- × -- = --"));
        return;
    }
    const qint64 total = qint64(grid.columns) * grid.rows;
    gridCountLabel_->setText(QStringLiteral("%1 × %2 = %3")
                                 .arg(grid.columns).arg(grid.rows).arg(total));
}

void MainWindow::setBusyUi(bool busy) { openButton_->setEnabled(!busy);directoryButton_->setEnabled(!busy);mergeButton_->setEnabled(!busy&&!directoryPaths_.isEmpty());detectButton_->setEnabled(!busy&&document_&&ImageLoader::isProcessable(document_->metadata));manualPreviewButton_->setEnabled(!busy&&document_&&ImageLoader::isProcessable(document_->metadata));const bool hasSegmentation=segmentation_.rows>0&&segmentation_.columns>0;exportButton_->setEnabled(!busy&&hasSegmentation);pathButton_->setEnabled(!busy&&hasSegmentation);previousButton_->setEnabled(!busy&&!directoryPaths_.isEmpty());nextButton_->setEnabled(!busy&&!directoryPaths_.isEmpty());mergedExportButton_->setEnabled(!busy&&mergedList_->currentRow()>=0);fileList_->setEnabled(!busy);mergedList_->setEnabled(!busy);cancelButton_->setEnabled(busy); }

void MainWindow::openImage(){const QString p=QFileDialog::getOpenFileName(this,QStringLiteral("打开图片"),{},QStringLiteral("图片 (*.bmp *.png *.jpg *.jpeg *.tif *.tiff)"));if(!p.isEmpty()){directoryPaths_.clear();fileList_->clear();loadPath(p);}}
void MainWindow::openDirectory(){const QString d=QFileDialog::getExistingDirectory(this,QStringLiteral("选择图片目录"));if(d.isEmpty())return;pendingAction_=PendingAction::Scan;tasks_->start(QStringLiteral("扫描目录"),[d](const auto&t,const auto&p,QString*){return QVariant(DirectoryScanner::scan(d,t,p));});}
void MainWindow::loadPath(const QString& path){if(tasks_->isBusy())return;pendingAction_=PendingAction::Load;const quint64 gen=generation_++;tasks_->start(QStringLiteral("加载图片"),[path,gen](const auto&t,const auto&p,QString*e){p({QStringLiteral("读取元数据"),0,0});if(t->isCancelled())return QVariant{};auto doc=ImageLoader::load(path,gen,e);p({QStringLiteral("准备首屏预览"),1,1});return QVariant::fromValue(doc);});}
void MainWindow::loadCurrentListItem(){if(auto* item=fileList_->currentItem())loadPath(item->data(Qt::UserRole).toString());}
void MainWindow::navigate(int delta){if(fileList_->count()==0)return;const int row=std::clamp(fileList_->currentRow()+delta,0,fileList_->count()-1);fileList_->setCurrentRow(row,QItemSelectionModel::ClearAndSelect);loadCurrentListItem();}
void MainWindow::mergeImages(){if(directoryPaths_.isEmpty()){QMessageBox::information(this,QStringLiteral("图像合并"),QStringLiteral("请先打开包含合并图块的目录。"));return;}QStringList selectedPaths;for(const auto* item:fileList_->selectedItems())selectedPaths<<item->data(Qt::UserRole).toString();pendingMergePaths_=MergeInputSelector::resolve(directoryPaths_,selectedPaths);if(pendingMergePaths_.isEmpty()){QMessageBox::information(this,QStringLiteral("图像合并"),QStringLiteral("当前目录中没有可合并的 BMP 图片。"));return;}startMosaicBuild(false);}
void MainWindow::startMosaicBuild(bool allowMissingTiles){if(tasks_->isBusy()||pendingMergePaths_.isEmpty())return;pendingAction_=PendingAction::Merge;const auto paths=pendingMergePaths_;const quint64 gen=generation_++;const MosaicBuildOptions options{allowMissingTiles};tasks_->start(QStringLiteral("合并图像"),[paths,gen,options](const auto&t,const auto&p,QString*e){return QVariant::fromValue(MosaicBuilder::build(paths,gen,options,t,p,e));});}
void MainWindow::acceptMosaicResult(const MosaicBuildResult& result){if(result.status==MosaicBuildStatus::MissingTiles){QMessageBox warning(this);warning.setIcon(QMessageBox::Warning);warning.setWindowTitle(QStringLiteral("合并图像存在缺块"));QString detail=result.missingCoordinateSamples.join(QStringLiteral("、"));if(result.missingCount>result.missingCoordinateSamples.size())detail+=QStringLiteral(" 等");warning.setText(QStringLiteral("检测到 %1 个缺失坐标：%2\n继续后缺块及未覆盖区域将使用白色填充。").arg(result.missingCount).arg(detail));auto* continueButton=warning.addButton(QStringLiteral("同意并继续"),QMessageBox::AcceptRole);warning.addButton(QStringLiteral("取消"),QMessageBox::RejectRole);warning.exec();if(warning.clickedButton()==continueButton){startMosaicBuild(true);}else{pendingMergePaths_.clear();showTerminalTaskDisplay(QStringLiteral("已取消合并"));setBusyUi(false);}return;}if(!result.document){pendingMergePaths_.clear();return;}auto document=result.document;const int number=mergedDocuments_.size()+1;document->displayName=QStringLiteral("合并结果 %1（%2×%3，%4 张）").arg(number).arg(result.rows).arg(result.columns).arg(result.sourceCount);mergedDocuments_.push_back(document);auto* item=new QListWidgetItem(document->displayName,mergedList_);item->setData(Qt::UserRole,mergedDocuments_.size()-1);pendingMergePaths_.clear();mergedList_->setCurrentRow(mergedList_->count()-1);setBusyUi(false);}
void MainWindow::showMergedDocument(int row){if(row<0||row>=mergedDocuments_.size())return;acceptDocument(mergedDocuments_[row]);setBusyUi(tasks_->isBusy());}
void MainWindow::chooseMergedExportPath(){const int row=mergedList_->currentRow();if(row<0||row>=mergedDocuments_.size())return;QString defaultName=QStringLiteral("合并结果_%1.bmp").arg(row+1);QString path=QFileDialog::getSaveFileName(this,QStringLiteral("导出合并图"),defaultName,QStringLiteral("8-bit BMP (*.bmp)"));if(path.isEmpty())return;if(QFileInfo(path).suffix().compare(QStringLiteral("bmp"),Qt::CaseInsensitive)!=0)path+=QStringLiteral(".bmp");const auto document=mergedDocuments_[row];pendingAction_=PendingAction::MosaicExport;tasks_->start(QStringLiteral("导出合并图"),[path,document](const auto&t,const auto&p,QString*e){if(!MergedImageExporter::writeGray8Bmp(path,document,t,p,e))return QVariant{};return QVariant(true);});}
void MainWindow::detectSegmentation(){if(!document_){QMessageBox::information(this,QStringLiteral("分割预览"),QStringLiteral("请先加载图像。"));return;}pendingAction_=PendingAction::Detect;const auto doc=document_;tasks_->start(QStringLiteral("自动识别分割"),[doc](const auto&t,const auto&p,QString*e){return QVariant::fromValue(PatternDetector::detect(doc,t,p,e));});}
void MainWindow::applyManualSegmentation(){if(!document_||cellWidth_->value()<=0||cellHeight_->value()<=0)return;const SegmentationSettings settings{cellWidth_->value(),cellHeight_->value(),threshold_->value(),polarity_->currentIndex()==0?ForegroundPolarity::Dark:ForegroundPolarity::Light};const auto grid=PatternDetector::makeManualGrid(document_,settings);updateGridCountLabel();if(grid.rows<=0||grid.columns<=0){segmentation_=grid;pathButton_->setChecked(false);view_->clearSegmentation();exportButton_->setEnabled(false);pathButton_->setEnabled(false);statusBar()->showMessage(grid.warning,8000);return;}segmentation_=grid;view_->setSegmentation(segmentation_);view_->setTraversalMode(selectedTraversalMode());view_->setTraversalPathVisible(pathButton_->isChecked());exportButton_->setEnabled(true);pathButton_->setEnabled(true);if(grid.warning.isEmpty())statusBar()->clearMessage();else statusBar()->showMessage(grid.warning,8000);}
void MainWindow::chooseExportDirectory(){if(!document_||segmentation_.rows<=0||segmentation_.columns<=0){QMessageBox::information(this,QStringLiteral("导出"),QStringLiteral("请先完成分割预览。"));return;}const qint64 count=qint64(segmentation_.rows)*segmentation_.columns;if(count>kMaximumSegmentationCells&&QMessageBox::warning(this,QStringLiteral("确认超大批量导出"),QStringLiteral("当前参数将生成 %1 个 BMP 文件。导出可能占用大量时间和磁盘空间，是否继续？").arg(count),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;exportDirectory_=QFileDialog::getExistingDirectory(this,QStringLiteral("选择导出目录"));if(!exportDirectory_.isEmpty())startExport(false);}
void MainWindow::startExport(bool overwrite){pendingAction_=PendingAction::SplitExport;const auto doc=document_;const auto seg=segmentation_;const auto dir=exportDirectory_;const auto mode=traversal_->currentIndex()==0?TraversalMode::SnakeZ:TraversalMode::ClockwiseSpiral;tasks_->start(QStringLiteral("导出分割图片"),[doc,seg,dir,mode,overwrite](const auto&t,const auto&p,QString*e){if(!overwrite){qint64 conflicts=0;QDir d(dir);const qint64 count=qint64(seg.rows)*seg.columns;qint64 lastPermille=-1;for(qint64 i=1;i<=count;++i){if(t->isCancelled())return QVariant{};if(QFileInfo::exists(d.filePath(QString::number(i)+".bmp")))++conflicts;const qint64 permille=i*1000/std::max<qint64>(1,count);if(permille!=lastPermille){lastPermille=permille;p({QStringLiteral("检查文件冲突"),i,count});}}if(conflicts){*e=QStringLiteral("TARGETS_EXIST:%1").arg(conflicts);return QVariant{};}}if(!SplitExporter::exportTiles(dir,doc,seg,mode,overwrite,t,p,e))return QVariant{};return QVariant(true);});}
void MainWindow::acceptDocument(const ImageDocumentPtr& doc){if(!doc)return;document_=doc;segmentation_={};selectedRow_=-1;selectedColumn_=-1;pathButton_->setChecked(false);lastTilePhase_.clear();lastTileCompleted_=-1;lastTileTotal_=-1;view_->setDocument(doc);updateSelectedCellLabel();imageInfo_->setText(QStringLiteral("%1 × %2 | %3").arg(doc->metadata.width).arg(doc->metadata.height).arg(pixelDescription(doc->metadata)));updateGridCountLabel();setBusyUi(tasks_->isBusy());}
TraversalMode MainWindow::selectedTraversalMode() const{return traversal_->currentIndex()==0?TraversalMode::SnakeZ:TraversalMode::ClockwiseSpiral;}
void MainWindow::updateSelectedCellLabel(){if(!selectedCellLabel_)return;if(selectedRow_<0||selectedColumn_<0||segmentation_.rows<=0||segmentation_.columns<=0){selectedCellLabel_->setText(QStringLiteral("未选择"));return;}const qint64 index=SplitExporter::traversalIndex(segmentation_.rows,segmentation_.columns,selectedTraversalMode(),selectedRow_,selectedColumn_);if(index<0){selectedCellLabel_->setText(QStringLiteral("未选择"));return;}selectedCellLabel_->setText(QStringLiteral("%1x%2 : %3.bmp").arg(selectedRow_+1).arg(selectedColumn_+1).arg(index+1));}
QString MainWindow::pixelDescription(const ImageMetadata& m)const{switch(m.kind){case PixelKind::Binary1:return QStringLiteral("1-bit 二值");case PixelKind::Gray8:return QStringLiteral("8-bit 灰度");case PixelKind::Color:return QStringLiteral("彩色");default:return QStringLiteral("不支持");}}
