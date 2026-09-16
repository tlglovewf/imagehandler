#pragma once

#include "domain/JobModels.h"

#include <QWidget>
#include <QImage>

class QLabel;
class QCheckBox;
class QPushButton;

class JobPreviewWidget final : public QWidget {
    Q_OBJECT
public:
    explicit JobPreviewWidget(QWidget* parent = nullptr);
    ~JobPreviewWidget() override;

    void setDocument(const JobDocumentPtr& document);
    [[nodiscard]] JobDocumentPtr document() const { return document_; }
    [[nodiscard]] bool isVulkanAvailable() const noexcept;
    [[nodiscard]] qint64 selectedPlaceId() const noexcept { return selectedPlaceId_; }
    void setSelectedPlaceId(qint64 placeId, bool focus = false, bool preserveZoom = false);
    void clearSelection();
    void fitAll();
    [[nodiscard]] QImage grabFrame();

Q_SIGNALS:
    void placeSelected(qint64 placeId);
    void placeActivated(qint64 placementId);

private:
    class Backend;
    Backend* backend_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* coordinateLabel_ = nullptr;
    QCheckBox* guidesCheck_ = nullptr;
    QPushButton* fitButton_ = nullptr;
    JobDocumentPtr document_;
    qint64 selectedPlaceId_ = -1;

    void updateCoordinateLabel();
};
