#pragma once

#include "core/raw_plane_access.h"

#include <QColor>
#include <QPointF>
#include <QString>

namespace mvpview {

struct ComparisonPixelSample {
    bool valid = false;
    QPoint displayPixel;
    QPoint sourcePixel;
    QColor displayColor;
    std::optional<YuvPlaneSample> yuv;
    std::optional<BayerPlaneSample> bayer;
    // Encoded image samples at the frame's own bit depth, and the RGB companion of a source
    // plane sample (the YUV conversion or the demosaiced Bayer pipeline) expressed in the
    // source container's value range. Empty when the frame has no such value.
    QString encodedValueText;
    QString sourceRgbText;
    bool approximate = false;
    // The frame holds a bounded proxy, so exact source samples need a full decode first.
    bool sourceSamplesPending = false;

    [[nodiscard]] QString sourceValueText() const;
    [[nodiscard]] QString displayValueText() const;
};

class ComparisonPixelProbe final {
  public:
    // Zoom/pan space stays at the oriented source size even when rendering a preview.
    [[nodiscard]] static QSize logicalFrameSize(const ImageFrame& frame);
    [[nodiscard]] static QPointF normalizedPixelCenter(const QPoint& pixel, const QSize& imageSize);
    [[nodiscard]] static ComparisonPixelSample sampleAtDisplayPixel(const ImageFrame& frame,
                                                                    const QPoint& displayPixel);
    [[nodiscard]] static ComparisonPixelSample sampleAtLogicalPixel(const ImageFrame& frame,
                                                                    const QPoint& logicalPixel,
                                                                    const QSize& logicalSize);
    [[nodiscard]] static ComparisonPixelSample sample(const ImageFrame& frame,
                                                      const QPointF& normalizedPoint);
};

} // namespace mvpview
