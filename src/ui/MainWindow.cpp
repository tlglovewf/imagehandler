#include "ui/MainWindow.h"
#include "ui/LargeImageView.h"
#include "tasks/TaskManager.h"
#include "io/DirectoryScanner.h"
#include "io/SplitExporter.h"
#include "imaging/ImageLoader.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/PatternDetector.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), tasks_(new TaskManager(this)) {
    buildUi(); connectUi(); setWindowTitle(QStringLiteral("大图像处理程序")); resize(1440,900);
}

void MainWindow::buildUi() {
    auto* central=new QWidget(this); auto* root=new QVBoxLayout(central); root->setContentsMargins(6,6,6,6);
    auto* tools=new QHBoxLayout;
    openButton_=new QPushButton(QStringLiteral("打开图片")); directoryButton_=new QPushButton(QStringLiteral("打开目录"));
    mergeButton_=new QPushButton(QStringLiteral("图像合并")); detectButton_=new QPushButton(QStringLiteral("分割预览")); exportButton_=new QPushButton(QStringLiteral("导出分割"));
    auto* fit=new QPushButton(QStringLiteral("适应窗口")); auto* actual=new QPushButton(QStringLiteral("1:1"));
    for(auto* b:{openButton_,directoryButton_,mergeButton_,detectButton_,exportButton_,fit,actual}) tools->addWidget(b);
    tools->addStretch(); imageInfo_=new QLabel(QStringLiteral("未加载图像")); zoomInfo_=new QLabel; tools->addWidget(imageInfo_);tools->addWidget(zoomInfo_); root->addLayout(tools);

    auto* splitter=new QSplitter;
    fileList_=new QListWidget; fileList_->setMinimumWidth(190); fileList_->setMaximumWidth(320); splitter->addWidget(fileList_);
    auto* viewerHost=new QWidget; auto* viewerLayout=new QHBoxLayout(viewerHost); viewerLayout->setContentsMargins(0,0,0,0);
    previousButton_=new QPushButton(QStringLiteral("<")); previousButton_->setFixedWidth(36); nextButton_=new QPushButton(QStringLiteral(">"));nextButton_->setFixedWidth(36);
    view_=new LargeImageView; viewerLayout->addWidget(previousButton_);viewerLayout->addWidget(view_,1);viewerLayout->addWidget(nextButton_);splitter->addWidget(viewerHost);
    auto* panel=new QWidget; panel->setMinimumWidth(230); panel->setMaximumWidth(300); auto* panelLayout=new QVBoxLayout(panel);
    auto* form=new QFormLayout;
    cellWidth_=new QSpinBox;cellWidth_->setRange(1,INT_MAX);cellHeight_=new QSpinBox;cellHeight_->setRange(1,INT_MAX);
    threshold_=new QSpinBox;threshold_->setRange(0,255);threshold_->setValue(128);
    polarity_=new QComboBox;polarity_->addItems({QStringLiteral("暗色前景"),QStringLiteral("亮色前景")});
    traversal_=new QComboBox;traversal_->addItems({QStringLiteral("Z 字形（蛇形）"),QStringLiteral("回字形（顺时针）")});
    form->addRow(QStringLiteral("单元宽度"),cellWidth_);form->addRow(QStringLiteral("单元高度"),cellHeight_);form->addRow(QStringLiteral("阈值"),threshold_);form->addRow(QStringLiteral("前景极性"),polarity_);form->addRow(QStringLiteral("导出路径"),traversal_);
    panelLayout->addWidget(new QLabel(QStringLiteral("分割参数")));panelLayout->addLayout(form);panelLayout->addStretch();splitter->addWidget(panel);splitter->setStretchFactor(1,1);root->addWidget(splitter,1);

    auto* taskBar=new QHBoxLayout;taskTitle_=new QLabel(QStringLiteral("就绪"));taskTitle_->setMinimumWidth(150);taskPhase_=new QLabel;taskPhase_->setMinimumWidth(180);
    progress_=new QProgressBar;progress_->setRange(0,100);progress_->setValue(0);progress_->setFormat(QStringLiteral("%p%"));cancelButton_=new QPushButton(QStringLiteral("取消"));cancelButton_->setEnabled(false);
    taskBar->addWidget(taskTitle_);taskBar->addWidget(taskPhase_);taskBar->addWidget(progress_,1);taskBar->addWidget(cancelButton_);root->addLayout(taskBar);setCentralWidget(central);
    connect(fit,&QPushButton::clicked,view_,&LargeImageView::fitToWindow);connect(actual,&QPushButton::clicked,view_,&LargeImageView::actualSize);
}

