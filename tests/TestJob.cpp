#include "domain/MaskSliceModels.h"
#include "imaging/JobPreviewBuilder.h"
#include "imaging/JobLabelLod.h"
#include "imaging/JobPreviewMath.h"
#include "imaging/JobSpatialIndex.h"
#include "imaging/JobStackLod.h"
#include "imaging/VipsSupport.h"
#include "io/JobFileLoader.h"
#include "io/JobPackageLoader.h"

#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>

class TestJob final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void jsonAndAldlUseSameParser();
    void absoluteMoveUsesWhere();
    void validatesStructureAndIds();
    void supportsEscapesAndUnknownFields();
    void cancellationDoesNotReturnDocument();
    void previewMatchesExporterCoordinates();
    void labelLodThresholds();
    void projectedSpacingEnablesRangeLabels();
    void rayQueryPrioritizesNearestPoints();
    void vulkanScreenCoordinatesKeepYDirection();
    void guidesUseCoordinateOriginAndFullBounds();
    void coordinateDisplayTransformMovesScene();
    void coordinateOriginRemapsAcrossExportFrames();
    void terrainCameraIsGroundOrientedAndClamped();
    void terrainCameraContinuousOrbitCrossesPoles();
    void terrainCameraPanNearHorizonStaysBounded();
    void terrainCameraPanKeepsGroundAnchor();
    void millionPointPreviewScale();
    void layeredPackageLoadsStacksAndSceneBounds();
    void modelPathsAssignStablePointColors();
    void layeredPackageKeepsSharedLodDetail();
    void processingPackageLoadsFirstLayers();
    void processingMaskKeepsRasterYOrder();
    void processingPackageCentersLegacyExportedMasks();
    void processingPackageInfersGeometryWithoutReport();
    void processingPackageRejectsIncompleteInputs();
    void incompleteLayeredPackageFallsBackToPoints();
    void stackLodPyramidAndFrameBudget();
    void placementVisibilityUsesEntityBounds();
    void largePlacementVisibilityIsNotSpatiallyTruncated();
};

namespace {
QString writeJob(QTemporaryDir& directory, const QString& name, const QByteArray& data) {
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return {};
    file.close();
    return path;
}

QByteArray validJob() {
    return QByteArrayLiteral(R"({
      "models": [
        {"label":"0","path":"0"},
        {"label":"1","path":"1"}
      ],
      "commands": [
        {"command":"relmove","by":[1.5,-2.0,3e0]},
        {"command":"place","id":0},
        {"command":"relmove","by":[4,5,6]},
        {"command":"place","id":1}
      ]
    })");
}

QByteArray packageReport(int layers = 2, bool invertMask = false) {
    return QStringLiteral(R"({
      "schemaVersion": 3,
      "mode": "layered_package",
      "mapping": {"pixel2phy": 0.5},
      "layers": {"layerHeight": 0.25, "count": %1, "invertMask": %2},
      "output": {"attachToCanvas": false, "standardWidth": 2,
                 "standardHeight": 2, "canvasWidth": 8, "canvasHeight": 6,
                 "sourceCellWidth": 2, "sourceCellHeight": 2,
                 "bitsPerPixel": 1}
    })").arg(layers).arg(invertMask ? QStringLiteral("true") : QStringLiteral("false")).toUtf8();
}

bool writeBinaryMask(const QString& path, const QList<QPoint>& whitePixels) {
    QImage image(2, 2, QImage::Format_Mono);
    image.setColorTable({qRgb(0, 0, 0), qRgb(255, 255, 255)});
    image.fill(0);
    for (const QPoint& point : whitePixels) image.setPixel(point, 1);
    return image.save(path, "BMP");
}
}

void TestJob::initTestCase() {
    QString error;
    QVERIFY2(VipsSupport::initialize("JobDataTests", &error), qPrintable(error));
}

void TestJob::cleanupTestCase() { VipsSupport::shutdown(); }

void TestJob::jsonAndAldlUseSameParser() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString jsonPath = writeJob(directory, QStringLiteral("job.json"), validJob());
    const QString aldlPath = writeJob(directory, QStringLiteral("job.aldl"), validJob());
    QVERIFY(!jsonPath.isEmpty()); QVERIFY(!aldlPath.isEmpty());
    QString error;
    const auto token = std::make_shared<CancellationToken>();
    const auto json = JobFileLoader::load(jsonPath, token, {}, &error);
    QVERIFY2(json, qPrintable(error));
    const auto aldl = JobFileLoader::load(aldlPath, token, {}, &error);
    QVERIFY2(aldl, qPrintable(error));
    QCOMPARE(json->models.size(), aldl->models.size());
    QCOMPARE(json->points.size(), size_t(2));
    QCOMPARE(json->points[0].position, (JobCoordinate{1.5, -2.0, 3.0}));
    QCOMPARE(json->points[1].placeId, qint64(1));
    QVERIFY(!json->isLinked());
}

void TestJob::absoluteMoveUsesWhere() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = writeJob(directory, QStringLiteral("absolute.json"),
        QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[{"command":"absmove","where":[1.5,-2,3]},{"command":"place","id":0}]})"));
    QString error;
    const auto document = JobFileLoader::load(path, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->points[0].position, (JobCoordinate{1.5, -2.0, 3.0}));
}

