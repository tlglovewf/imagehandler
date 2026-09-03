#include "imaging/JobPreviewBuilder.h"
#include "imaging/JobLabelLod.h"
#include "imaging/JobPreviewMath.h"
#include "imaging/JobSpatialIndex.h"
#include "io/JobFileLoader.h"

#include <QFile>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>

class TestJob final : public QObject {
    Q_OBJECT
private Q_SLOTS:
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
}

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