void MainWindow::connectUi() {
    connect(openButton_,&QPushButton::clicked,this,&MainWindow::openImage);connect(directoryButton_,&QPushButton::clicked,this,&MainWindow::openDirectory);
    connect(mergeButton_,&QPushButton::clicked,this,&MainWindow::mergeImages);connect(detectButton_,&QPushButton::clicked,this,&MainWindow::detectSegmentation);connect(exportButton_,&QPushButton::clicked,this,&MainWindow::chooseExportDirectory);
    connect(previousButton_,&QPushButton::clicked,this,[this]{navigate(-1);});connect(nextButton_,&QPushButton::clicked,this,[this]{navigate(1);});
    connect(fileList_,&QListWidget::itemSelectionChanged,this,&MainWindow::loadCurrentListItem);connect(cancelButton_,&QPushButton::clicked,tasks_,&TaskManager::cancel);
    connect(cellWidth_,qOverload<int>(&QSpinBox::valueChanged),this,&MainWindow::updateGridFromControls);connect(cellHeight_,qOverload<int>(&QSpinBox::valueChanged),this,&MainWindow::updateGridFromControls);
    connect(threshold_,qOverload<int>(&QSpinBox::valueChanged),this,&MainWindow::updateGridFromControls);connect(polarity_,qOverload<int>(&QComboBox::currentIndexChanged),this,&MainWindow::updateGridFromControls);
    connect(view_,&LargeImageView::viewStatusChanged,this,[this](double z,qint64,qint64){zoomInfo_->setText(QStringLiteral("%1%").arg(z*100,0,'f',1));});
    connect(view_,&LargeImageView::tileProgress,this,[this](const QString& phase,qint64 done,qint64 total){if(!tasks_->isBusy()&&total>0&&done<total){taskTitle_->setText(QStringLiteral("后台渲染"));taskPhase_->setText(phase);progress_->setRange(0,int(total));progress_->setValue(int(done));}else if(!tasks_->isBusy()&&done==total){taskTitle_->setText(QStringLiteral("就绪"));taskPhase_->clear();progress_->setRange(0,100);progress_->setValue(100);}});
    connect(tasks_,&TaskManager::taskStarted,this,[this](quint64,const QString& title){setBusyUi(true);taskTitle_->setText(title);taskPhase_->setText(QStringLiteral("准备中"));progress_->setRange(0,0);});
    connect(tasks_,&TaskManager::taskProgress,this,[this](quint64,const TaskProgress& p){taskPhase_->setText(p.phase);if(p.total<=0)progress_->setRange(0,0);else{progress_->setRange(0,1000);progress_->setValue(int(std::clamp<qint64>(p.completed*1000/p.total,0,1000)));}});
    connect(tasks_,&TaskManager::taskCancelled,this,[this](quint64){pendingAction_=PendingAction::None;setBusyUi(false);taskTitle_->setText(QStringLiteral("已取消"));taskPhase_->clear();progress_->setRange(0,100);progress_->setValue(0);});
    connect(tasks_,&TaskManager::taskFailed,this,[this](quint64,const QString& error){
        const auto action=pendingAction_;pendingAction_=PendingAction::None;setBusyUi(false);taskTitle_->setText(QStringLiteral("失败"));taskPhase_->clear();
        if(action==PendingAction::Export&&error.startsWith("TARGETS_EXIST:")) { const int count=error.mid(14).toInt(); if(QMessageBox::question(this,QStringLiteral("确认覆盖"),QStringLiteral("目标目录存在 %1 个同名文件，是否覆盖？").arg(count))==QMessageBox::Yes)startExport(true); }
        else QMessageBox::critical(this,QStringLiteral("操作失败"),error);
    });
    connect(tasks_,&TaskManager::taskFinished,this,[this](quint64,const QVariant& value){
        const auto action=pendingAction_;pendingAction_=PendingAction::None;setBusyUi(false);taskTitle_->setText(QStringLiteral("完成"));taskPhase_->clear();progress_->setRange(0,100);progress_->setValue(100);
        if(action==PendingAction::Scan){directoryPaths_=value.toStringList();fileList_->clear();for(const auto& p:directoryPaths_){auto* item=new QListWidgetItem(QFileInfo(p).fileName(),fileList_);item->setData(Qt::UserRole,p);}if(!directoryPaths_.isEmpty())fileList_->setCurrentRow(0);else QMessageBox::information(this,QStringLiteral("扫描结果"),QStringLiteral("目录中没有支持的图片。"));}
        else if(action==PendingAction::Load||action==PendingAction::Merge)acceptDocument(value.value<ImageDocumentPtr>());
        else if(action==PendingAction::Detect){segmentation_=value.value<SegmentationResult>();cellWidth_->blockSignals(true);cellHeight_->blockSignals(true);threshold_->blockSignals(true);polarity_->blockSignals(true);cellWidth_->setValue(segmentation_.settings.cellWidth);cellHeight_->setValue(segmentation_.settings.cellHeight);threshold_->setValue(segmentation_.settings.threshold);polarity_->setCurrentIndex(segmentation_.settings.polarity==ForegroundPolarity::Dark?0:1);cellWidth_->blockSignals(false);cellHeight_->blockSignals(false);threshold_->blockSignals(false);polarity_->blockSignals(false);view_->setSegmentation(segmentation_);exportButton_->setEnabled(segmentation_.rows>0&&segmentation_.columns>0);if(!segmentation_.warning.isEmpty())QMessageBox::warning(this,QStringLiteral("识别提示"),segmentation_.warning);}
        else if(action==PendingAction::Export)QMessageBox::information(this,QStringLiteral("导出完成"),QStringLiteral("所有分割图片已成功导出。"));
    });
}

