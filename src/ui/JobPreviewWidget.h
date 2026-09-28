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
    void viewTop();
    [[nodiscard]] QImage grabFrame();
    void setForceIndividualLabels(bool force);
    [[nodiscard]] bool forceIndividualLabels() const noexcept;
    // Negative renders every available stack; zero renders none; positive values
    // render the placementId prefix [0, count).
    void setVisibleStackPlacementCount(qint64 count);
    [[nodiscard]] qint64 visibleStackPlacementCount() const noexcept;
    void setReverseCoordinates(bool reverse);
    [[nodiscard]] bool reverseCoordinates() const noexcept;
    void setCenterRelativeCoordinates(bool relative);
    [[nodiscard]] bool centerRelativeCoordinates() const noexcept;
    void setCoordinateOptionsVisible(bool visible);
    void setXAxisPositiveLeft(bool positiveLeft);
    [[nodiscard]] bool xAxisPositiveLeft() const noexcept { return xAxisPositiveLeft_; }
    void setYAxisPositiveDown(bool positiveDown);
    [[nodiscard]] bool yAxisPositiveDown() const noexcept { return yAxisPositiveDown_; }
    void setPreserveCoordinateValues(bool preserve);
    [[nodiscard]] bool preserveCoordinateValues() const noexcept {
        return preserveCoordinateValues_;
    }
    [[nodiscard]] JobCoordinate displayCoordinate(const JobCoordinate& coordinate) const;

Q_SIGNALS:
    void placeSelected(qint64 placeId);
    void placeActivated(qint64 placementId);

private:
    class Backend;
    Backend* backend_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* coordinateLabel_ = nullptr;
    QCheckBox* guidesCheck_ = nullptr;
    QCheckBox* reverseCoordinatesCheck_ = nullptr;
    QCheckBox* centerRelativeCheck_ = nullptr;
    QPushButton* fitButton_ = nullptr;
    JobDocumentPtr document_;
    qint64 selectedPlaceId_ = -1;
    bool forceIndividualLabels_ = false;
    bool xAxisPositiveLeft_ = false;
    bool yAxisPositiveDown_ = false;
    bool preserveCoordinateValues_ = false;
    qint64 visibleStackPlacementCount_ = -1;

    void updateCoordinateLabel();
};