void TestJob::validatesStructureAndIds() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    struct Case { QByteArray data; QString expected; };
    const QList<Case> cases = {
        {QByteArrayLiteral(R"({"commands":[]})"), QStringLiteral("同时包含")},
        {QByteArrayLiteral(R"({"models":[],"commands":[]})"), QStringLiteral("不包含")},
        {QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[{"command":"place","id":0}]})"), QStringLiteral("期望 relmove")},
        {QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[{"command":"move","by":[0,0,0]},{"command":"place","id":0}]})"), QStringLiteral("不支持的命令")},
        {QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[{"command":"relmove","by":[0,0]},{"command":"place","id":0}]})"), QStringLiteral("坐标")},
        {QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[{"command":"relmove","by":[0,0,0]},{"command":"place","id":2}]})"), QStringLiteral("超出模型范围")},
        {QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[)"), QStringLiteral("字节")}
    };
    for (int i = 0; i < cases.size(); ++i) {
        const QString path = writeJob(directory, QStringLiteral("bad_%1.json").arg(i), cases[i].data);
        QString error;
        QVERIFY(!JobFileLoader::load(path, std::make_shared<CancellationToken>(), {}, &error));
        QVERIFY2(error.contains(cases[i].expected), qPrintable(error));
    }
    const QByteArray reused = QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[{"command":"relmove","by":[0,0,0]},{"command":"place","id":0},{"command":"relmove","by":[1,0,0]},{"command":"place","id":0}]})");
    const QString reusedPath = writeJob(directory, QStringLiteral("reused.json"), reused);
    QString reusedError;
    const auto reusedDocument = JobFileLoader::load(
        reusedPath, std::make_shared<CancellationToken>(), {}, &reusedError);
    QVERIFY2(reusedDocument, qPrintable(reusedError));
    QCOMPARE(reusedDocument->models.size(), size_t(1));
    QCOMPARE(reusedDocument->points.size(), size_t(2));
    QCOMPARE(reusedDocument->points[0].placeId, qint64(0));
    QCOMPARE(reusedDocument->points[1].placeId, qint64(0));
    QCOMPARE(reusedDocument->points[1].placementId, qint64(1));
}

void TestJob::supportsEscapesAndUnknownFields() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QByteArray data = QByteArrayLiteral(R"({"version":1,"models":[{"label":"\u4e2d\u6587","path":"a\\b","extra":{"x":true}}],"commands":[{"command":"relmove","by":[0,-0.25,1],"note":null},{"command":"place","id":0}],"tail":[false]})");
    const QString path = writeJob(directory, QStringLiteral("escaped.aldl"), data);
    QString error;
    const auto document = JobFileLoader::load(path, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->models[0].label, QStringLiteral("中文"));
    QCOMPARE(document->models[0].path, QStringLiteral("a\\b"));
}

void TestJob::cancellationDoesNotReturnDocument() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = writeJob(directory, QStringLiteral("job.json"), validJob());
    auto token = std::make_shared<CancellationToken>(); token->cancel();
    QString error;
    QVERIFY(!JobFileLoader::load(path, token, {}, &error));
}

void TestJob::layeredPackageLoadsStacksAndSceneBounds() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray job = QByteArrayLiteral(R"({
      "models":[{"label":"shared","path":"0"}],
      "commands":[
        {"command":"relmove","by":[0,0,0]},{"command":"place","id":0},
        {"command":"relmove","by":[2,3,1]},{"command":"place","id":0}
      ]
    })");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"), packageReport()).isEmpty());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    const QDir model(directory.filePath(QStringLiteral("0")));
    QVERIFY(writeBinaryMask(model.filePath(QStringLiteral("1.bmp")),
                            {{0,0},{1,0},{0,1},{1,1}}));
    QVERIFY(writeBinaryMask(model.filePath(QStringLiteral("2.bmp")), {{1,0}}));

    QString error;
    const auto document = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QVERIFY(!document->layeredPackageDetected);
    QCOMPARE(document->loadedStackCount(), 0);
    QVERIFY(document->packageWarnings.isEmpty());
    QCOMPARE(document->models[0].stackState, JobModelStackState::NotDetected);
    const auto loaded = JobPackageLoader::loadStack(
        document, 0, 17, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(loaded.preview, qPrintable(error));
    QCOMPARE(loaded.requestGeneration, quint64(17));
    QCOMPARE(loaded.placementId, qint64(0));
    QCOMPARE(loaded.modelPath, QStringLiteral("0"));
    QCOMPARE(loaded.preview->requestedOutputSize, QSize(2,2));
    QCOMPARE(loaded.preview->gridSize, QSize(2,2));
    QCOMPARE(loaded.preview->physicalSize, QSizeF(1,1));
    QCOMPARE(loaded.preview->layerCount, 2);
    QCOMPARE(loaded.preview->layerHeight, 0.25);
    QCOMPARE(loaded.thumbnails.size(), 2);
    QCOMPARE(loaded.thumbnails[0].size(), QSize(2,2));
    QCOMPARE(loaded.thumbnails[0].pixelColor(0,0), QColor(Qt::white));
    QCOMPARE(loaded.thumbnails[0].pixelColor(1,1), QColor(Qt::white));
    QCOMPARE(loaded.thumbnails[1].pixelColor(1,0), QColor(Qt::white));
    QCOMPARE(loaded.thumbnails[1].pixelColor(0,0), QColor(Qt::black));
    QVERIFY(loaded.preview->solidGeometry);
    QVERIFY(loaded.preview->explodedGeometry);
    QVERIFY(!loaded.preview->solidGeometry->indices.empty());
    QCOMPARE(document->sceneBounds.minimum, (JobCoordinate{0,0,0}));
    QCOMPARE(document->sceneBounds.maximum, (JobCoordinate{2,3,1}));

    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"),
                      packageReport(2, true)).isEmpty());
    QVERIFY(writeBinaryMask(model.filePath(QStringLiteral("1.bmp")), {}));
    QVERIFY(writeBinaryMask(model.filePath(QStringLiteral("2.bmp")),
                            {{0,0},{0,1},{1,1}}));
    const auto inverted = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(inverted, qPrintable(error));
    const auto invertedStack = JobPackageLoader::loadStack(
        inverted, 1, 18, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(invertedStack.preview, qPrintable(error));
    QVERIFY(invertedStack.preview->solidGeometry);
    QVERIFY(!invertedStack.preview->solidGeometry->indices.empty());
}

