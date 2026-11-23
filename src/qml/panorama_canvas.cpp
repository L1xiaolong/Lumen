#include "qml/panorama_canvas.h"

#include "diagnostics/diagnostics.h"
#include "io/encoded_color_management.h"

#include <QCoreApplication>
#include <QFile>
#include <QMatrix4x4>
#include <QPointer>
#include <QQuickWindow>
#include <QThreadPool>
#include <QtMath>
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>

namespace mvpview {
namespace {

constexpr double minimumFov = 15;
constexpr double maximumFov = 120;
constexpr double minimumVerticalFov = 5;
constexpr double maximumVerticalFov = 150;
constexpr double defaultFov = 75;
constexpr double pi = std::numbers::pi;
constexpr int maximumAtlasSide = 4096;
constexpr int pageSide = 72;
constexpr int cpuTileLimit = 60;
constexpr int uploadsPerFrame = 3;
constexpr std::array<float, 16> quad{
    -1.F, -1.F, 0.F, 1.F, 1.F, -1.F, 1.F, 1.F,
    -1.F, 1.F, 0.F, 0.F, 1.F, 1.F, 1.F, 0.F,
};

struct PanoramaTaskPools {
    QThreadPool imports;
    QThreadPool reads;
    std::atomic_bool stopping{false};

