#include "domain/MaskSliceModels.h"
#include "imaging/JobPreviewBuilder.h"
#include "imaging/JobLabelLod.h"
#include "imaging/JobPreviewMath.h"
#include "imaging/JobSpatialIndex.h"
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
    void validatesStructureAndIds();
    void supportsEscapesAndUnknownFields();
    void cancellationDoesNotReturnDocument();
    void previewMatchesExporterCoordinates();
    void labelLodThresholds();
    void projectedSpacingEnablesRangeLabels();
    void rayQueryPrioritizesNearestPoints();
    void vulkanScreenCoordinatesKeepYDirection();
    void guidesUsePointZeroAndFullBounds();
    void terrainCameraIsGroundOrientedAndClamped();
    void terrainCameraContinuousOrbitCrossesPoles();
    void millionPointPreviewScale();
    void layeredPackageLoadsStacksAndSceneBounds();
    void layeredPackageUsesInstanceAwareLod();
    void incompleteLayeredPackageFallsBackToPoints();
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
    QVERIFY(document->layeredPackageDetected);
    QCOMPARE(document->loadedStackCount(), 1);
    QVERIFY(document->packageWarnings.isEmpty());
    QCOMPARE(document->models[0].stackState, JobModelStackState::Loaded);
    const auto stack = document->models[0].stack;
    QVERIFY(stack); QCOMPARE(stack->sourceSize, QSize(2,2));
    QCOMPARE(stack->gridSize, QSize(2,2)); QCOMPARE(stack->physicalSize, QSizeF(1,1));
    QCOMPARE(stack->layerCount, 2); QCOMPARE(stack->layerHeight, 0.25);
    QVERIFY(stack->solidGeometry); QVERIFY(!stack->solidGeometry->indices.empty());
    QVERIFY(stack->explodedGeometry); QVERIFY(!stack->explodedGeometry->indices.empty());
    QCOMPARE(document->sceneBounds.minimum, (JobCoordinate{0,0,0}));
    QCOMPARE(document->sceneBounds.maximum, (JobCoordinate{3,4,1.5}));

    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"),
                      packageReport(2, true)).isEmpty());
    QVERIFY(writeBinaryMask(model.filePath(QStringLiteral("1.bmp")), {}));
    QVERIFY(writeBinaryMask(model.filePath(QStringLiteral("2.bmp")),
                            {{0,0},{0,1},{1,1}}));
    const auto inverted = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(inverted, qPrintable(error));
    QCOMPARE(inverted->loadedStackCount(), 1);
    QVERIFY(inverted->models[0].stack->solidGeometry);
    QVERIFY(!inverted->models[0].stack->solidGeometry->indices.empty());
}

void TestJob::layeredPackageUsesInstanceAwareLod() {
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
    const auto stack=document->models[0].stack;QVERIFY(stack);QVERIFY(stack->downsampled);
    QVERIFY(qint64(stack->gridSize.width())*stack->gridSize.height()<=10'201);
    QCOMPARE(stack->solidGeometry->indices.size(),size_t(30));
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
    QVERIFY(document->layeredPackageDetected);
    QCOMPARE(document->loadedStackCount(), 0);
    QVERIFY(!document->packageWarnings.isEmpty());
    QCOMPARE(document->models[0].stackState, JobModelStackState::Invalid);

    QVERIFY(!writeJob(directory, QStringLiteral("export-report.json"), packageReport()).isEmpty());
    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("1.bmp")), {{0,0}}));
    document = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->points.size(), size_t(2));
    QCOMPARE(document->loadedStackCount(), 0);
    QCOMPARE(document->models[0].stackState, JobModelStackState::Invalid);
    QCOMPARE(document->models[1].stackState, JobModelStackState::Invalid);
    QVERIFY(document->packageWarnings.size() >= 2);

    QVERIFY(writeBinaryMask(QDir(directory.filePath(QStringLiteral("0"))).filePath(
                                QStringLiteral("2.bmp")), {{0,0}}));
    document = JobPackageLoader::load(
        jobPath, std::make_shared<CancellationToken>(), {}, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->loadedStackCount(), 1);
    QCOMPARE(document->models[0].stackState, JobModelStackState::Loaded);
    QCOMPARE(document->models[1].stackState, JobModelStackState::Invalid);
    QVERIFY(!document->packageWarnings.isEmpty());
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

void TestJob::guidesUsePointZeroAndFullBounds() {
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
    QCOMPARE(guides.pointZero, document.points[1].position.relativeTo(floatingOrigin));
    QCOMPARE(guides.xStart.x(), document.bounds.minimum.relativeTo(floatingOrigin).x());
    QCOMPARE(guides.xEnd.x(), document.bounds.maximum.relativeTo(floatingOrigin).x());
    QCOMPARE(guides.xStart.y(), guides.pointZero.y());
    QCOMPARE(guides.yStart.y(), document.bounds.minimum.relativeTo(floatingOrigin).y());
    QCOMPARE(guides.yEnd.y(), document.bounds.maximum.relativeTo(floatingOrigin).y());
    QCOMPARE(guides.yStart.x(), guides.pointZero.x());
    QCOMPARE(guides.xDimension, 20.0);
    QCOMPARE(guides.yDimension, 30.0);
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

void TestJob::millionPointPreviewScale() {
    ImageMetadata metadata;metadata.width=1000;metadata.height=1000;
    SegmentationResult segmentation;segmentation.settings={1,1,128,ForegroundPolarity::Dark};segmentation.rows=1000;segmentation.columns=1000;
    QString error;QElapsedTimer elapsed;elapsed.start();
    const auto document=JobPreviewBuilder::build(metadata,segmentation,TraversalMode::SnakeZ,
                                                  {1000,1000,1,false},std::make_shared<CancellationToken>(),{},&error);
    QVERIFY2(document,qPrintable(error));QCOMPARE(document->points.size(),size_t(1'000'000));
    QCOMPARE(document->linkedCells.size(),size_t(1'000'000));QCOMPARE(document->idPrefixSums.size(),size_t(1'000'001));
    QVERIFY(document->spatialIndex);QVERIFY2(elapsed.elapsed()<60000,"百万点预览构建超过 60 秒");
}

QTEST_GUILESS_MAIN(TestJob)
#include "TestJob.moc"