void TestJob::modelPathsAssignStablePointColors() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray job = QByteArrayLiteral(R"({
      "models":[{"label":"a","path":"folder\\A"},
                {"label":"same","path":"folder/A"},
                {"label":"b","path":"folder/B"}],
      "commands":[
        {"command":"relmove","by":[0,0,0]},{"command":"place","id":0},
        {"command":"relmove","by":[1,0,0]},{"command":"place","id":1},
        {"command":"relmove","by":[2,0,0]},{"command":"place","id":2}
      ]})");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    QString error;
    const auto first = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    const auto second = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(first && second, qPrintable(error));
    QVERIFY(first->points[0].groupColor.isValid());
    QCOMPARE(first->points[0].groupColor, first->points[1].groupColor);
    QVERIFY(first->points[0].groupColor != first->points[2].groupColor);
    for (std::size_t index = 0; index < first->points.size(); ++index)
        QCOMPARE(first->points[index].groupColor, second->points[index].groupColor);
}

void TestJob::stackLodPyramidAndFrameBudget() {
    const QSize baseSize(128,64);
    QVector<quint16> heights(baseSize.width()*baseSize.height(),0);
    heights[baseSize.width()*17+93]=77;
    const auto levels=JobStackLod::buildPyramid(
        baseSize,heights,std::make_shared<CancellationToken>());
    QCOMPARE(levels.size(),size_t(3));
    QCOMPARE(levels.front().gridSize,QSize(32,16));
    QCOMPARE(levels.back().gridSize,baseSize);
    for(const auto& level:levels) {
        QVERIFY(std::find(level.columnLayers.cbegin(),level.columnLayers.cend(),quint16(77))!=
                level.columnLayers.cend());
        const int mappedX=93*level.gridSize.width()/baseSize.width();
        const int mappedY=17*level.gridSize.height()/baseSize.height();
        QCOMPARE(level.columnLayers[static_cast<std::size_t>(mappedY)*
                                    level.gridSize.width()+mappedX],quint16(77));
        QCOMPARE(level.columnLayers[static_cast<std::size_t>(
                     level.gridSize.height()-1-mappedY)*level.gridSize.width()+mappedX],quint16(0));
    }
    QCOMPARE(JobStackLod::chooseLevel(levels,20,10,true),0);
    QCOMPARE(JobStackLod::chooseLevel(levels,120,60,false),2);

    JobFrameBudgetController budget;
    const qint64 initial=budget.triangleBudget();
    for(int i=0;i<3;++i)budget.recordFrame(20.0,true);
    QVERIFY(budget.triangleBudget()<initial);QVERIFY(budget.qualityOffset()<0);
    const qint64 reduced=budget.triangleBudget();
    budget.recordFrame(40.0,true);
    QVERIFY(budget.triangleBudget()<reduced);
    budget.reset();
    budget.recordFrame(500.0,true);
    QVERIFY(budget.triangleBudget()<initial);
    budget.reset();
    for(int i=0;i<30;++i)budget.recordFrame(10.0,false);
    QVERIFY(budget.triangleBudget()>initial);
    QVERIFY(budget.framesPerSecond()>90.0);
}

void TestJob::placementVisibilityUsesEntityBounds() {
    auto stack=std::make_shared<JobModelStack>();
    stack->physicalSize={2.5,0.5};stack->layerCount=2;stack->layerHeight=0.25;
    JobDocument document;
    document.models.push_back({0,QStringLiteral("0"),QStringLiteral("0"),
                               JobModelStackState::Loaded,stack,{}});
    // The Job point is the centre: a width-2.5 placement centred at x=2 still
    // reaches the identity frustum at x=0.75. Corner anchoring would cull it.
    document.points.push_back({0,{2.0,0.0,0.0},0,0,{}});
    const auto token=std::make_shared<CancellationToken>();
    document.placementSpatialIndex=JobPlacementSpatialIndex::build(document,token);
    QVERIFY(document.placementSpatialIndex);
    QMatrix4x4 identity;
    std::vector<quint32> visible;
    document.placementSpatialIndex->queryVisible(identity,{},&visible);
    QCOMPARE(visible,std::vector<quint32>({0}));

    document.points[0].position.x=-4.0;
    document.placementSpatialIndex=JobPlacementSpatialIndex::build(document,token);
    document.placementSpatialIndex->queryVisible(identity,{},&visible);
    QVERIFY(visible.empty());
}

void TestJob::largePlacementVisibilityIsNotSpatiallyTruncated() {
    constexpr int placementCount=200'257;
    auto stack=std::make_shared<JobModelStack>();
    stack->physicalSize={0.001,0.001};stack->layerCount=1;stack->layerHeight=0.001;
    JobDocument document;
    document.models.push_back({0,QStringLiteral("0"),QStringLiteral("0"),
                               JobModelStackState::Loaded,stack,{}});
    document.points.reserve(placementCount);
    for(int index=0;index<placementCount;++index) {
        const double x=-0.8+1.6*index/(placementCount-1.0);
        document.points.push_back({0,{x,0.0,0.5},index,index,{}});
    }
    const auto token=std::make_shared<CancellationToken>();
    const auto spatial=JobPlacementSpatialIndex::build(document,token);
    QVERIFY(spatial);
    QMatrix4x4 identity;
    std::vector<quint32> visible;
    spatial->queryVisible(identity,{},&visible,std::numeric_limits<std::size_t>::max());
    QCOMPARE(visible.size(),static_cast<std::size_t>(placementCount));
}

