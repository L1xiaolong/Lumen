#pragma once

class QFile;

namespace mvpview {

struct ImageMetadata;

void readJpegGainMapMetadata(QFile& file, ImageMetadata& metadata);

} // namespace mvpview
