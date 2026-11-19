#pragma once

#include "io/image_decoder.h"

namespace mvpview::JpegStripeCache {
bool eligible(QSize source);
bool exists(const QString& path);
void build(const QString& path, QSize source);
QImage read(const DecodeRequest& request, QSize source, QSize scaledSize);
}
