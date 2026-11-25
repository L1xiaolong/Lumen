#pragma once

#include "io/panorama_source.h"
#include "core/pixel_memory_ledger.h"

#include <QElapsedTimer>

#include <QHash>
#include <QQuickRhiItem>
#include <QQuaternion>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>
#include <optional>

namespace mvpview {

class PanoramaRenderer;

class PanoramaCanvas : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double fieldOfView READ fieldOfView WRITE setFieldOfView NOTIFY lensChanged)
    Q_PROPERTY(double horizontalFieldOfView READ fieldOfView WRITE setFieldOfView NOTIFY lensChanged)
    Q_PROPERTY(double verticalFieldOfView READ verticalFieldOfView WRITE setVerticalFieldOfView NOTIFY lensChanged)
    Q_PROPERTY(QVariantList navigationRectangles READ navigationRectangles NOTIFY navigationChanged)
    Q_PROPERTY(QString pixelReadout READ pixelReadout NOTIFY pixelReadoutChanged)
    Q_PROPERTY(bool navigationReady READ navigationReady NOTIFY statusChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged)
    Q_PROPERTY(QString detailMessage READ detailMessage NOTIFY statusChanged)

public:
    explicit PanoramaCanvas(QQuickItem* parent = nullptr);
    ~PanoramaCanvas() override;

    void setPixelLedger(std::shared_ptr<PixelMemoryLedger> ledger);
    void open(const QString& path, const PanoramaInfo& info);
    [[nodiscard]] double fieldOfView() const;
    [[nodiscard]] double verticalFieldOfView() const;
    [[nodiscard]] QVariantList navigationRectangles() const;
    [[nodiscard]] QString pixelReadout() const { return pixelReadout_; }
    [[nodiscard]] bool navigationReady() const { return !preview_.isNull() && errorText_.isEmpty(); }
    [[nodiscard]] bool loading() const { return loading_; }
    [[nodiscard]] QString errorText() const { return errorText_; }
    [[nodiscard]] QString detailMessage() const { return detailMessage_; }
    void setFieldOfView(double degrees);
    void setVerticalFieldOfView(double degrees);

    Q_INVOKABLE void turn(int horizontal, int vertical);
    Q_INVOKABLE void drag(double x, double y);
    Q_INVOKABLE void zoom(double steps);
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void lookAtSource(double u, double v);
    Q_INVOKABLE void inspectPixel(const QPointF& position);
    Q_INVOKABLE void clearPixelInspection();

signals:
    void lensChanged();
    void navigationChanged();
    void pixelReadoutChanged();
    void statusChanged();
    void imageFrameRendered();

protected:
    QQuickRhiItemRenderer* createRenderer() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    friend class PanoramaRenderer;
    struct Session;
    struct TileBounds {
        PanoramaTile tile;
        QVector3D center;
        float radius;
    };
    std::shared_ptr<Session> session_;
    QTimer selectionTimer_;
    PanoramaInfo info_;
    QList<QSize> levels_;
    QList<QList<TileBounds>> tileBounds_;
    quint64 tileRevision_ = 0;
    QList<PanoramaTile> wanted_;
    QHash<PanoramaTile, QImage> tiles_;
    QSet<PanoramaTile> unreadable_;
    QSet<int> availableRows_;
    std::shared_ptr<PixelMemoryLedger> ledger_;
    std::shared_ptr<PixelMemoryLedger::Lease> imageLease_;
    void accountPixels();
    QElapsedTimer firstFrameTimer_;
    QElapsedTimer cameraInput_;
    QImage preview_;
    QString path_;
    QString errorText_;
    QString detailMessage_;
    quint64 generation_ = 0;
    double yaw_ = 0;
    double pitch_ = 0;
    double horizontalFov_ = 75;
    double verticalFov_ = 0;
    bool loading_ = false;
    bool pendingTileRead_ = false;
    int targetLevel_ = -1;
    QImage originalPreview_;
    QImage inspectionImage_;
    std::optional<PanoramaTile> inspectionTile_;
    std::optional<QPointF> inspectionPosition_;
    QString pixelReadout_;
    bool pendingInspection_ = false;
    bool buildingTiles_ = false;

    [[nodiscard]] QPointF lensTangents() const;
    [[nodiscard]] QQuaternion cameraRotation() const;
    void viewChanged();
    void refreshPixelInspection();
    void setPixelReadout(const QString& text);
    void changeDirection(double yaw, double pitch);
    void scheduleSelection();
    void selectTiles();
    [[nodiscard]] QList<PanoramaTile> visibleTiles(int level, const QQuaternion& camera,
                                                  double horizontal, double vertical) const;
    void buildTileBounds();
    void loadVisibleTiles();
    void graphicsFailed(quint64 generation);
    void frameReady(quint64 generation);
};

} // namespace mvpview
