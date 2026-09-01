#pragma once

#include "domain/PointCloudModels.h"

#include <QCache>
#include <QRectF>
#include <QSet>
#include <QWidget>

class QLabel;
class QCheckBox;
class QPushButton;

class PointCloudWidget final : public QWidget {
    Q_OBJECT
public:
    explicit PointCloudWidget(QWidget* parent = nullptr);
    ~PointCloudWidget() override;

    void setDocument(const PointCloudDocumentPtr& document);
    void clear();
    [[nodiscard]] PointCloudDocumentPtr document() const { return document_; }
    [[nodiscard]] bool isVulkanAvailable() const noexcept;
    void fitAll();
    [[nodiscard]] QImage grabFrame();

private:
    class Backend;
    void updateVisibleLod(float centerX, float centerY, float worldPerPixel,
                          float spanX, float spanY);
    void requestVisibleTiles();
    void rebuildVisiblePoints();
    QString tileKey(int level, int tileX, int tileY) const;

    Backend* backend_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QCheckBox* guidesCheck_ = nullptr;
    QPushButton* fitButton_ = nullptr;
    PointCloudDocumentPtr document_;
    CancellationTokenPtr requestToken_;
    QCache<QString, PointCloudTile> tileCache_{128 * 1024};
    QSet<QString> pendingTiles_;
    int visibleLevel_ = -1;
    QRect visibleTileRange_;
    QRectF visiblePixelRect_;
};
