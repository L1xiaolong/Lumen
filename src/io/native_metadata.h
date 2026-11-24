#pragma once

#include "core/image_types.h"

namespace mvpview {

// Reads container and codestream headers without decoding pixel data. The return value
// indicates whether the file should also be examined by the generic tag reader.
bool readNativeMetadata(const QString& path, ImageMetadata& metadata);

} // namespace mvpview
