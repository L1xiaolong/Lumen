#pragma once

#include "io/image_decoder.h"

namespace mvpview {

class JxlExrDecoder final : public IImageDecoder {
  public:
    [[nodiscard]] static QStringList supportedSuffixes();
    [[nodiscard]] static bool supportsPath(const QString& path);
    [[nodiscard]] QString cacheIdentity() const override;
    [[nodiscard]] bool canDecode(const QString& path) const override;
    [[nodiscard]] DecodeResult decode(const DecodeRequest& request) const override;
};

} // namespace mvpview
