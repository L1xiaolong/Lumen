#include "diagnostics/diagnostics.h"
#include "qml/full_screen_controller.h"

#include "core/comparison_pixel_probe.h"
#include "io/image_loader.h"
#include "io/single_file_rename.h"
#include "platform/platform_services.h"
#include "qml/qml_image_canvas.h"
#include "qml/panorama_canvas.h"
#include "browser/file_clipboard.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QLocale>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <QThreadPool>

#include <algorithm>

namespace mvpview {

FullScreenController::FullScreenController(ImageLoader* loader, QObject* parent)
    : QObject(parent), loader_(loader) {
    Q_ASSERT(loader_);
    fullLoadTimer_.setSingleShot(true);
    fullLoadTimer_.setInterval(250);
    connect(&fullLoadTimer_, &QTimer::timeout, this, [this] {
        if (!frame_ || fullRequested_ || currentPath().isEmpty()) {
            return;
        }
        if (loader_->fastScrolling() || loader_->hasInteractiveWork()) {
            fullLoadTimer_.start();
            return;
        }
        if (loader_->canAutomaticallyLoadFull({frame_})) {
            requestFullFrame(currentPath(), generation_);
        }
    });
}

QString FullScreenController::currentPath() const {
    return currentIndex_ >= 0 && currentIndex_ < paths_.size() ? paths_.at(currentIndex_)
                                                               : QString{};
}

QString FullScreenController::fileName() const { return QFileInfo(currentPath()).fileName(); }

QString FullScreenController::fileType() const {
    const QString suffix = QFileInfo(currentPath()).suffix().toUpper();
    return suffix.isEmpty() ? QStringLiteral("IMAGE") : suffix;
}

QString FullScreenController::fileSizeText() const {
    if (!frame_ && !panoramaEnabled_) return {};

    const auto bytes = frame_ ? frame_->metadata.fileSize : QFileInfo(currentPath()).size();
    QString text = QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
    // QLocale separates the number and unit with locale-dependent whitespace (often a normal,
    // non-breaking, or narrow non-breaking space). The fullscreen HUD intentionally keeps the
    // compact "100MB" form regardless of the active locale.
    for (qsizetype index = text.size(); index > 0; --index) {
        if (text.at(index - 1).isSpace())
            text.remove(index - 1, 1);
    }
    return text;
}

QSize FullScreenController::imageSize() const {
    if (panoramaEnabled_) return panoramaInfo_.size;
    return frame_ ? ComparisonPixelProbe::logicalFrameSize(*frame_) : QSize{};
}

QString FullScreenController::positionText() const {
    return currentIndex_ >= 0 ? QStringLiteral("%1 / %2").arg(currentIndex_ + 1).arg(paths_.size())
                              : QString{};
}

void FullScreenController::open(const QStringList& requestedPaths, int initialIndex) {
    paths_ = requestedPaths;
    if (!paths_.isEmpty()) {
        diagnostics::event(diagnostics::Level::Info, diagnostics::browse(),
                           QStringLiteral("fullscreen.session_open"),
                           {{QStringLiteral("slots"), static_cast<int>(paths_.size())},
                            {QStringLiteral("index"), initialIndex}},
                           true);
    }
    if (paths_.isEmpty()) {
        closeSession();
        emit closeRequested();
        return;
    }
    showIndex(std::clamp(initialIndex, 0, static_cast<int>(paths_.size()) - 1));
}

void FullScreenController::closeSession() {
    if (!paths_.isEmpty())
        diagnostics::event(diagnostics::Level::Info, diagnostics::browse(),
                           QStringLiteral("fullscreen.session_close"),
                           {{QStringLiteral("slots"), static_cast<int>(paths_.size())}}, true);
    cancelImageRequests();
    ++generation_;
    panoramaEnabled_ = false;
    panoramaInfo_ = {};
    panoramaKey_.clear();
    if (panorama_) panorama_->open({}, {});
    paths_.clear();
    currentIndex_ = -1;
    loading_ = false;
    fullRequested_ = false;
    exactResolutionRequested_ = false;
    errorText_.clear();
    frame_.reset();
    detailFrame_.reset();
    requestedDetailRegion_ = {};
    refreshCanvas(true);
    // Shared caches survive session transitions; the loader enforces their budget.
    emit stateChanged();
}

void FullScreenController::attachCanvas(QObject* object) {
    auto* canvas = qobject_cast<QmlImageCanvas*>(object);
    if (!canvas || canvas == canvas_) return;
    if (canvas_) disconnect(canvas_, nullptr, this, nullptr);
    canvas_ = canvas;
    canvas_->setPresentationMode(0);
    canvas_->setSynchronized(false);
    canvas_->setViewportTextureEnabled(true);
    connect(canvas_, &QmlImageCanvas::viewStateChanged, this, [this](int slot, const ViewState& state) {
        if (slot == 0) updateZoomDetail(state);
    });
    const auto updateRegionCoverage = [this] {
        if (canvas_ && (detailFrame_ || !requestedDetailRegion_.isEmpty()))
            updateZoomDetail(canvas_->effectiveViewState(0));
    };
    connect(canvas_, &QQuickItem::widthChanged, this, updateRegionCoverage, Qt::QueuedConnection);
    connect(canvas_, &QQuickItem::heightChanged, this, updateRegionCoverage, Qt::QueuedConnection);
    // Hovering a RAW/YUV preview promotes it to the full decode so the readout can show exact
    // source samples, exactly like the gallery probe does.
    connect(canvas_, &QmlImageCanvas::pixelProbeFullResolutionRequested, this, [this](int) {
        if (frame_ && !fullRequested_) {
            requestFullFrame(currentPath(), generation_);
        }
    });
    refreshCanvas(true);
}

void FullScreenController::attachPanorama(QObject* object) {
    auto* panorama = qobject_cast<PanoramaCanvas*>(object);
    if (!panorama || panorama == panorama_) return;
    if (panorama_) disconnect(panorama_, nullptr, this, nullptr);
    panorama_ = panorama;
    panorama_->setPixelLedger(loader_->residentPixelLedger());
    connect(panorama_, &PanoramaCanvas::statusChanged, this, [this] {
        if (!panoramaEnabled_ || !panorama_ || !panoramaInfo_.canPreview) return;
        loading_ = panorama_->loading();
        errorText_ = panorama_->errorText();
        emit stateChanged();
    });
    if (panoramaEnabled_ && panoramaInfo_.canPreview) panorama_->open(currentPath(), panoramaInfo_);
}

void FullScreenController::cancelImageRequests() {
    fullLoadTimer_.stop();
    previewHandle_.cancel();
    detailHandle_.cancel();
    fullHandle_.cancel();
}

void FullScreenController::setPanoramaEnabled(bool enabled) {
    if (!panoramaInfo_.canPreview || panoramaEnabled_ == enabled) return;
    cancelImageRequests();
    ++generation_;
    if (!panoramaKey_.isEmpty()) panoramaOverrides_.insert(panoramaKey_, new bool(enabled));
    applyPanoramaProbe(currentPath(), panoramaInfo_);
}

void FullScreenController::applyPanoramaProbe(const QString& path, const PanoramaInfo& info) {
    panoramaInfo_ = info;
    const auto* preference = panoramaOverrides_.object(panoramaKey_);
    panoramaEnabled_ = info.canPreview && (preference ? *preference : info.autoPreview);
    loading_ = true;
    errorText_.clear();
    fullRequested_ = false;
    exactResolutionRequested_ = false;
    frame_.reset();
    detailFrame_.reset();
    requestedDetailRegion_ = {};
    refreshCanvas(true);
    if (panorama_) panorama_->open(panoramaEnabled_ ? path : QString{}, panoramaInfo_);
    emit stateChanged();
    if (!panoramaEnabled_) startFlatPreview(path, generation_);
}

void FullScreenController::showPrevious() {
    if (canGoPrevious()) showIndex(currentIndex_ - 1);
}

void FullScreenController::showNext() {
    if (canGoNext()) showIndex(currentIndex_ + 1);
}

void FullScreenController::fitImage() {
    if (panoramaEnabled_) {
        if (panorama_) panorama_->resetView();
        return;
    }
    exactResolutionRequested_ = false;
    if (canvas_) canvas_->fitAll();
}

void FullScreenController::actualPixels() {
    if (panoramaEnabled_) return;
    exactResolutionRequested_ = true;
    fullLoadTimer_.stop();
    if (canvas_) {
        canvas_->actualPixelsAll();
        updateZoomDetail(canvas_->effectiveViewState(0), true);
    } else if (frame_ && !fullRequested_) {
        requestFullFrame(currentPath(), generation_);
    }
}

void FullScreenController::reload() {
    if (currentIndex_ >= 0)
        showIndex(currentIndex_);
}

void FullScreenController::copyCurrent(bool cut) {
    if (!currentPath().isEmpty()) FileClipboard::setPaths({currentPath()}, cut);
}

QString FullScreenController::renameCurrentTo(const QString& requestedName) {
    const QFileInfo source(currentPath());
    if (!source.exists()) return QCoreApplication::translate("FileOperations", "The current file no longer exists.");
    QString error;
    const QString newName = SingleFileRename::resolveName(source, requestedName, error);
    if (newName.isEmpty()) return error;
    if (newName == source.fileName()) return {};
    const QString destination = source.dir().filePath(newName);
    if (!SingleFileRename::execute(source.absoluteFilePath(), destination, &error)) return error;
    paths_[currentIndex_] = destination;
    emit filesystemChanged();
    showIndex(currentIndex_);
    return {};
}

QString FullScreenController::moveCurrentToTrash() {
    const QString path = currentPath();
    if (path.isEmpty()) return QCoreApplication::translate("FileOperations", "No image is open.");
    if (!QFile::moveToTrash(path))
        return QCoreApplication::translate("FileOperations", "Could not move the file to the Trash. Try again or use the system file manager.");
    paths_.removeAt(currentIndex_);
    emit filesystemChanged();
    if (paths_.isEmpty()) {
        closeSession();
        emit closeRequested();
        return {};
    }
    showIndex(std::min(currentIndex_, static_cast<int>(paths_.size()) - 1));
    return {};
}

QString FullScreenController::revealCurrent() {
    if (currentPath().isEmpty() || PlatformServices::revealInFileManager(currentPath())) return {};
    return QCoreApplication::translate("FileOperations", "Could not open the system file manager. Open it manually and locate the file.");
}

void FullScreenController::showIndex(int index) {
    if (index < 0 || index >= paths_.size()) return;
    // Navigation stays out of the crash breadcrumbs: paging through a large folder would flush
    // every other operator out of the fixed-size ring.
    diagnostics::event(diagnostics::Level::Debug, diagnostics::browse(),
                       QStringLiteral("fullscreen.index_change"),
                       {{QStringLiteral("index"), index}}, false);
    cancelImageRequests();
    panoramaInfo_ = {};
    panoramaKey_.clear();
    currentIndex_ = index;
    frame_.reset();
    detailFrame_.reset();
    requestedDetailRegion_ = {};
    loading_ = true;
    fullRequested_ = false;
    exactResolutionRequested_ = false;
    errorText_.clear();
    const quint64 generation = ++generation_;
    const QString path = currentPath();
    if (!isPanoramaJpeg(path)) {
        applyPanoramaProbe(path, {});
        return;
    }

    panoramaKey_ = panoramaCacheDirectory(path);
    if (const auto* info = panoramaProbes_.object(panoramaKey_)) {
        applyPanoramaProbe(path, *info);
        return;
    }

    // Preserve the existing canvas while probing. Adjacent panoramas can then
    // reuse the atlas and shared buffers, while the new image starts blank.
    if (panorama_) panorama_->open({}, {});
    refreshCanvas(true);
    emit stateChanged();
    const QPointer<FullScreenController> self(this);
    auto* application = QCoreApplication::instance();
    QThreadPool::globalInstance()->start([self, application, path, generation] {
        const auto info = probePanorama(path);
        QMetaObject::invokeMethod(application, [self, path, generation, info] {
            if (!self || self->generation_ != generation) return;
            if (!self->panoramaKey_.isEmpty())
                self->panoramaProbes_.insert(self->panoramaKey_, new PanoramaInfo(info));
            self->applyPanoramaProbe(path, info);
        }, Qt::QueuedConnection);
    }, 25);
}

void FullScreenController::startFlatPreview(const QString& path, quint64 generation) {
    const auto rawParameters = loader_->rawParameters(path);
    const qreal dpr = canvas_ && canvas_->window() ? canvas_->window()->devicePixelRatio() : 1.0;
    QSize previewSize = canvas_ ? QSize(qRound(canvas_->width() * dpr),
                                       qRound(canvas_->height() * dpr))
                                : QSize(1920, 1200);
    previewSize = previewSize.expandedTo(QSize(960, 720)).boundedTo(QSize(2560, 1600));
    if (rawParameters && !rawParameters->isYuv()) {
        previewSize = previewSize.boundedTo(QSize(1280, 800));
    }
    const bool quick = QFileInfo(path).suffix().compare(QStringLiteral("yuv"), Qt::CaseInsensitive) == 0 &&
        !loader_->isCached({path, DecodePurpose::Preview, previewSize});
    requestPreview(path, generation, previewSize, quick);
}

void FullScreenController::requestPreview(const QString& path, quint64 generation, QSize size, bool quick) {
    const QPointer<FullScreenController> self(this);
    previewHandle_ = loader_->request(generation, {path, DecodePurpose::Preview, quick ? QSize(512, 512) : size},
                     [self, generation, path, size, quick](quint64 id, const DecodeResult& result) {
        if (!self || id != generation || self->generation_ != generation ||
            self->currentPath() != path || self->fullRequested_) return;
        if (!result.frame) {
            self->loading_ = false;
            self->errorText_ = result.error;
            emit self->stateChanged();
            return;
        }
        const bool firstFrame = !self->frame_;
        self->frame_ = result.frame;
        self->loading_ = false;
        self->refreshCanvas(firstFrame);
        emit self->stateChanged();
        if (self->exactResolutionRequested_) {
            self->actualPixels();
        } else if (quick) {
            QTimer::singleShot(0, self, [self, path, generation, size] {
                if (self && self->generation_ == generation && self->currentPath() == path && !self->fullRequested_)
                    self->requestPreview(path, generation, size, false);
            });
        } else {
            self->scheduleFullFrame(path, generation);
            self->loader_->prefetchAdjacentImages(self->paths_, self->currentIndex_,
                                                 self->frame_->descriptor.size);
        }
    }, RequestOptions{LoadCategory::Interactive, quick ? 30 : 20, QStringLiteral("fullscreen-preview")});
}

void FullScreenController::scheduleFullFrame(const QString& path, quint64 generation) {
    if (path != currentPath() || generation != generation_ || fullRequested_) {
        return;
    }
    fullLoadTimer_.start();
}

QRect FullScreenController::visibleSourceRegion(const ViewState& state) const {
    if (!canvas_) return {};
    const QSize source = imageSize();
    const QSizeF viewport(canvas_->width(), canvas_->height());
    if (source.isEmpty() || viewport.isEmpty()) return {};
    const QPointF first = ViewTransform::widgetToImage({}, viewport, source, state);
    const QPointF last = ViewTransform::widgetToImage(QPointF(viewport.width(), viewport.height()),
                                                     viewport, source, state);
    return QRectF(first, last).toAlignedRect().intersected(QRect(QPoint{}, source));
}

void FullScreenController::updateZoomDetail(const ViewState& state, bool force) {
    if (detailFrame_) refreshCanvas(false);
    if (!canvas_ || !frame_ || fullRequested_ || state.fitMode != FitMode::Manual) return;
    const QSize source = imageSize();
    const QSize preview = frame_->descriptor.size;
    if (source.isEmpty() || preview.isEmpty()) return;
    if (source == preview && !frame_->sourceSamplesPending) return;
    const qreal dpr = canvas_->window() ? canvas_->window()->effectiveDevicePixelRatio() : 1.0;
    const double previewScale = std::max(double(preview.width()) / source.width(),
                                         double(preview.height()) / source.height());
    if (!force && state.pixelsPerImagePixel * dpr <= previewScale * 1.05) return;

    const QRect visible = visibleSourceRegion(state);
    if (visible.isEmpty()) return;
    if ((detailFrame_ && detailFrame_->decodedRegion.contains(visible)) ||
        requestedDetailRegion_.contains(visible)) return;

    const QRect region = visible.adjusted(-128, -128, 128, 128).intersected(QRect(QPoint{}, source));
    const QString& format = frame_->metadata.format;
    constexpr qsizetype maximumRegionBytes = 32LL * 1024 * 1024;
    const qsizetype sourceBytes = estimatedPixelBytes(source, 4);
    if (!detailFrame_ && requestedDetailRegion_.isEmpty() &&
        (format == QStringLiteral("JPG") || format == QStringLiteral("JPEG")) &&
        sourceBytes > 2 * maximumRegionBytes &&
        estimatedPixelBytes(region.size(), 4) <= std::min(maximumRegionBytes, sourceBytes / 4)) {
        requestDetailRegion(region);
    } else {
        requestFullFrame(currentPath(), generation_);
    }
}

void FullScreenController::requestDetailRegion(const QRect& region) {
    fullLoadTimer_.stop();
    requestedDetailRegion_ = region;
    const QString path = currentPath();
    const quint64 generation = generation_;
    DecodeRequest request{path, DecodePurpose::Region};
    request.sourceRegion = region;
    const QPointer<FullScreenController> self(this);
    detailHandle_ = loader_->request(generation, std::move(request),
        [self, generation, path, region](quint64 id, const DecodeResult& result) {
            if (!self || id != generation || self->generation_ != generation ||
                self->currentPath() != path || self->requestedDetailRegion_ != region) return;
            self->requestedDetailRegion_ = {};
            if (!result.frame || result.frame->decodedRegion != region ||
                result.frame->metadata.sourceSize != self->imageSize()) {
                if (self->canvas_ && self->canvas_->effectiveViewState(0).fitMode == FitMode::Manual)
                    self->requestFullFrame(path, generation);
                return;
            }
            self->detailFrame_ = result.frame;
            self->refreshCanvas(false);
        }, RequestOptions{LoadCategory::Interactive, 10, QStringLiteral("fullscreen-detail")});
}

void FullScreenController::requestFullFrame(const QString& path, quint64 generation) {
    if (panoramaEnabled_ || fullRequested_ || path != currentPath() || generation != generation_) {
        return;
    }
    fullRequested_ = true;
    fullLoadTimer_.stop();
    previewHandle_.cancel(); // A late preview must never replace exact pixels.
    detailHandle_.cancel();
    requestedDetailRegion_ = {};
    const QPointer<FullScreenController> self(this);
    fullHandle_ = loader_->request(generation, {path, DecodePurpose::Full, {}},
                     [self, generation, path](quint64 id, const DecodeResult& result) {
        if (!self || id != generation || self->generation_ != generation ||
            self->currentPath() != path) return;
        if (!result.frame) {
            self->fullRequested_ = false;
            self->errorText_ = result.error;
            emit self->stateChanged();
            return;
        }
        self->detailFrame_.reset();
        self->frame_ = result.frame;
        self->refreshCanvas(false);
        emit self->stateChanged();
    }, RequestOptions{LoadCategory::Interactive, 0, QStringLiteral("fullscreen-full")});
}

void FullScreenController::refreshCanvas(bool resetView) {
    if (!canvas_) return;
    ImageFramePtr displayFrame = frame_;
    if (!resetView && detailFrame_) {
        const auto state = canvas_->effectiveViewState(0);
        const QRect visible = visibleSourceRegion(state);
        if (state.fitMode == FitMode::Manual && !visible.isEmpty() &&
            detailFrame_->decodedRegion.contains(visible)) {
            displayFrame = detailFrame_;
        }
    }
    if (resetView || canvas_->frameAt(0) != displayFrame)
        canvas_->setFrames({displayFrame}, 0, resetView);
    canvas_->setPresentationMode(0);
    canvas_->setSynchronized(false);
}

} // namespace mvpview