void TestJob::layeredPackageKeepsSharedLodDetail() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QByteArray commands;
    for (int index = 0; index < 100; ++index)
        commands += QStringLiteral(R"({"command":"relmove","by":[%1,0,0]},
                                      {"command":"place","id":0}%2)" )
                        .arg(index).arg(index == 99 ? QString{} : QStringLiteral(","))
                        .toUtf8();
    const QByteArray job = QByteArrayLiteral(R"({"models":[{"label":"0","path":"0"}],"commands":[)") +
                           commands + QByteArrayLiteral("]}");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    const QByteArray report = QByteArrayLiteral(R"({
      "schemaVersion":3,"mode":"layered_package",
      "mapping":{"pixel2phy":0.5},
      "layers":{"layerHeight":0.25,"count":1,"invertMask":false},
      "output":{"attachToCanvas":false,"standardWidth":128,"standardHeight":128,
                "canvasWidth":128,"canvasHeight":128,"bitsPerPixel":1}})");
    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"), report).isEmpty());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    QImage mask(128,128,QImage::Format_Mono);
    mask.setColorTable({qRgb(0,0,0),qRgb(255,255,255)});mask.fill(1);
    QVERIFY(mask.save(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                          QStringLiteral("1.bmp")),"BMP"));
    QString error;
    const auto document=JobPackageLoader::load(
        jobPath,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(document,qPrintable(error));QCOMPARE(document->points.size(),size_t(100));
    QVERIFY(!document->models[0].stack);
    const auto stack=JobPackageLoader::loadStack(
        document,57,21,std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(stack.preview,qPrintable(error));QVERIFY(!stack.preview->downsampled);
    QCOMPARE(stack.preview->gridSize,QSize(128,128));
    QCOMPARE(stack.preview->solidGeometry->indices.size(),size_t(30));
}

void TestJob::processingPackageLoadsFirstLayers() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray job = QByteArrayLiteral(R"({
      "models":[{"label":"a","path":"0"},{"label":"b","path":"0"}],
      "commands":[
        {"command":"relmove","by":[10,20,0]},{"command":"place","id":1},
        {"command":"relmove","by":[20,0,0]},{"command":"place","id":0},
        {"command":"relmove","by":[7,8,0]},{"command":"place","id":1}
      ]
    })");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"),
                      packageReport()).isEmpty());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("1.bmp")), {{0,0},{1,1}}));
    QString error;
    const auto loaded = JobPackageLoader::loadProcessingPackage(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(loaded.document, qPrintable(error));
    QVERIFY(!loaded.inferredGeometry);
    QCOMPARE(loaded.document->points.size(), size_t(3));
    QCOMPARE(loaded.document->points[0].position, (JobCoordinate{10,20,0}));
    QCOMPARE(loaded.document->points[0].placeId, qint64(1));
    QCOMPARE(loaded.document->points[1].position, (JobCoordinate{20,0,0}));
    QVERIFY(loaded.document->points[1].position.x >
            loaded.document->points[0].position.x);
    QCOMPARE(loaded.document->idPrefixSums.back(), (JobCoordinate{37,28,0}));
    QCOMPARE(loaded.document->loadedStackCount(), 2);
    QCOMPARE(loaded.document->models[0].stack.get(), loaded.document->models[1].stack.get());
    QCOMPARE(loaded.document->models[0].stack->physicalSize, QSizeF(1,1));
    QCOMPARE(loaded.document->models[0].stack->layerCount, 1);
    QCOMPARE(loaded.document->models[0].stack->layerHeight, 0.25);
    QCOMPARE(loaded.document->models[0].stack->lodLevels.size(), size_t(1));
    QVERIFY(std::max(loaded.document->models[0].stack->lodLevels[0].gridSize.width(),
                     loaded.document->models[0].stack->lodLevels[0].gridSize.height()) <= 64);
    QVERIFY(loaded.document->sceneBounds.valid);
    QCOMPARE(loaded.document->sceneBounds.minimum, (JobCoordinate{6.5,-0.5,0}));
    QCOMPARE(loaded.document->sceneBounds.maximum, (JobCoordinate{20.5,20.5,0.25}));
}

void TestJob::processingMaskKeepsRasterYOrder() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray job = QByteArrayLiteral(R"({
      "models":[{"label":"0","path":"0"}],
      "commands":[
        {"command":"relmove","by":[0,0,0]},{"command":"place","id":0}
      ]
    })");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"),
                      packageReport(1)).isEmpty());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("1.bmp")), {{0,0}}));

    QString error;
    const auto loaded = JobPackageLoader::loadProcessingPackage(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(loaded.document, qPrintable(error));
    const auto& stack = loaded.document->models[0].stack;
    QVERIFY(stack);
    QVERIFY(!stack->mapRasterYToPositiveY);
    QVERIFY(stack->solidGeometry);

    float minimumY = std::numeric_limits<float>::max();
    float maximumY = std::numeric_limits<float>::lowest();
    for (const MaskStackVertex& vertex : stack->solidGeometry->vertices) {
        minimumY = std::min(minimumY, vertex.y);
        maximumY = std::max(maximumY, vertex.y);
    }
    QCOMPARE(minimumY, 0.0F);
    QCOMPARE(maximumY, 0.5F);
}

