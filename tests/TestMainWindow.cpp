#include "imaging/VipsSupport.h"
#include "ui/LargeImageView.h"
#include "ui/MainWindow.h"

#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QtTest>

class TestMainWindow final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void listModesAndMergedExportControl();
};

void TestMainWindow::initTestCase() {
    QString error;
    QVERIFY2(VipsSupport::initialize("MainWindowTests", &error), qPrintable(error));
}

void TestMainWindow::cleanupTestCase() { VipsSupport::shutdown(); }

void TestMainWindow::listModesAndMergedExportControl() {
    MainWindow window;
    const auto* directoryList = window.findChild<QListWidget*>(QStringLiteral("directoryImageList"));
    const auto* mergedList = window.findChild<QListWidget*>(QStringLiteral("mergedImageList"));
    const auto* exportButton = window.findChild<QPushButton*>(QStringLiteral("mergedExportButton"));
    const auto* previewButton = window.findChild<QPushButton*>(QStringLiteral("manualSegmentationPreviewButton"));
    const auto* taskTitle = window.findChild<QLabel*>(QStringLiteral("taskTitle"));
    const auto* progress = window.findChild<QProgressBar*>(QStringLiteral("taskProgress"));
    const auto* percent = window.findChild<QLabel*>(QStringLiteral("taskProgressPercent"));
    const auto* gridCount = window.findChild<QLabel*>(QStringLiteral("segmentationGridCount"));
    const auto* cellWidth = window.findChild<QSpinBox*>(QStringLiteral("segmentationCellWidth"));
    const auto* cellHeight = window.findChild<QSpinBox*>(QStringLiteral("segmentationCellHeight"));
    const auto* traversal = window.findChild<QComboBox*>(QStringLiteral("segmentationTraversal"));
    const auto* pathButton = window.findChild<QPushButton*>(QStringLiteral("traversalPathButton"));
    const auto* splitExportButton = window.findChild<QPushButton*>(QStringLiteral("splitExportButton"));
    const auto* selectedCell = window.findChild<QLabel*>(QStringLiteral("selectedSegmentationCell"));
    QVERIFY(directoryList); QVERIFY(mergedList); QVERIFY(exportButton);QVERIFY(previewButton);
    QVERIFY(taskTitle);QVERIFY(progress);QVERIFY(percent);QVERIFY(gridCount);
    QVERIFY(cellWidth);QVERIFY(cellHeight);QVERIFY(traversal);QVERIFY(pathButton);QVERIFY(splitExportButton);QVERIFY(selectedCell);
    QCOMPARE(directoryList->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(mergedList->selectionMode(), QAbstractItemView::SingleSelection);
    QVERIFY(!exportButton->isEnabled());
    QVERIFY(!previewButton->isEnabled());
    QCOMPARE(taskTitle->property("fullText").toString(),QStringLiteral("就绪"));
    QVERIFY(!progress->isVisible());QVERIFY(!percent->isVisible());
    QVERIFY(!progress->isTextVisible());
    QCOMPARE(gridCount->text(),QStringLiteral("-- × -- = --"));
    QVERIFY(gridCount->styleSheet().contains(QStringLiteral("#d32f2f"),Qt::CaseInsensitive));
    QCOMPARE(cellWidth->value(),220);QCOMPARE(cellHeight->value(),220);
    QVERIFY(pathButton->isCheckable());QVERIFY(!pathButton->isEnabled());QVERIFY(!pathButton->isChecked());
    QVERIFY(!splitExportButton->isEnabled());QCOMPARE(selectedCell->text(),QStringLiteral("未选择"));
    QVERIFY(qobject_cast<LargeImageView*>(selectedCell->parentWidget()));
    QVERIFY(selectedCell->testAttribute(Qt::WA_TransparentForMouseEvents));
    QCOMPARE(splitExportButton->parentWidget(),previewButton->parentWidget());
    window.show();QTest::qWait(20);
    QVERIFY(splitExportButton->geometry().top()>=previewButton->geometry().bottom());
    const auto* imageView=qobject_cast<LargeImageView*>(selectedCell->parentWidget());
    QVERIFY(std::abs(selectedCell->geometry().center().x()-imageView->rect().center().x())<=2);
    QVERIFY(selectedCell->geometry().bottom()<imageView->rect().bottom());
}

QTEST_MAIN(TestMainWindow)
#include "TestMainWindow.moc"
