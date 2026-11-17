#include "scene/scene_assembler.h"

#include <QQuickItem>
#include <QQuickRenderTarget>
#include <QSize>
#include <QUrl>

#include <rhi/qrhi.h>

SceneAssembler::SceneAssembler(int frameWidth, int frameHeight, QString qmlScenePath)
    : frameWidth_(frameWidth),
      frameHeight_(frameHeight),
      qmlScenePath_(std::move(qmlScenePath)),
      quickWindow_(&renderControl_) {}

// Out-of-line (not "= default" in the header): std::unique_ptr's deleter
// needs the QRhi* member types to be complete at the point it runs, which
// is here, after <rhi/qrhi.h> above -- not at the header, which only
// forward-declares them (see scene_assembler.h's comment).
SceneAssembler::~SceneAssembler() = default;

bool SceneAssembler::initialize(const StaticProperties& staticProps, QString* errorMessage) {
    component_ = std::make_unique<QQmlComponent>(&qmlEngine_, QUrl::fromLocalFile(qmlScenePath_));
    if (component_->status() != QQmlComponent::Ready) {
        *errorMessage =
            QStringLiteral("Failed to load QML scene: %1").arg(component_->errorString());
        return false;
    }

    rootObject_.reset(component_->create());
    rootItem_ = qobject_cast<QQuickItem*>(rootObject_.get());
    if (!rootItem_) {
        *errorMessage = QStringLiteral("Root QML object is not a QQuickItem");
        return false;
    }

    // Static (once-per-video) properties for the split-screen chapter layout
    // (ref: KISARAGI-style design-process reel -- brand block + chapter
    // counter + segmented footer timeline + technical metadata line).
    rootItem_->setProperty("topic", staticProps.topic);
    rootItem_->setProperty("brandLabel", staticProps.brandLabel);
    rootItem_->setProperty("slideCount", staticProps.slideCount);
    rootItem_->setProperty("metadataLine", staticProps.metadataLine);
    rootItem_->setProperty("slideBoundaries", staticProps.slideBoundaries);

    // 書き出す大きさを QML 側にも伝える。CloudRagScene.qml は 1280 幅を基準の設計として、
    // この幅に合わせて全体を拡大する(1920x1080 でも、文字・図が同じ割合で大きくなる)。
    rootItem_->setSize(QSizeF(frameWidth_, frameHeight_));
    rootItem_->setParentItem(quickWindow_.contentItem());
    quickWindow_.contentItem()->setSize(QSizeF(frameWidth_, frameHeight_));
    quickWindow_.setGeometry(0, 0, frameWidth_, frameHeight_);

    if (!renderControl_.initialize()) {
        *errorMessage = QStringLiteral("QQuickRenderControl::initialize() failed");
        return false;
    }

    rhi_ = renderControl_.rhi();
    if (!rhi_) {
        *errorMessage = QStringLiteral("No QRhi available after initialize()");
        return false;
    }

    const QSize pixelSize(frameWidth_, frameHeight_);

    texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, pixelSize, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture_->create()) {
        *errorMessage = QStringLiteral("Failed to create offscreen render texture");
        return false;
    }

    depthStencil_.reset(rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!depthStencil_->create()) {
        *errorMessage = QStringLiteral("Failed to create depth/stencil buffer");
        return false;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(texture_.get()));
    rtDesc.setDepthStencilBuffer(depthStencil_.get());
    renderTarget_.reset(rhi_->newTextureRenderTarget(rtDesc));
    renderPassDesc_.reset(renderTarget_->newCompatibleRenderPassDescriptor());
    renderTarget_->setRenderPassDescriptor(renderPassDesc_.get());
    if (!renderTarget_->create()) {
        *errorMessage = QStringLiteral("Failed to create QRhiTextureRenderTarget");
        return false;
    }

    quickWindow_.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(renderTarget_.get()));
    return true;
}

QImage SceneAssembler::renderFrame(const FrameProperties& frameProps) {
    rootItem_->setProperty("progress", frameProps.progress);
    rootItem_->setProperty("slideIndex", frameProps.slideIndex);
    rootItem_->setProperty("slideHeading", frameProps.slideHeading);
    rootItem_->setProperty("slideBullet1", frameProps.slideBullet1);
    rootItem_->setProperty("slideBullet2", frameProps.slideBullet2);
    rootItem_->setProperty("slideCodeBlock", frameProps.slideCodeBlock);
    rootItem_->setProperty("slideReferenceItems", frameProps.slideReferenceItems);
    rootItem_->setProperty("slideProgress", frameProps.slideProgress);
    rootItem_->setProperty("slideDiagramSource", frameProps.slideDiagramSource);

    renderControl_.polishItems();
    renderControl_.beginFrame();
    renderControl_.sync();
    renderControl_.render();

    QRhiReadbackResult readResult;
    QRhiResourceUpdateBatch* readbackBatch = rhi_->nextResourceUpdateBatch();
    readbackBatch->readBackTexture(texture_.get(), &readResult);
    renderControl_.commandBuffer()->resourceUpdate(readbackBatch);

    renderControl_.endFrame();

    // 画面全体を不透明な Rectangle で塗っているので、アルファは常に 255。この場合、premultiplied と
    // straight は同じバイト列になる。convertToFormat(RGBA8888) は 1 フレームごとに全画素を変換していて、
    // 1080p では無駄が大きいため、最初から RGBA8888 として読み、コピーだけにした(2026-11-17)。
    // readResult はこの関数を出ると消えるので、コピーは必要(flipped() も、コピーを返す)。
    const QImage readback(reinterpret_cast<const uchar*>(readResult.data.constData()),
                          readResult.pixelSize.width(), readResult.pixelSize.height(),
                          QImage::Format_RGBA8888);
    return rhi_->isYUpInFramebuffer() ? readback.flipped() : readback.copy();
}