void TestJob::processingPackageCentersLegacyExportedMasks() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray job = QByteArrayLiteral(R"({
      "models":[{"label":"0","path":"0"}],
      "commands":[
        {"command":"relmove","by":[-45,-45,0]},{"command":"place","id":0},
        {"command":"relmove","by":[0,-45,0]},{"command":"place","id":0},
        {"command":"relmove","by":[0,0,0]},{"command":"place","id":0},
        {"command":"relmove","by":[-45,0,0]},{"command":"place","id":0}
      ]
    })");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    const QByteArray legacyReport = QByteArrayLiteral(R"({
      "schemaVersion":3,"mode":"layered_package",
      "mapping":{"pixel2phy":0.25},
      "layers":{"layerHeight":1,"count":1,"invertMask":false},
      "output":{"attachToCanvas":false,"standardWidth":610,
                "standardHeight":610,"canvasWidth":1920,"canvasHeight":1080,
                "exportMultiplier":13.55,"bitsPerPixel":1}
    })");
    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"), legacyReport).isEmpty());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    QImage mask(610, 610, QImage::Format_Mono);
    mask.setColorTable({qRgb(0,0,0), qRgb(255,255,255)});
    mask.fill(1);
    QVERIFY(mask.save(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                          QStringLiteral("1.bmp")), "BMP"));

    QString error;
    const auto loaded = JobPackageLoader::loadProcessingPackage(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(loaded.document, qPrintable(error));
    QCOMPARE(loaded.document->models[0].stack->physicalSize, QSizeF(45,45));
    QCOMPARE(loaded.document->points[0].position, (JobCoordinate{-45,-45,0}));
    QCOMPARE(loaded.document->sceneBounds.minimum, (JobCoordinate{-67.5,-67.5,0}));
    QCOMPARE(loaded.document->sceneBounds.maximum, (JobCoordinate{22.5,22.5,1}));
}

void TestJob::processingPackageInfersGeometryWithoutReport() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString jobPath = writeJob(directory, QStringLiteral("job.aldl"), validJob());
    for (const QString& name : {QStringLiteral("0"), QStringLiteral("1")}) {
        QVERIFY(QDir(directory.path()).mkdir(name));
        QVERIFY(writeBinaryMask(QDir(directory.filePath(name)).filePath(
                                    QStringLiteral("1.bmp")), {{0,0}}));
    }
    QString error;
    const auto loaded = JobPackageLoader::loadProcessingPackage(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(loaded.document, qPrintable(error));
    QVERIFY(loaded.inferredGeometry);
    QVERIFY(loaded.warning.contains(QStringLiteral("推断")));
    QCOMPARE(loaded.document->loadedStackCount(), 2);
    QCOMPARE(loaded.document->models[0].stack->physicalSize, QSizeF(2.5,2.5));
    QVERIFY(loaded.document->models[0].stack->layerHeight > 0.0);
}

void TestJob::processingPackageRejectsIncompleteInputs() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), validJob());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("1.bmp")), {{0,0}}));
    QString error;
    auto loaded = JobPackageLoader::loadProcessingPackage(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY(!loaded.document);
    QVERIFY(error.contains(QStringLiteral("目录")) || error.contains(QStringLiteral("1.bmp")));

    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("1")));
    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("1"))).filePath(
                                QStringLiteral("1.bmp")), {{0,0}}));
    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"),
                      QByteArrayLiteral("not-json")).isEmpty());
    error.clear();
    loaded = JobPackageLoader::loadProcessingPackage(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY(!loaded.document);
    QVERIFY(error.contains(QStringLiteral("export-report.json")));
}