    PanoramaTaskPools() {
        // Keep one pyramid writer and one tile reader active. Leave the GUI
        // and the main image loader out of disk and coefficient-buffer waits.
        imports.setMaxThreadCount(1);
        reads.setMaxThreadCount(1);
        auto* application = QCoreApplication::instance();
        QObject::connect(application, &QCoreApplication::aboutToQuit, application, [this] {
            stopping = true;
            imports.clear();
            reads.clear();
            imports.waitForDone();
            reads.waitForDone();
        });
    }
};

PanoramaTaskPools& panoramaTasks() {
    static PanoramaTaskPools pools;
    return pools;
}

int tileCount(int pixels) { return (pixels + panoramaTileSize - 1) / panoramaTileSize; }

QQuaternion panoramaRotation(double yaw, double pitch, double roll) {
    return QQuaternion::fromAxisAndAngle(0, 1, 0, float(std::remainder(yaw, 360.0))) *
           QQuaternion::fromAxisAndAngle(1, 0, 0, float(-pitch)) *
           QQuaternion::fromAxisAndAngle(0, 0, 1, float(std::remainder(roll, 360.0)));
}

QVector3D direction(double longitude, double latitude) {
    return {float(std::sin(longitude) * std::cos(latitude)), float(std::sin(latitude)),
            float(std::cos(longitude) * std::cos(latitude))};
}

double halfFovTangent(double degrees) { return std::tan(qDegreesToRadians(degrees * 0.5)); }
double fovFromTangent(double tangent) { return qRadiansToDegrees(2 * std::atan(tangent)); }

QPointF sourceLocation(const QVector3D& ray) {
    const double length = std::hypot(double(ray.x()), double(ray.z()));
    const double longitude = length > 1e-6 ? std::atan2(ray.x(), ray.z()) : 0.0;
    const double latitude = std::atan2(ray.y(), std::max(length, 1e-6));
    const double u = 0.5 + longitude / (2 * pi);
    return {u - std::floor(u), std::clamp(0.5 - latitude / pi, 0.0, 1.0)};
}

QShader shader(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}

struct alignas(16) PanoramaUniforms {
    std::array<float, 16> clip{};
    std::array<float, 16> camera{};
    std::array<float, 4> lens{};
    std::array<float, 4> atlas{};
    std::array<std::array<float, 4>, 8> levels{};
};

void normalize(QImage& image, DisplayColorSpace target) {
    if (image.isNull()) return;
    ImageMetadata metadata;
    EncodedColorManagement::normalizeToDisplay(image, metadata, target);
    image = image.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace

struct PanoramaCanvas::Session {
    QString directory;
    DisplayColorSpace displaySpace = DisplayColorSpace::Srgb;
    std::atomic_bool cancelled{false};
};

class PanoramaRenderer final : public QQuickRhiItemRenderer {
    struct Slot {
        PanoramaTile tile;
        quint64 stamp = 0;
        bool valid = false;
    };

    QPointer<PanoramaCanvas> item_;
    QRhi* backend_ = nullptr;
    std::unique_ptr<QRhiBuffer> vertices_;
    std::unique_ptr<QRhiBuffer> uniform_;
    std::unique_ptr<QRhiTexture> previewTexture_;
    std::unique_ptr<QRhiTexture> atlas_;
    std::unique_ptr<QRhiTexture> pages_;
    std::unique_ptr<QRhiSampler> previewSampler_;
    std::unique_ptr<QRhiSampler> tileSampler_;
    std::unique_ptr<QRhiSampler> lookupSampler_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    QRhiRenderPassDescriptor* renderPass_ = nullptr;
    std::shared_ptr<PixelMemoryLedger> gpuLedger_;
    std::shared_ptr<PixelMemoryLedger::Lease> gpuLease_;
    std::shared_ptr<PixelMemoryLedger::Lease> imageLease_;
    QRhiTexture* accountedColor_ = nullptr;
    QRhiTexture* accountedPreview_ = nullptr;
    QRhiTexture* accountedAtlas_ = nullptr;
    std::vector<Slot> slots_;
    QImage preview_;
    QHash<PanoramaTile, QImage> tiles_;
    QList<PanoramaTile> wanted_;
    QList<QSize> levels_;
    QQuaternion camera_;
    QPointF lens_;
    quint64 generation_ = 0;
    quint64 stamp_ = 0;
    quint64 tileRevision_ = 0;
    bool atlasDirty_ = true;
    qint64 previewKey_ = 0;
    int atlasColumns_ = 0;
    int targetLevel_ = -1;
    bool cameraMoving_ = false;
    bool atlasAttempted_ = false;
    bool needsClear_ = true;
    bool submitted_ = false;
    bool failed_ = false;

    void clearResources() {
        gpuLease_.reset();
        accountedColor_ = nullptr;
        accountedPreview_ = nullptr;
        accountedAtlas_ = nullptr;
        pipeline_.reset();
        bindings_.reset();
        lookupSampler_.reset();
        tileSampler_.reset();
        previewSampler_.reset();
        pages_.reset();
        atlas_.reset();
        previewTexture_.reset();
        uniform_.reset();
        vertices_.reset();
        slots_.clear();
        renderPass_ = nullptr;
        previewKey_ = 0;
        atlasAttempted_ = false;
        needsClear_ = true;
        failed_ = false;
        submitted_ = false;
    }

    void fail() {
        if (failed_) return;
        failed_ = true;
        const auto canvas = item_;
        const auto generation = generation_;
        if (canvas) QMetaObject::invokeMethod(canvas, [canvas, generation] {
            if (canvas) canvas->graphicsFailed(generation);
        }, Qt::QueuedConnection);
    }

    bool createTextures() {
        const bool needAtlas = !tiles_.isEmpty();
        if (atlas_ && (!needAtlas || atlasAttempted_)) return true;
        atlasAttempted_ = atlasAttempted_ || needAtlas;
        const int cap = std::min(maximumAtlasSide, backend_->resourceLimit(QRhi::TextureSizeMax));
        const int columns = needAtlas ? cap / panoramaTileExtent : 0;
        pipeline_.reset();
        bindings_.reset();
        gpuLease_.reset();
        accountedAtlas_ = nullptr;
        atlasColumns_ = 0;
        for (int candidate = columns; candidate >= 0; --candidate) {
            const int side = std::max(1, candidate * panoramaTileExtent);
            atlas_.reset(backend_->newTexture(QRhiTexture::RGBA8, QSize(side, side)));
            if (atlas_->create()) {
                atlasColumns_ = candidate;
                break;
            }
            atlas_.reset();
        }
        if (!atlas_) return false;
        slots_.resize(size_t(atlasColumns_ * atlasColumns_));
        needsClear_ = true;
        if (!pages_) {
            pages_.reset(backend_->newTexture(QRhiTexture::RGBA8, QSize(pageSide, pageSide)));
            if (!pages_->create()) return false;
        }
        return true;
    }

    void accountGpu() {
        if (!gpuLedger_ || (gpuLease_ && accountedColor_ == colorTexture() &&
            accountedPreview_ == previewTexture_.get() && accountedAtlas_ == atlas_.get())) return;
        accountedColor_ = colorTexture();
        accountedPreview_ = previewTexture_.get();
        accountedAtlas_ = atlas_.get();
        PixelStorageFootprint footprint;
        for (auto* texture : {accountedColor_, accountedPreview_, accountedAtlas_, pages_.get()}) {
            if (!texture) continue;
            const QSize size = texture->pixelSize();
            footprint.addResource(texture, qsizetype(size.width()) * size.height() * 4);
        }
        gpuLease_ = gpuLedger_->retain(std::move(footprint));
    }

    bool prepare(QRhiCommandBuffer* cb) {
        if (failed_ || !backend_ || !createTextures()) return false;
        if (!vertices_) {
            vertices_.reset(backend_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
            uniform_.reset(backend_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                               sizeof(PanoramaUniforms)));
            previewSampler_.reset(backend_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                QRhiSampler::None, QRhiSampler::Repeat, QRhiSampler::ClampToEdge));
            tileSampler_.reset(backend_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
            lookupSampler_.reset(backend_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
            if (!vertices_->create() || !uniform_->create() || !previewSampler_->create() ||
                !tileSampler_->create() || !lookupSampler_->create()) return false;
            auto* updates = backend_->nextResourceUpdateBatch();
            updates->uploadStaticBuffer(vertices_.get(), quad.data());
            cb->resourceUpdate(updates);
        }

        if (preview_.cacheKey() != previewKey_) {
            const int cap = backend_->resourceLimit(QRhi::TextureSizeMax);
            QImage upload = preview_;
            if (upload.width() > cap || upload.height() > cap)
                upload = upload.scaled(QSize(cap, cap), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pipeline_.reset();
            bindings_.reset();
            previewTexture_.reset(backend_->newTexture(QRhiTexture::RGBA8, upload.size()));
            if (!previewTexture_->create()) return false;
            auto* updates = backend_->nextResourceUpdateBatch();
            updates->uploadTexture(previewTexture_.get(), upload);
            cb->resourceUpdate(updates);
            previewKey_ = preview_.cacheKey();
        }

        if (!bindings_) {
            bindings_.reset(backend_->newShaderResourceBindings());
            using Binding = QRhiShaderResourceBinding;
            bindings_->setBindings({
                Binding::uniformBuffer(0, Binding::VertexStage | Binding::FragmentStage, uniform_.get()),
                Binding::sampledTexture(1, Binding::FragmentStage, previewTexture_.get(), previewSampler_.get()),
                Binding::sampledTexture(2, Binding::FragmentStage, atlas_.get(), tileSampler_.get()),
                Binding::sampledTexture(3, Binding::FragmentStage, pages_.get(), lookupSampler_.get()),
            });
            if (!bindings_->create()) return false;
        }

        if (!pipeline_ || renderPass_ != renderTarget()->renderPassDescriptor()) {
            renderPass_ = renderTarget()->renderPassDescriptor();
            pipeline_.reset(backend_->newGraphicsPipeline());
            pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            pipeline_->setSampleCount(renderTarget()->sampleCount());
            pipeline_->setShaderStages({
                {QRhiShaderStage::Vertex, shader(QStringLiteral(":/shaders/panorama.vert.qsb"))},
                {QRhiShaderStage::Fragment, shader(QStringLiteral(":/shaders/panorama.frag.qsb"))},
            });
            QRhiVertexInputLayout layout;
            layout.setBindings({{4 * sizeof(float)}});
            layout.setAttributes({
                {0, 0, QRhiVertexInputAttribute::Float2, 0},
                {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
            });
            pipeline_->setVertexInputLayout(layout);
            pipeline_->setShaderResourceBindings(bindings_.get());
            pipeline_->setRenderPassDescriptor(renderPass_);
            if (!pipeline_->create()) return false;
        }
        return true;
    }

    void updateAtlas(QRhiResourceUpdateBatch* updates) {
        if (!needsClear_ && !atlasDirty_) return;
        int uploaded = 0;
        bool changed = needsClear_;
        int pending = 0;
        ++stamp_;
        // Add at most the number of physical slots to the working set. The
        // front of the list holds a coarse cover and the center of the view.
        const auto selected = wanted_.first(std::min(wanted_.size(), qsizetype(slots_.size())));
        QSet<PanoramaTile> protectedTiles(selected.cbegin(), selected.cend());
        for (const auto& tile : selected) {
            const auto existing = std::ranges::find_if(slots_, [&](const Slot& slot) {
                return slot.valid && slot.tile == tile;
            });
            if (existing != slots_.end()) {
                existing->stamp = stamp_;
                continue;
            }
            const auto image = tiles_.constFind(tile);
            if (image == tiles_.cend()) continue;
            if (uploaded >= (cameraMoving_ ? 1 : uploadsPerFrame)) {
                ++pending;
                continue;
            }
            auto target = slots_.end();
            for (auto slot = slots_.begin(); slot != slots_.end(); ++slot) {
                if (!slot->valid) {
                    target = slot;
                    break;
                }
                if (!protectedTiles.contains(slot->tile) &&
                    (target == slots_.end() || slot->stamp < target->stamp)) target = slot;
            }
            if (target == slots_.end()) break;
            const int index = int(target - slots_.begin());
            QRhiTextureSubresourceUploadDescription description(image.value());
            description.setDestinationTopLeft(QPoint(index % atlasColumns_, index / atlasColumns_) *
                                              panoramaTileExtent);
            updates->uploadTexture(atlas_.get(), QRhiTextureUploadDescription(
                QRhiTextureUploadEntry(0, 0, description)));
            *target = {tile, stamp_, true};
            changed = true;
            ++uploaded;
        }
        if (changed) {
            QImage table(pageSide, pageSide, QImage::Format_RGBA8888);
            table.fill(Qt::transparent);
            std::array<int, 8> y{};
            for (int level = 1; level < levels_.size(); ++level)
                y[size_t(level)] = y[size_t(level - 1)] + tileCount(levels_[level - 1].height());
            for (size_t index = 0; index < slots_.size(); ++index) {
                const auto& slot = slots_[index];
                if (!slot.valid) continue;
                auto* entry = table.scanLine(y[size_t(slot.tile.level)] + slot.tile.row) + slot.tile.column * 4;
                entry[0] = uchar(index % size_t(atlasColumns_));
                entry[1] = uchar(index / size_t(atlasColumns_));
                entry[2] = 255;
            }
            updates->uploadTexture(pages_.get(), table);
            needsClear_ = false;
        }
        atlasDirty_ = pending > 0;
        if (atlasDirty_) update();
    }

protected:
    void initialize(QRhiCommandBuffer*) override {
        if (backend_ == rhi()) return;
        clearResources();
        backend_ = rhi();
    }

    void synchronize(QQuickRhiItem* item) override {
        auto* canvas = static_cast<PanoramaCanvas*>(item);
        item_ = canvas;
        if (!gpuLedger_ && canvas->ledger_) gpuLedger_ = canvas->ledger_->gpuResources();
        imageLease_ = canvas->imageLease_;
        if (tileRevision_ != canvas->tileRevision_) {
            tileRevision_ = canvas->tileRevision_;
            atlasDirty_ = true;
        }
        if (generation_ != canvas->generation_) {
            if (failed_) clearResources();
            generation_ = canvas->generation_;
            std::ranges::fill(slots_, Slot{});
            needsClear_ = true;
            submitted_ = false;
        }
        preview_ = canvas->preview_;
        tiles_ = canvas->tiles_;
        wanted_ = canvas->wanted_;
        levels_ = canvas->levels_;
        camera_ = canvas->cameraRotation();
        lens_ = canvas->lensTangents();
        targetLevel_ = canvas->targetLevel_;
        cameraMoving_ = canvas->cameraInput_.isValid() && canvas->cameraInput_.elapsed() < 140;
    }

    void render(QRhiCommandBuffer* cb) override {
        const QColor background(QStringLiteral("#17191d"));
        if (preview_.isNull() || !prepare(cb)) {
            if (!preview_.isNull()) fail();
            cb->beginPass(renderTarget(), background, {1.F, 0});
            cb->endPass();
            return;
        }
        auto* updates = backend_->nextResourceUpdateBatch();
        updateAtlas(updates);
        PanoramaUniforms data;
        const auto clip = backend_->clipSpaceCorrMatrix();
        QMatrix4x4 camera;
        camera.rotate(camera_);
        std::copy_n(clip.constData(), 16, data.clip.begin());
        std::copy_n(camera.constData(), 16, data.camera.begin());
        data.lens = {float(lens_.x()), float(lens_.y()), 1.0F / float(pageSide), 0.F};
        const float inverseAtlas = 1.0F / float(std::max(1, atlasColumns_ * panoramaTileExtent));
        data.atlas = {inverseAtlas, inverseAtlas, float(std::max(0, targetLevel_)),
                      targetLevel_ >= 0 && atlasColumns_ > 0 ? float(levels_.size()) : 0.F};
        int tableRow = 0;
        for (int level = 0; level < levels_.size(); ++level) {
            const QSize size = levels_[level];
            data.levels[size_t(level)] = {float(size.width()), float(size.height()), float(tableRow), 0.F};
            tableRow += tileCount(size.height());
        }
        updates->updateDynamicBuffer(uniform_.get(), 0, sizeof(data), &data);
        const QSize size = renderTarget()->pixelSize();
        cb->beginPass(renderTarget(), background, {1.F, 0}, updates);
        cb->setGraphicsPipeline(pipeline_.get());
        cb->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
        cb->setShaderResources(bindings_.get());
        const QRhiCommandBuffer::VertexInput input(vertices_.get(), 0);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
        cb->endPass();
        accountGpu();
        if (!submitted_) {
            submitted_ = true;
            const auto canvas = item_;
            const auto generation = generation_;
            if (canvas) QMetaObject::invokeMethod(canvas, [canvas, generation] {
                if (canvas) canvas->frameReady(generation);
            }, Qt::QueuedConnection);
        }
    }
};

PanoramaCanvas::PanoramaCanvas(QQuickItem* parent) : QQuickRhiItem(parent) {
    setAlphaBlending(false);
    setMirrorVertically(false);
    selectionTimer_.setSingleShot(true);
    selectionTimer_.setInterval(28);
    connect(&selectionTimer_, &QTimer::timeout, this, &PanoramaCanvas::selectTiles);
}

PanoramaCanvas::~PanoramaCanvas() {
    if (session_) session_->cancelled = true;
}

void PanoramaCanvas::setPixelLedger(std::shared_ptr<PixelMemoryLedger> ledger) {
    ledger_ = std::move(ledger);
    accountPixels();
}

void PanoramaCanvas::accountPixels() {
    if (!ledger_) return;
    PixelStorageFootprint footprint;
    footprint.add(preview_);
    footprint.add(originalPreview_);
    footprint.add(inspectionImage_);
    for (const auto& tile : std::as_const(tiles_)) footprint.add(tile);
    imageLease_ = footprint.allocations().isEmpty() ? nullptr : ledger_->retain(std::move(footprint));
}

void PanoramaCanvas::open(const QString& path, const PanoramaInfo& info) {
    if (session_) session_->cancelled = true;
    session_.reset();
    selectionTimer_.stop();
    ++generation_;
    info_ = info;
    path_ = path;
    levels_ = panoramaLevels(info.size);
    buildTileBounds();
    tiles_.clear();
    preview_ = {};
    originalPreview_ = {};
    inspectionImage_ = {};
    inspectionTile_.reset();
    pendingInspection_ = false;
    buildingTiles_ = false;
    setPixelReadout({});
    wanted_.clear();
    ++tileRevision_;
    unreadable_.clear();
    availableRows_.clear();
    accountPixels();
    pendingTileRead_ = false;
    targetLevel_ = -1;
    loading_ = !path.isEmpty();
    errorText_.clear();
    detailMessage_.clear();
    resetView();
    cameraInput_.invalidate();
    emit statusChanged();
    if (path.isEmpty()) return;
    firstFrameTimer_.start();
    if (window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        loading_ = false;
        graphicsFailed(generation_);
        return;
    }

    auto session = std::make_shared<Session>();
    session->displaySpace = currentDisplayColorSpace();
    session_ = session;
    const QPointer<PanoramaCanvas> self(this);
    auto* application = QCoreApplication::instance();
    panoramaTasks().imports.start([self, application, session, path, info] {
        const auto cancelled = [session] { return session->cancelled.load() || panoramaTasks().stopping.load(); };
        if (cancelled()) return;
        session->directory = panoramaCacheDirectory(path);
        QImage original = readPanoramaPreview(session->directory);
        const bool warmPreview = !original.isNull();
        if (!warmPreview) original = decodePanoramaPreview(path, info, cancelled);
        if (cancelled()) return;
        const bool ready = !original.isNull();
        const auto existingRows = availablePanoramaRows(session->directory);
        const bool needsTiles = ready && !panoramaLevels(info.size).isEmpty() &&
            original.width() < info.size.width() &&
            !panoramaCacheComplete(session->directory, info.size, existingRows);
        QImage preview = original;
        normalize(preview, session->displaySpace);
        const QImage originalPreview = original.size() == info.size ? original : QImage{};
        QMetaObject::invokeMethod(application, [self, session, preview = std::move(preview),
                                                 originalPreview, needsTiles, existingRows] {
            if (!self || self->session_ != session) return;
            self->preview_ = preview;
            self->originalPreview_ = originalPreview;
            self->availableRows_ = existingRows;
            self->buildingTiles_ = needsTiles;
            self->accountPixels();
            self->loading_ = false;
            if (preview.isNull()) {
                self->errorText_ = self->info_.multipleScans
                    ? tr("This JPEG needs more memory to open in 360°. Convert it to a single-scan JPEG.")
                    : tr("Could not load the panorama.");
            } else if (needsTiles) {
                self->detailMessage_ = tr("Preparing full detail…");
            }
            emit self->statusChanged();
            self->refreshPixelInspection();
            self->selectTiles();
            self->update();
        }, Qt::QueuedConnection);

        if (!ready || cancelled()) return;
        if (!warmPreview && original.width() >= 1024) savePanoramaPreview(session->directory, original);
        original = {};
        if (!needsTiles) return;
        QElapsedTimer buildTimer;
        buildTimer.start();
        const bool built = buildPanoramaTiles(path, info, session->directory, cancelled,
            [self, application, session](int level, int row) {
                QMetaObject::invokeMethod(application, [self, session, level, row] {
                    if (!self || self->session_ != session) return;
                    self->availableRows_.insert(panoramaRowKey(level, row));
                    self->scheduleSelection();
                    if (level == 0) self->refreshPixelInspection();
                }, Qt::QueuedConnection);
            });
        if (cancelled()) return;
        const auto elapsed = buildTimer.elapsed();
        const auto completedRows = built ? availablePanoramaRows(session->directory) : QSet<int>{};
        QMetaObject::invokeMethod(application, [self, session, built, elapsed, completedRows] {
            if (!self || self->session_ != session) return;
            if (built) {
                self->availableRows_ = completedRows;
                self->unreadable_.clear();
            }
            self->buildingTiles_ = false;
            self->detailMessage_ = built ? QString{} : tr("Preview only · Full detail unavailable");
            diagnostics::event(diagnostics::Level::Info, diagnostics::render(), QStringLiteral("panorama.cache_build"),
                {{QStringLiteral("complete"), built}, {QStringLiteral("elapsedMs"), elapsed},
                 {QStringLiteral("rows"), self->availableRows_.size()}});
            emit self->statusChanged();
            self->refreshPixelInspection();
            self->selectTiles();
        }, Qt::QueuedConnection);
    });
}

QPointF PanoramaCanvas::lensTangents() const {
    const double aspect = width() > 0 && height() > 0 ? width() / height() : 16.0 / 9.0;
    if (horizontalFov_ == 0) {
        const double vertical = halfFovTangent(verticalFov_);
        return {std::clamp(vertical * aspect, halfFovTangent(minimumFov), halfFovTangent(maximumFov)), vertical};
    }
    const double horizontal = halfFovTangent(horizontalFov_);
    const double vertical = verticalFov_ > 0 ? halfFovTangent(verticalFov_) : horizontal / aspect;
    return {horizontal, std::clamp(vertical, halfFovTangent(minimumVerticalFov),
                                  halfFovTangent(maximumVerticalFov))};
}

double PanoramaCanvas::fieldOfView() const { return fovFromTangent(lensTangents().x()); }
double PanoramaCanvas::verticalFieldOfView() const { return fovFromTangent(lensTangents().y()); }

QQuaternion PanoramaCanvas::cameraRotation() const {
    const auto imagePose = panoramaRotation(info_.heading, info_.pitch, -info_.roll);
    return imagePose.conjugated() * panoramaRotation(yaw_, pitch_, info_.initialRoll);
}

void PanoramaCanvas::viewChanged() {
    cameraInput_.start();
    emit navigationChanged();
    refreshPixelInspection();
    update();
    scheduleSelection();
}

void PanoramaCanvas::changeDirection(double yaw, double pitch) {
    yaw_ = std::remainder(yaw, 360.0);
    pitch_ = std::clamp(pitch, -90.0, 90.0);
    viewChanged();
}

void PanoramaCanvas::setFieldOfView(double degrees) {
    if (!std::isfinite(degrees)) return;
    const double clamped = std::clamp(degrees, minimumFov, maximumFov);
    if (qFuzzyCompare(clamped, horizontalFov_)) return;
    if (verticalFov_ == 0) verticalFov_ = verticalFieldOfView();
    horizontalFov_ = clamped;
    emit lensChanged();
    viewChanged();
}

void PanoramaCanvas::setVerticalFieldOfView(double degrees) {
    if (!std::isfinite(degrees)) return;
    const double clamped = std::clamp(degrees, minimumVerticalFov, maximumVerticalFov);
    if (qFuzzyCompare(clamped, verticalFov_)) return;
    if (horizontalFov_ == 0) horizontalFov_ = fieldOfView();
    verticalFov_ = clamped;
    emit lensChanged();
    viewChanged();
}

void PanoramaCanvas::turn(int horizontal, int vertical) {
    changeDirection(yaw_ + horizontal * 5 * fieldOfView() / defaultFov,
                    pitch_ + vertical * 5 * verticalFieldOfView() / 45);
}

void PanoramaCanvas::drag(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    const auto lens = lensTangents();
    changeDirection(yaw_ - x * qRadiansToDegrees(2 * lens.x()) / std::max(1.0, width()),
                    pitch_ + y * qRadiansToDegrees(2 * lens.y()) / std::max(1.0, height()));
}

void PanoramaCanvas::zoom(double steps) {
    if (!std::isfinite(steps)) return;
    const double scale = std::pow(0.87, steps);
    const auto zoomAxis = [scale](double degrees, double minimum, double maximum) {
        return degrees > 0
            ? std::clamp(fovFromTangent(halfFovTangent(degrees) * scale), minimum, maximum)
            : 0.0;
    };
    const double horizontal = zoomAxis(horizontalFov_, minimumFov, maximumFov);
    const double vertical = zoomAxis(verticalFov_, minimumVerticalFov, maximumVerticalFov);
    if (qFuzzyCompare(horizontal, horizontalFov_) && qFuzzyCompare(vertical, verticalFov_)) return;
    horizontalFov_ = horizontal;
    verticalFov_ = vertical;
    emit lensChanged();
    viewChanged();
}

void PanoramaCanvas::resetView() {
    horizontalFov_ = info_.initialHorizontalFov > 0 && info_.initialHorizontalFov < 180
        ? std::clamp(info_.initialHorizontalFov, minimumFov, maximumFov) : 0;
    verticalFov_ = info_.initialVerticalFov > 0 && info_.initialVerticalFov < 180
        ? std::clamp(info_.initialVerticalFov, minimumVerticalFov, maximumVerticalFov) : 0;
    if (horizontalFov_ == 0 && verticalFov_ == 0) horizontalFov_ = defaultFov;
    emit lensChanged();
    changeDirection(info_.initialHeading, info_.initialPitch);
}

void PanoramaCanvas::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size()) return;
    emit lensChanged();
    viewChanged();
}

void PanoramaCanvas::lookAtSource(double u, double v) {
    if (!std::isfinite(u) || !std::isfinite(v)) return;
    const auto ray = direction((std::clamp(u, 0.0, 1.0) - 0.5) * 2 * pi,
                               (0.5 - std::clamp(v, 0.0, 1.0)) * pi);
    const auto imagePose = panoramaRotation(info_.heading, info_.pitch, -info_.roll);
    const auto worldLocation = sourceLocation(imagePose.rotatedVector(ray));
    changeDirection((worldLocation.x() - 0.5) * 360, (0.5 - worldLocation.y()) * 180);
}

QVariantList PanoramaCanvas::navigationRectangles() const {
    const auto pose = cameraRotation();
    const auto lens = lensTangents();
    const float x = float(lens.x()), y = float(lens.y());
    const std::array corners{pose.rotatedVector({-x, y, 1}), pose.rotatedVector({x, y, 1}),
                             pose.rotatedVector({x, -y, 1}), pose.rotatedVector({-x, -y, 1})};
    const double center = sourceLocation(pose.rotatedVector({0, 0, 1})).x();
    double left = center, right = center, top = 1, bottom = 0;
    for (const auto& corner : corners) {
        const auto location = sourceLocation(corner);
        const double u = center + std::remainder(location.x() - center, 1.0);
        left = std::min(left, u);
        right = std::max(right, u);
        top = std::min(top, location.y());
        bottom = std::max(bottom, location.y());
    }
    // Latitude extrema may lie between two corners (for example when tilted).
    for (size_t i = 0; i < corners.size(); ++i) {
        const auto& a = corners[i];
        const auto b = corners[(i + 1) % corners.size()] - a;
        const double aa = QVector3D::dotProduct(a, a);
        const double ab = QVector3D::dotProduct(a, b);
        const double bb = QVector3D::dotProduct(b, b);
        const double denominator = a.y() * bb - b.y() * ab;
        if (qFuzzyIsNull(denominator)) continue;
        const double t = (b.y() * aa - a.y() * ab) / denominator;
        if (t <= 0 || t >= 1) continue;
        const double v = sourceLocation(a + float(t) * b).y();
        top = std::min(top, v);
        bottom = std::max(bottom, v);
    }
    const auto inverse = pose.conjugated();
    const auto includesPole = [inverse, x, y](float pole) {
        const auto local = inverse.rotatedVector({0, pole, 0});
        return local.z() > 0 && std::abs(local.x()) <= x * local.z()
                             && std::abs(local.y()) <= y * local.z();
    };
    if (includesPole(1)) return {QRectF(0, 0, 1, bottom)};
    if (includesPole(-1)) return {QRectF(0, top, 1, 1 - top)};
    if (left < 0) return {QRectF(left + 1, top, -left, bottom - top),
                          QRectF(0, top, right, bottom - top)};
    if (right > 1) return {QRectF(left, top, 1 - left, bottom - top),
                           QRectF(0, top, right - 1, bottom - top)};
    return {QRectF(left, top, right - left, bottom - top)};
}

void PanoramaCanvas::inspectPixel(const QPointF& position) {
    inspectionPosition_ = position;
    refreshPixelInspection();
}

void PanoramaCanvas::clearPixelInspection() {
    inspectionPosition_.reset();
    setPixelReadout({});
}

void PanoramaCanvas::setPixelReadout(const QString& text) {
    if (pixelReadout_ == text) return;
    pixelReadout_ = text;
    emit pixelReadoutChanged();
}

void PanoramaCanvas::refreshPixelInspection() {
    if (!navigationReady() || !inspectionPosition_ || !contains(*inspectionPosition_) ||
        width() <= 0 || height() <= 0) {
        setPixelReadout({});
        return;
    }
    const auto lens = lensTangents();
    const auto point = *inspectionPosition_;
    const auto source = sourceLocation(cameraRotation().rotatedVector({
        float((2 * point.x() / width() - 1) * lens.x()),
        float((1 - 2 * point.y() / height()) * lens.y()), 1}));
    const int x = std::clamp(int(source.x() * info_.size.width()), 0, info_.size.width() - 1);
    const int y = std::clamp(int(source.y() * info_.size.height()), 0, info_.size.height() - 1);
    const QString coordinates = QStringLiteral("(%1,%2) ").arg(x).arg(y);
    const auto show = [this, &coordinates](QRgb rgb) {
        setPixelReadout(coordinates + QStringLiteral("RGB(%1, %2, %3)")
                       .arg(qRed(rgb)).arg(qGreen(rgb)).arg(qBlue(rgb)));
    };
    if (!originalPreview_.isNull()) {
        show(originalPreview_.pixel(x, y));
        return;
    }
    const PanoramaTile tile{0, x / panoramaTileSize, y / panoramaTileSize};
    if (inspectionTile_ == tile) {
        if (inspectionImage_.isNull()) {
            setPixelReadout(coordinates + tr("Pixel unavailable"));
        } else {
            show(inspectionImage_.pixel(x % panoramaTileSize + panoramaTileBorder,
                                        y % panoramaTileSize + panoramaTileBorder));
        }
        return;
    }

    const bool rowReady = availableRows_.contains(panoramaRowKey(tile.level, tile.row));
    setPixelReadout(coordinates + (rowReady || buildingTiles_ ? tr("Reading pixel…") : tr("Pixel unavailable")));
    if (!rowReady || pendingInspection_ || !session_) return;

    pendingInspection_ = true;
    const auto session = session_;
    const QPointer<PanoramaCanvas> self(this);
    auto* application = QCoreApplication::instance();
    panoramaTasks().reads.start([self, application, session, tile] {
        if (session->cancelled || panoramaTasks().stopping) return;
        auto image = readPanoramaTile(session->directory, tile);
        QMetaObject::invokeMethod(application, [self, session, tile, image = std::move(image)] {
            if (!self || self->session_ != session) return;
            self->inspectionTile_ = tile;
            self->inspectionImage_ = image;
            self->pendingInspection_ = false;
            self->accountPixels();
            self->refreshPixelInspection();
        }, Qt::QueuedConnection);
    });
}

void PanoramaCanvas::scheduleSelection() {
    if (!selectionTimer_.isActive()) selectionTimer_.start();
}

void PanoramaCanvas::buildTileBounds() {
    tileBounds_.clear();
    for (int level = 0; level < levels_.size(); ++level) {
        const auto raster = levels_[level];
        QList<TileBounds> bounds;
        bounds.reserve(tileCount(raster.width()) * tileCount(raster.height()));
        for (int row = 0; row < tileCount(raster.height()); ++row) {
            const double top = (0.5 - double(row * panoramaTileSize) / raster.height()) * pi;
            const double bottom = (0.5 - double(std::min((row + 1) * panoramaTileSize, raster.height()))
                                              / raster.height()) * pi;
            const double latitude = (top + bottom) * 0.5;
            const double nearestEquator = std::clamp(0.0, bottom, top);
            for (int column = 0; column < tileCount(raster.width()); ++column) {
                const double left = (double(column * panoramaTileSize) / raster.width() - 0.5) * 2 * pi;
                const double right = (double(std::min((column + 1) * panoramaTileSize, raster.width()))
                                      / raster.width() - 0.5) * 2 * pi;
                const double arc = std::min(pi, (top - bottom) * 0.5 +
                                             (right - left) * 0.5 * std::cos(nearestEquator));
                bounds.append({{level, column, row}, direction((left + right) * 0.5, latitude),
                               float(2 * std::sin(arc * 0.5))});
            }
        }
        tileBounds_.append(std::move(bounds));
    }
}

QList<PanoramaTile> PanoramaCanvas::visibleTiles(int level, const QQuaternion& camera,
                                                double horizontal, double vertical) const {
    // Rotating the five planes once also avoids accumulating round-off across
    // the precomputed conservative bounds at the seam and poles.
    std::array<QVector3D, 5> planes{
        QVector3D(1, 0, float(horizontal)).normalized(),
        QVector3D(-1, 0, float(horizontal)).normalized(),
        QVector3D(0, 1, float(vertical)).normalized(),
        QVector3D(0, -1, float(vertical)).normalized(), QVector3D(0, 0, 1),
    };
    for (auto& plane : planes) plane = camera.rotatedVector(plane);
    struct Candidate { PanoramaTile tile; float depth; };
    std::vector<Candidate> visible;
    for (const auto& bound : tileBounds_[level]) {
        if (std::ranges::any_of(planes, [&](const QVector3D& plane) {
                return QVector3D::dotProduct(plane, bound.center) < -bound.radius;
            })) continue;
        visible.push_back({bound.tile, QVector3D::dotProduct(planes.back(), bound.center)});
    }
    std::ranges::sort(visible, std::greater<>{}, &Candidate::depth);
    QList<PanoramaTile> result;
    result.reserve(qsizetype(visible.size()));
    for (const auto& candidate : visible) result.append(candidate.tile);
    return result;
}

void PanoramaCanvas::selectTiles() {
    if (preview_.isNull() || levels_.isEmpty() || !session_ || width() <= 0 || height() <= 0) {
        update();
        return;
    }
    const double ratio = window() ? window()->effectiveDevicePixelRatio() : 1;
    const auto lens = lensTangents();
    const double required = ratio * pi * std::max(width() / lens.x(), height() / lens.y());
    if (preview_.width() >= info_.size.width() ||
        required < preview_.width() * (targetLevel_ < 0 ? 1.1 : 0.9)) {
        if (targetLevel_ >= 0) {
            targetLevel_ = -1;
            wanted_.clear();
            ++tileRevision_;
        }
        update();
        return;
    }
    int level = 0;
    while (level + 1 < levels_.size() && levels_[level + 1].width() >= required) ++level;
    if (targetLevel_ > level && levels_[targetLevel_].width() >= required * 0.88)
        level = targetLevel_;
    if (targetLevel_ >= 0 && targetLevel_ < level && levels_[level].width() <= required * 1.12)
        level = targetLevel_;
    const bool levelChanged = targetLevel_ != level;
    targetLevel_ = level;

    QList<PanoramaTile> wanted;
    const auto pose = cameraRotation();
    if (level + 1 < levels_.size() && levels_[level + 1].width() > preview_.width()) {
        const auto parents = visibleTiles(level + 1, pose, lens.x(), lens.y());
        wanted.append(parents.first(std::min(qsizetype(8), parents.size())));
    }
    const auto visible = visibleTiles(level, pose, lens.x(), lens.y());
    wanted.append(visible.first(std::min(qsizetype(40), visible.size())));
    if (levelChanged || wanted_ != wanted) {
        wanted_ = std::move(wanted);
        ++tileRevision_;
        update();
    }
    loadVisibleTiles();
}

void PanoramaCanvas::loadVisibleTiles() {
    if (pendingTileRead_ || !session_) return;
    QList<PanoramaTile> missing;
    for (const auto& tile : std::as_const(wanted_)) {
        if (tiles_.contains(tile) || unreadable_.contains(tile) ||
            !availableRows_.contains(panoramaRowKey(tile.level, tile.row))) continue;
        missing.append(tile);
        if (missing.size() == 3) break;
    }
    if (missing.isEmpty()) return;

    pendingTileRead_ = true;
    const auto session = session_;
    const QPointer<PanoramaCanvas> self(this);
    auto* application = QCoreApplication::instance();
    panoramaTasks().reads.start([self, application, session, missing] {
        QHash<PanoramaTile, QImage> available;
        for (const auto& tile : missing) {
            if (session->cancelled || panoramaTasks().stopping) return;
            QImage image = readPanoramaTile(session->directory, tile);
            if (image.isNull()) invalidatePanoramaCache(session->directory);
            normalize(image, session->displaySpace);
            available.insert(tile, std::move(image));
        }
        QMetaObject::invokeMethod(application, [self, session, available = std::move(available)] {
            if (!self || self->session_ != session) return;
            self->pendingTileRead_ = false;
            const QSet<PanoramaTile> wanted(self->wanted_.cbegin(), self->wanted_.cend());
            for (auto tile = available.cbegin(); tile != available.cend(); ++tile) {
                if (tile.value().isNull()) {
                    self->unreadable_.insert(tile.key());
                } else if (wanted.contains(tile.key())) {
                    if (self->tiles_.size() >= cpuTileLimit) {
                        const auto discard = std::find_if(self->tiles_.keyBegin(), self->tiles_.keyEnd(),
                            [&wanted](const PanoramaTile& candidate) { return !wanted.contains(candidate); });
                        if (discard != self->tiles_.keyEnd()) self->tiles_.remove(*discard);
                    }
                    self->tiles_.insert(tile.key(), tile.value());
                }
            }
            ++self->tileRevision_;
            self->accountPixels();
            self->update();
            self->loadVisibleTiles();
        }, Qt::QueuedConnection);
    });
}

void PanoramaCanvas::frameReady(quint64 generation) {
    if (generation != generation_) return;
    diagnostics::event(diagnostics::Level::Info, diagnostics::render(), QStringLiteral("panorama.first_frame"),
        {{QStringLiteral("elapsedMs"), firstFrameTimer_.elapsed()},
         {QStringLiteral("sourceWidth"), info_.size.width()},
         {QStringLiteral("previewWidth"), preview_.width()}});
    emit imageFrameRendered();
}

void PanoramaCanvas::graphicsFailed(quint64 generation) {
    if (generation != generation_) return;
    errorText_ = tr("The graphics device could not display this panorama.");
    emit statusChanged();
}

QQuickRhiItemRenderer* PanoramaCanvas::createRenderer() { return new PanoramaRenderer; }

} // namespace mvpview
