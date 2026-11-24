#pragma once

#include "io/image_decoder.h"

#include <array>

namespace mvpview {

struct ExrHistogramChannel {
    QVector<quint64> bins;
    qint64 sampleCount = 0;
    double minimum = 0;
    double maximum = 0;
    double axisMinimum = 0;
    double axisMaximum = 1;
    double mean = 0;
    double variance = 0;
    double median = 0;
};

struct ExrHistogram {
    std::array<ExrHistogramChannel, 4> channels; // Luminance, red, green, blue.
};

class JxlExrDecoder final : public IImageDecoder {
  public:
    [[nodiscard]] static QStringList supportedSuffixes();
    [[nodiscard]] static bool supportsPath(const QString& path);
    [[nodiscard]] static ExrHistogram analyzeExr(const QString& path, const std::function<bool()>& cancelled);
    [[nodiscard]] QString cacheIdentity() const override;
    [[nodiscard]] bool canDecode(const QString& path) const override;
    [[nodiscard]] DecodeResult decode(const DecodeRequest& request) const override;
};

} // namespace mvpview