void TestJob::incompleteLayeredPackageFallsBackToPoints() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QByteArray job = QByteArrayLiteral(R"({
      "models":[{"label":"0","path":"0"},{"label":"unsafe","path":"../outside"}],
      "commands":[
        {"command":"relmove","by":[0,0,0]},{"command":"place","id":0},
        {"command":"relmove","by":[1,0,0]},{"command":"place","id":1}
      ]
    })");
    const QString jobPath = writeJob(directory, QStringLiteral("job.json"), job);
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("0")));
    QString error;
    auto document = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QVERIFY(!document->layeredPackageDetected);
    QCOMPARE(document->loadedStackCount(), 0);
    QVERIFY(document->packageWarnings.isEmpty());
    auto loaded = JobPackageLoader::loadStack(
        document, 0, 1, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY(!loaded.preview); QVERIFY(error.contains(QStringLiteral("export-report.json")));

    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"), packageReport()).isEmpty());
    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("1.bmp")), {{0,0}}));
    document = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->points.size(), size_t(2));
    QCOMPARE(document->loadedStackCount(), 0);
    loaded = JobPackageLoader::loadStack(
        document, 0, 2, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY(!loaded.preview); QVERIFY(error.contains(QStringLiteral("切片")));

    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("2.bmp")), {{0,0}}));
    document = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    loaded = JobPackageLoader::loadStack(
        document, 0, 3, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(loaded.preview, qPrintable(error));
    const auto unsafe = JobPackageLoader::loadStack(
        document, 1, 4, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY(!unsafe.preview); QVERIFY(error.contains(QStringLiteral("安全相对路径")));
}

void TestJob::previewMatchesExporterCoordinates() {
    ImageMetadata metadata; metadata.width = 1000; metadata.height = 500;
    SegmentationResult segmentation;
    segmentation.settings = {500, 250, 128, ForegroundPolarity::Dark};
    segmentation.rows = 2; segmentation.columns = 2;
    PrintExportOptions options{20, 10, 1, true};
    QString error;
    const auto document = JobPreviewBuilder::build(metadata, segmentation, TraversalMode::SnakeZ,
                                                    options, std::make_shared<CancellationToken>(),
                                                    {}, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->points.size(), size_t(4));
    QCOMPARE(document->linkedCells[2], QPoint(1, 1));
    QCOMPARE(document->points[2].position, (JobCoordinate{10.0, -5.0, 0.0}));
    QVERIFY(document->hasImageCenter);
    QCOMPARE(document->imageCenter, (JobCoordinate{5.0, -2.5, 0.0}));
    QVERIFY(document->isLinked());
}

void TestJob::labelLodThresholds() {
    QCOMPARE(JobLabelLod::decide(3.99,1000000).level,JobLabelLodLevel::Hidden);
    const auto range=JobLabelLod::decide(8.0,1000000);
    QCOMPARE(range.level,JobLabelLodLevel::Range);QCOMPARE(range.rangeSize,qint64(100));
    const auto individual=JobLabelLod::decide(32.0,1000000);
    QCOMPARE(individual.level,JobLabelLodLevel::Individual);QCOMPARE(individual.rangeSize,qint64(1));
    QCOMPARE(individual.maximumOrdinaryLabels,4096);
}

void TestJob::projectedSpacingEnablesRangeLabels() {
    const std::array<QPointF, 8> corners = {
        QPointF(20.0, 20.0), QPointF(900.0, 20.0),
        QPointF(20.0, 740.0), QPointF(900.0, 740.0),
        QPointF(20.0, 20.0), QPointF(900.0, 20.0),
        QPointF(20.0, 740.0), QPointF(900.0, 740.0)
    };
    const double spacing = JobPreviewMath::projectedPointSpacing(corners, 6400);
    QVERIFY(spacing > 4.0);
    QCOMPARE(JobLabelLod::decide(spacing, 6400).level, JobLabelLodLevel::Range);

    const std::array<QPointF, 2> nullExtent = {QPointF(10.0, 10.0), QPointF(10.0, 10.0)};
    QCOMPARE(JobPreviewMath::projectedPointSpacing(nullExtent, 6400), 0.0);

    const std::array<QPointF, 5> closeView = {
        QPointF(30.0, 40.0), QPointF(330.0, 80.0), QPointF(80.0, 190.0),
        QPointF(230.0, 290.0), QPointF(300.0, 470.0)
    };
    const double closeSpacing=JobPreviewMath::projectedPointSpacing(closeView,closeView.size());
    QCOMPARE(JobLabelLod::decide(closeSpacing,6400).level,JobLabelLodLevel::Individual);
}

void TestJob::rayQueryPrioritizesNearestPoints() {
    std::vector<JobPoint> points;
    points.reserve(10000);
    for(qint64 id=0;id<10000;++id)points.push_back({id,{0.0,0.0,double(id+1)},id*2});
    const auto index=JobSpatialIndex::build(points,std::make_shared<CancellationToken>());
    QVERIFY(index);
    std::vector<quint32> candidates;
    index->queryRay({0.0F,0.0F,0.0F},{0.0F,0.0F,1.0F},0.0F,{},&candidates,32);
    QVERIFY(!candidates.empty());
    QVERIFY(std::find(candidates.begin(),candidates.end(),0U)!=candidates.end());
}

void TestJob::vulkanScreenCoordinatesKeepYDirection() {
    const QSizeF viewport(800.0, 600.0);
    QCOMPARE(JobPreviewMath::vulkanNdcToScreen({-1.0F, -1.0F}, viewport), QPointF(0.0, 0.0));
    QCOMPARE(JobPreviewMath::vulkanNdcToScreen({1.0F, 1.0F}, viewport), QPointF(800.0, 600.0));
    QCOMPARE(JobPreviewMath::screenToVulkanNdc({200.0, 150.0}, viewport),
             QVector2D(-0.5F, -0.5F));

    QMatrix4x4 identity;
    QPointF screen;
    QVERIFY(JobPreviewMath::project({-0.5F, -0.5F, 0.5F}, identity, viewport, &screen));
    QCOMPARE(screen, QPointF(200.0, 150.0));
    QVector3D rayOrigin, rayDirection, intersection;
    QVERIFY(JobPreviewMath::screenRay(screen, viewport, identity, &rayOrigin, &rayDirection));
    QVERIFY(JobPreviewMath::intersectHorizontalPlane(rayOrigin, rayDirection, 0.5F, &intersection));
    QVERIFY((intersection - QVector3D(-0.5F, -0.5F, 0.5F)).length() < 0.0001F);
}

void TestJob::guidesUseCoordinateOriginAndFullBounds() {
    JobDocument document;
    document.points = {
        {2, {30.0, -40.0, 4.0}, 5},
        {0, {10.0, -10.0, 2.0}, 1},
        {1, {20.0, -25.0, 3.0}, 3}
    };
    for (const JobPoint& point : document.points) document.bounds.include(point.position);
    const JobCoordinate floatingOrigin = document.bounds.center();
    const JobGuideGeometry guides = JobPreviewMath::guideGeometry(document, floatingOrigin);
    QVERIFY(guides.valid);
    QCOMPARE(guides.coordinateOrigin, JobCoordinate{}.relativeTo(floatingOrigin));
    QCOMPARE(guides.xStart.x(), JobCoordinate{}.relativeTo(floatingOrigin).x());
    QCOMPARE(guides.xEnd.x(), document.bounds.maximum.relativeTo(floatingOrigin).x());
    QCOMPARE(guides.xStart.y(), guides.coordinateOrigin.y());
    QCOMPARE(guides.yStart.y(), document.bounds.minimum.relativeTo(floatingOrigin).y());
    QCOMPARE(guides.yEnd.y(), JobCoordinate{}.relativeTo(floatingOrigin).y());
    QCOMPARE(guides.yStart.x(), guides.coordinateOrigin.x());
    QCOMPARE(guides.xDimension, 20.0);
    QCOMPARE(guides.yDimension, 30.0);

    const JobCoordinate centeredOrigin{20.0, -25.0, 3.0};
    const JobGuideGeometry centered = JobPreviewMath::guideGeometry(
        document, floatingOrigin, centeredOrigin);
    QCOMPARE(centered.coordinateOrigin, centeredOrigin.relativeTo(floatingOrigin));
    QCOMPARE(centered.xStart.y(), centered.coordinateOrigin.y());
    QCOMPARE(centered.yStart.x(), centered.coordinateOrigin.x());
}

void TestJob::coordinateDisplayTransformMovesScene() {
    const JobCoordinate floatingOrigin{100.0, 200.0, 5.0};
    const JobCoordinate imageCenter{110.0, 190.0, 0.0};
    const JobCoordinate firstPoint{90.0, 205.0, 0.0};
    const QVector3D point(30.0F, -5.0F, 2.0F);
    const QVector3D center = imageCenter.relativeTo(floatingOrigin);
    const QVector3D firstLocal = firstPoint.relativeTo(floatingOrigin);

    const QMatrix4x4 fromFirstPoint = JobPreviewMath::coordinateDisplayTransform(
        floatingOrigin, firstPoint, false);
    QCOMPARE(fromFirstPoint.map(point), QVector3D(40.0F, -10.0F, 2.0F));
    QCOMPARE(fromFirstPoint.map(firstLocal), QVector3D(0.0F, 0.0F, firstLocal.z()));

    const QMatrix4x4 centered = JobPreviewMath::coordinateDisplayTransform(
        floatingOrigin, imageCenter, false);
    QCOMPARE(centered.map(point), QVector3D(20.0F, 5.0F, 2.0F));
    QCOMPARE(centered.map(center), QVector3D(0.0F, 0.0F, center.z()));

    const QMatrix4x4 reversed = JobPreviewMath::coordinateDisplayTransform(
        floatingOrigin, firstPoint, true);
    QCOMPARE(reversed.map(point), QVector3D(-40.0F, 10.0F, 2.0F));

    const QMatrix4x4 centeredAndReversed = JobPreviewMath::coordinateDisplayTransform(
        floatingOrigin, imageCenter, true);
    QCOMPARE(centeredAndReversed.map(point), QVector3D(-20.0F, -5.0F, 2.0F));
    QCOMPARE(centeredAndReversed.map(center), QVector3D(0.0F, 0.0F, center.z()));
}

void TestJob::coordinateOriginRemapsAcrossExportFrames() {
    const JobCoordinate previewCenter{-6120.0, -5805.0, 0.0};
    const JobCoordinate previewFirst{0.0, 0.0, 0.0};
    const JobCoordinate exportedFirst{6120.0, 5805.0, 0.0};

    QCOMPARE(JobPreviewMath::remapCoordinateOrigin(
                 previewCenter, previewFirst, exportedFirst),
             (JobCoordinate{0.0, 0.0, 0.0}));
}

void TestJob::terrainCameraIsGroundOrientedAndClamped() {
    JobTerrainCamera camera;
    QCOMPARE(camera.minimumElevation(),5.0F);QCOMPARE(camera.maximumElevation(),89.0F);
    camera.setSceneBounds({-50.0F, -25.0F, 0.0F}, {50.0F, 25.0F, 10.0F});
    QCOMPARE(camera.pose().pivot, QVector3D(0.0F, 0.0F, 5.0F));
    QVERIFY(camera.pose().distance > camera.sceneRadius());
    QVERIFY(std::abs(QVector3D::dotProduct(camera.groundForward(), camera.right())) < 0.0001F);
    QVERIFY(std::abs(camera.groundForward().z()) < 0.0001F);
    QVERIFY(std::abs(camera.right().z()) < 0.0001F);

    camera.orbit({0.0, 10000.0});
    QCOMPARE(camera.pose().elevation, 89.0F);
    camera.orbit({0.0, -20000.0});
    QCOMPARE(camera.pose().elevation, 5.0F);
    camera.setElevationLimits(-89.0F,89.0F);
    QCOMPARE(camera.minimumElevation(),-89.0F);QCOMPARE(camera.maximumElevation(),89.0F);
    camera.orbit({0.0,-10000.0});QCOMPARE(camera.pose().elevation,-89.0F);
    camera.orbit({0.0,200.0});QCOMPARE(camera.pose().elevation,-39.0F);
    const QMatrix4x4 belowSurfaceView=camera.viewMatrix();
    for(int i=0;i<16;++i)QVERIFY(std::isfinite(belowSurfaceView.constData()[i]));
    camera.zoomBy(0.0F);
    QVERIFY(camera.pose().distance >= camera.minimumDistance());
    camera.zoomBy(1.0e-12F);
    QCOMPARE(camera.pose().distance, camera.minimumDistance());
    camera.zoomBy(1.0e20F);
    QCOMPARE(camera.pose().distance, camera.maximumDistance());

    const JobCameraPose focused = camera.focusPose({12.0F, 8.0F, 3.0F});
    QCOMPARE(focused.pivot, QVector3D(12.0F, 8.0F, 3.0F));
    QVERIFY(focused.distance >= camera.sceneRadius() * 0.03F);
    const float currentDistance=camera.pose().distance;
    const JobCameraPose linkedFocus=camera.focusPose({-7.0F,4.0F,1.0F},true);
    QCOMPARE(linkedFocus.pivot,QVector3D(-7.0F,4.0F,1.0F));
    QCOMPARE(linkedFocus.distance,currentDistance);
    QCOMPARE(linkedFocus.azimuth,camera.pose().azimuth);
    QCOMPARE(linkedFocus.elevation,camera.pose().elevation);
}

void TestJob::terrainCameraContinuousOrbitCrossesPoles() {
    JobTerrainCamera camera;
    camera.setContinuousOrbitEnabled(true);
    QVERIFY(camera.continuousOrbitEnabled());
    camera.setSceneBounds({-50.0F,-25.0F,0.0F},{50.0F,25.0F,10.0F});
    JobCameraPose pose=camera.pose();pose.elevation=89.0F;camera.setPose(pose);
    camera.orbit({0.0,8.0});
    QCOMPARE(camera.pose().elevation,91.0F);
    const QMatrix4x4 abovePole=camera.viewMatrix();
    for(int i=0;i<16;++i)QVERIFY(std::isfinite(abovePole.constData()[i]));
    camera.orbit({0.0,-728.0});
    QCOMPARE(camera.pose().elevation,-91.0F);
    const QMatrix4x4 belowPole=camera.viewMatrix();
    for(int i=0;i<16;++i)QVERIFY(std::isfinite(belowPole.constData()[i]));
    camera.setContinuousOrbitEnabled(false);
    QCOMPARE(camera.pose().elevation,camera.minimumElevation());
}

void TestJob::terrainCameraPanNearHorizonStaysBounded() {
    JobTerrainCamera camera;
    camera.setSceneBounds({-50,-50,0},{50,50,0});
    camera.orbit({0,-1000}); // Reach the supported five-degree elevation.
    const QSizeF viewport(800,600);
    const auto matrix = [&] {
        QMatrix4x4 projection, correction;
        projection.perspective(45.0F, 800.0F/600.0F,
            camera.pose().distance * 0.001F,
            camera.pose().distance + camera.sceneRadius() * 8.0F);
        correction(1,1) = -1.0F;
        correction(2,2) = 0.5F;
        correction(2,3) = 0.5F;
        return correction * projection * camera.viewMatrix();
    };
    // The horizon is inside the viewport even though elevation is clamped.
    const double horizon = 300.0 - std::tan(5.0 * 3.141592653589793 / 180.0) *
                                        300.0 / std::tan(22.5 * 3.141592653589793 / 180.0);
    const QPointF previous(400,horizon+0.1), current(400,horizon+1.1);
    QVector3D rayOrigin, direction, before, after;
    const auto rawIntersection = [&](const QPointF& screen, QVector3D* hit) {
        return JobPreviewMath::screenRay(screen,viewport,matrix(),&rayOrigin,&direction) &&
            JobPreviewMath::intersectHorizontalPlane(rayOrigin,direction,0,hit);
    };
    QVERIFY(rawIntersection(previous,&before));
    QVERIFY(rawIntersection(current,&after));
    QVERIFY((before-after).length() > camera.pose().distance * 100.0F);
    QVERIFY(!camera.groundIntersection(previous,viewport,matrix(),&before));
    const float distance = camera.pose().distance;
    for (int i=0; i<100; ++i) {
        const QVector3D pivot = camera.pose().pivot;
        camera.panOnGround(previous,current,viewport,matrix());
        QVERIFY((camera.pose().pivot-pivot).length() < distance * 0.02F);
        QCOMPARE(camera.pose().pivot.z(),0.0F);
        bool invertible = false;
        const QMatrix4x4 inverse = matrix().inverted(&invertible);
        Q_UNUSED(inverse);
        QVERIFY(invertible);
    }
    const QVector3D pivot = camera.pose().pivot;
    camera.translate({std::numeric_limits<float>::infinity(),0,0});
    QCOMPARE(camera.pose().pivot,pivot);
}

void TestJob::terrainCameraPanKeepsGroundAnchor() {
    JobTerrainCamera camera;
    camera.setSceneBounds({-50,-50,0},{50,50,0});
    const QSizeF viewport(800,600);
    QMatrix4x4 projection, correction;
    projection.perspective(45.0F,800.0F/600.0F,0.1F,1000.0F);
    correction(1,1)=-1; correction(2,2)=0.5F; correction(2,3)=0.5F;
    const QMatrix4x4 mvp=correction*projection*camera.viewMatrix();
    QVector3D before,after;
    QVERIFY(camera.groundIntersection({400,300},viewport,mvp,&before));
    QVERIFY(camera.groundIntersection({410,310},viewport,mvp,&after));
    const QVector3D pivot=camera.pose().pivot;
    camera.panOnGround({400,300},{410,310},viewport,mvp);
    QVERIFY((camera.pose().pivot-pivot-(before-after)).length()<0.001F);
}

void TestJob::millionPointPreviewScale() {
    ImageMetadata metadata;metadata.width=1000;metadata.height=1000;
    SegmentationResult segmentation;segmentation.settings={1,1,128,ForegroundPolarity::Dark};segmentation.rows=1000;segmentation.columns=1000;
    QString error;QElapsedTimer elapsed;elapsed.start();
    const auto document=JobPreviewBuilder::build(metadata,segmentation,TraversalMode::SnakeZ,
                                                  PrintExportOptions{1000,1000,1,false},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(document,qPrintable(error));QCOMPARE(document->points.size(),size_t(1'000'000));
    QCOMPARE(document->linkedCells.size(),size_t(1'000'000));QCOMPARE(document->idPrefixSums.size(),size_t(1'000'001));
    QVERIFY(document->spatialIndex);QVERIFY2(elapsed.elapsed()<60000,"百万点预览构建超过 60 秒");
}

QTEST_GUILESS_MAIN(TestJob)
#include "TestJob.moc"
