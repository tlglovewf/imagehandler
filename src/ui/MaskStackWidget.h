#pragma once

#include "domain/MaskSliceModels.h"

#include <QColor>
#include <QWidget>

enum class MaskStackDisplayMode { Solid, Exploded };

class MaskStackWidget final : public QWidget {
    Q_OBJECT
public:
    explicit MaskStackWidget(QWidget* parent = nullptr);
    ~MaskStackWidget() override;

    void setPreview(const MaskStackPreviewDataPtr& preview);
    void setColors(const QColor& start, const QColor& end);
    void setDisplayMode(MaskStackDisplayMode mode);
    void setSelectedLayer(int layer);
    void setZScale(double scale);
    void clear();
    void fitAll();
    void viewTop();
    void viewFront();
    void viewRight();

    [[nodiscard]] bool isVulkanAvailable() const noexcept;
    [[nodiscard]] MaskStackPreviewDataPtr preview() const { return preview_; }
    [[nodiscard]] MaskStackDisplayMode displayMode() const noexcept { return mode_; }
    [[nodiscard]] int selectedLayer() const noexcept { return selectedLayer_; }
    [[nodiscard]] double zScale() const noexcept { return zScale_; }
    [[nodiscard]] QImage grabFrame();

private:
    class Backend;
    Backend* backend_ = nullptr;
    MaskStackPreviewDataPtr preview_;
    QColor startColor_{0, 0, 255};
    QColor endColor_{0, 255, 0};
    MaskStackDisplayMode mode_ = MaskStackDisplayMode::Solid;
    int selectedLayer_ = 1;
    double zScale_ = 1.0;
};
