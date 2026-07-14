// Phase 1 PoC: headless QQuickRenderControl render -> FFmpeg mux.
// See docs/architecture/video-factory-design.md §2 (rendering architecture)
// and §8 (Phase 1 scope: static QML content only, no LLM/RAG yet).
//
// RHI offscreen render sequence follows the official Qt 6.11
// "QQuickRenderControl RHI Example"
// (https://doc.qt.io/qt-6/qtquick-rendercontrol-rendercontrol-rhi-example.html).
//
// Note: do NOT set QT_QPA_PLATFORM=offscreen. Qt's "offscreen" QPA plugin
// cannot create the D3D11/OpenGL/Vulkan context QRhi needs, so
// QQuickRenderControl::initialize() fails. The default "windows" platform
// integration is required even though no window is ever shown on screen
// (QQuickRenderControl never calls show() on the QQuickWindow).

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QUrl>

#include <rhi/qrhi.h>

#include <QDebug>
#include <memory>

#include "encode/video_encoder.h"

namespace {

constexpr int kFrameWidth = 1280;
constexpr int kFrameHeight = 720;
constexpr int kFps = 30;
constexpr int kDurationSeconds = 3;
constexpr int kFrameCount = kFps * kDurationSeconds;

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    QQuickRenderControl renderControl;
    QQuickWindow quickWindow(&renderControl);

    QQmlEngine qmlEngine;
    QQmlComponent component(&qmlEngine, QUrl::fromLocalFile(QStringLiteral(TUTORIAL_SCENE_QML_PATH)));
    if (component.status() != QQmlComponent::Ready) {
        qCritical() << "Failed to load QML scene:" << component.errorString();
        return 1;
    }

    std::unique_ptr<QObject> rootObject(component.create());
    auto* rootItem = qobject_cast<QQuickItem*>(rootObject.get());
    if (!rootItem) {
        qCritical() << "Root QML object is not a QQuickItem";
        return 1;
    }

    rootItem->setParentItem(quickWindow.contentItem());
    quickWindow.contentItem()->setSize(QSizeF(kFrameWidth, kFrameHeight));
    quickWindow.setGeometry(0, 0, kFrameWidth, kFrameHeight);

    if (!renderControl.initialize()) {
        qCritical() << "QQuickRenderControl::initialize() failed";
        return 1;
    }

    QRhi* rhi = renderControl.rhi();
    if (!rhi) {
        qCritical() << "No QRhi available after initialize()";
        return 1;
    }

    const QSize pixelSize(kFrameWidth, kFrameHeight);

    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        qCritical() << "Failed to create offscreen render texture";
        return 1;
    }

    std::unique_ptr<QRhiRenderBuffer> depthStencil(
        rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!depthStencil->create()) {
        qCritical() << "Failed to create depth/stencil buffer";
        return 1;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(texture.get()));
    rtDesc.setDepthStencilBuffer(depthStencil.get());
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget(rhi->newTextureRenderTarget(rtDesc));
    std::unique_ptr<QRhiRenderPassDescriptor> renderPassDesc(
        renderTarget->newCompatibleRenderPassDescriptor());
    renderTarget->setRenderPassDescriptor(renderPassDesc.get());
    if (!renderTarget->create()) {
        qCritical() << "Failed to create QRhiTextureRenderTarget";
        return 1;
    }

    quickWindow.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(renderTarget.get()));

    VideoEncoder encoder("phase1_poc.mp4", kFrameWidth, kFrameHeight, kFps);

    for (int i = 0; i < kFrameCount; ++i) {
        rootItem->setProperty("progress", static_cast<double>(i) / (kFrameCount - 1));

        renderControl.polishItems();
        renderControl.beginFrame();
        renderControl.sync();
        renderControl.render();

        QRhiReadbackResult readResult;
        QRhiResourceUpdateBatch* readbackBatch = rhi->nextResourceUpdateBatch();
        readbackBatch->readBackTexture(texture.get(), &readResult);
        renderControl.commandBuffer()->resourceUpdate(readbackBatch);

        renderControl.endFrame();

        QImage frameImage(reinterpret_cast<const uchar*>(readResult.data.constData()),
                           readResult.pixelSize.width(), readResult.pixelSize.height(),
                           QImage::Format_RGBA8888_Premultiplied);
        if (rhi->isYUpInFramebuffer()) {
            frameImage = frameImage.flipped();
        }
        frameImage = frameImage.convertToFormat(QImage::Format_RGBA8888);

        encoder.pushFrame(frameImage.constBits());

        if (i % kFps == 0) {
            qDebug() << "Rendered frame" << i << "/" << kFrameCount;
        }
    }

    encoder.finish();
    qDebug() << "Wrote phase1_poc.mp4 (" << kFrameCount << "frames )";

    return 0;
}