void MainWindow::setBusyUi(bool busy) { for(auto* b:{openButton_,directoryButton_,mergeButton_,detectButton_,exportButton_,previousButton_,nextButton_})b->setEnabled(!busy);fileList_->setEnabled(!busy);cancelButton_->setEnabled(busy); }

void MainWindow::openImage(){const QString p=QFileDialog::getOpenFileName(this,QStringLiteral("打开图片"),{},QStringLiteral("图片 (*.bmp *.png *.jpg *.jpeg *.tif *.tiff)"));if(!p.isEmpty()){directoryPaths_.clear();fileList_->clear();loadPath(p);}}
void MainWindow::openDirectory(){const QString d=QFileDialog::getExistingDirectory(this,QStringLiteral("选择图片目录"));if(d.isEmpty())return;pendingAction_=PendingAction::Scan;tasks_->start(QStringLiteral("扫描目录"),[d](const auto&t,const auto&p,QString*){return QVariant(DirectoryScanner::scan(d,t,p));});}
void MainWindow::loadPath(const QString& path){if(tasks_->isBusy())return;pendingAction_=PendingAction::Load;const quint64 gen=generation_++;tasks_->start(QStringLiteral("加载图片"),[path,gen](const auto&t,const auto&p,QString*e){p({QStringLiteral("读取元数据"),0,0});if(t->isCancelled())return QVariant{};auto doc=ImageLoader::load(path,gen,e);p({QStringLiteral("准备首屏预览"),1,1});return QVariant::fromValue(doc);});}
void MainWindow::loadCurrentListItem(){if(auto* item=fileList_->currentItem())loadPath(item->data(Qt::UserRole).toString());}
void MainWindow::navigate(int delta){if(fileList_->count()==0)return;fileList_->setCurrentRow(std::clamp(fileList_->currentRow()+delta,0,fileList_->count()-1));}
void MainWindow::mergeImages(){if(directoryPaths_.isEmpty()){QMessageBox::information(this,QStringLiteral("图像合并"),QStringLiteral("请先打开包含合并图块的目录。"));return;}pendingAction_=PendingAction::Merge;const auto paths=directoryPaths_;const quint64 gen=generation_++;tasks_->start(QStringLiteral("合并图像"),[paths,gen](const auto&t,const auto&p,QString*e){return QVariant::fromValue(MosaicBuilder::build(paths,gen,t,p,e));});}
void MainWindow::detectSegmentation(){if(!document_){QMessageBox::information(this,QStringLiteral("分割预览"),QStringLiteral("请先加载图像。"));return;}pendingAction_=PendingAction::Detect;const auto doc=document_;tasks_->start(QStringLiteral("自动识别分割"),[doc](const auto&t,const auto&p,QString*e){return QVariant::fromValue(PatternDetector::detect(doc,t,p,e));});}
void MainWindow::updateGridFromControls(){if(!document_||cellWidth_->value()<=0||cellHeight_->value()<=0)return;SegmentationSettings s{cellWidth_->value(),cellHeight_->value(),threshold_->value(),polarity_->currentIndex()==0?ForegroundPolarity::Dark:ForegroundPolarity::Light};segmentation_=PatternDetector::makeGrid(document_,s);view_->setSegmentation(segmentation_);exportButton_->setEnabled(segmentation_.rows>0&&segmentation_.columns>0);}
void MainWindow::chooseExportDirectory(){if(!document_||segmentation_.rows<=0||segmentation_.columns<=0){QMessageBox::information(this,QStringLiteral("导出"),QStringLiteral("请先完成分割预览。"));return;}exportDirectory_=QFileDialog::getExistingDirectory(this,QStringLiteral("选择导出目录"));if(!exportDirectory_.isEmpty())startExport(false);}
void MainWindow::startExport(bool overwrite){pendingAction_=PendingAction::Export;const auto doc=document_;const auto seg=segmentation_;const auto dir=exportDirectory_;const auto mode=traversal_->currentIndex()==0?TraversalMode::SnakeZ:TraversalMode::ClockwiseSpiral;tasks_->start(QStringLiteral("导出分割图片"),[doc,seg,dir,mode,overwrite](const auto&t,const auto&p,QString*e){if(!overwrite){int conflicts=0;QDir d(dir);const qint64 count=qint64(seg.rows)*seg.columns;for(qint64 i=1;i<=count;++i){if(t->isCancelled())return QVariant{};if(QFileInfo::exists(d.filePath(QString::number(i)+".bmp")))++conflicts;p({QStringLiteral("检查文件冲突"),i,count});}if(conflicts){*e=QStringLiteral("TARGETS_EXIST:%1").arg(conflicts);return QVariant{};}}if(!SplitExporter::exportTiles(dir,doc,seg,mode,overwrite,t,p,e))return QVariant{};return QVariant(true);});}
void MainWindow::acceptDocument(const ImageDocumentPtr& doc){if(!doc)return;document_=doc;segmentation_={};view_->setDocument(doc);imageInfo_->setText(QStringLiteral("%1 × %2 | %3").arg(doc->metadata.width).arg(doc->metadata.height).arg(pixelDescription(doc->metadata)));detectButton_->setEnabled(ImageLoader::isProcessable(doc->metadata));exportButton_->setEnabled(false);}
QString MainWindow::pixelDescription(const ImageMetadata& m)const{switch(m.kind){case PixelKind::Binary1:return QStringLiteral("1-bit 二值");case PixelKind::Gray8:return QStringLiteral("8-bit 灰度");case PixelKind::Color:return QStringLiteral("彩色");default:return QStringLiteral("不支持");}}
