#pragma once

#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QString>
#include <QVariantList>

#include <memory>

// Forward declarations only -- the QRhi resource chain is only ever
// touched inside scene_assembler.cpp (after including <rhi/qrhi.h> there),
// so callers of this header don't need to pull in the (heavy) rhi/qrhi.h
// themselves. This is why SceneAssembler declares (rather than defaults)
// its destructor in this header: std::unique_ptr's default deleter needs
// the complete type at the point it's invoked, which for these members is
// scene_assembler.cpp, not any translation unit that merely includes this
// header.
class QQuickItem;
class QRhi;
class QRhiTexture;
class QRhiRenderBuffer;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;

// Owns the headless-rendering machinery (QQuickRenderControl + offscreen
// QQuickWindow + the QRhi texture/render-target chain) that main_cloudrag.cpp
// used to set up inline, per docs/architecture/video-factory-design.md §2
// and IMPROVEMENT_PLAN.md Phase 3.
//
// Scope note (IMPROVEMENT_PLAN.md Phase 3): the design doc's original
// sketch has SceneAssembler take a ShotList (Phase 2) and bind it as QML
// model data, replacing the flat per-frame property assignments the
// current CloudRagScene.qml contract uses. That would mean rewriting
// CloudRagScene.qml's entire data-binding contract -- untested, high-risk,
// and not something this phase's actual goal (encapsulate the render-
// control/QRhi machinery behind a class boundary) requires. This class
// keeps the existing, proven flat-property contract (FrameProperties
// below mirrors exactly what main_cloudrag.cpp's render loop used to set
// via rootItem->setProperty() per frame) -- ShotList remains available
// from ScriptComposer for a future pass that actually changes the QML
// binding model, which is a separable decision from this extraction.
//
// CPU-readback only (design doc §2's "GPUテクスチャ→エンコーダのゼロコピー
// 経路" stretch goal is not attempted here, per Phase 3's own checklist:
// zero-copy is explicitly not a completion condition) -- renderFrame()
// returns a QImage the caller hands to VideoEncoder, exactly as
// main_cloudrag.cpp's render loop did before this extraction.
class SceneAssembler {
public:
    // Once-per-video properties -- see CloudRagScene.qml's header comment
    // for what each one drives on screen.
    struct StaticProperties {
        QString topic;
        QString brandLabel;
        int slideCount = 1;
        QString metadataLine;
        QVariantList slideBoundaries;
    };

    // Per-frame properties -- one instance per renderFrame() call, mirrors
    // CloudRagScene.qml's per-frame property set exactly (see that file's
    // header comment for what each drives).
    struct FrameProperties {
        double progress = 0.0;
        int slideIndex = 0;
        QString slideHeading;
        QString slideBullet1;
        QString slideBullet2;
        QString slideCodeBlock;
        QVariantList slideReferenceItems;
        QString slideDiagramSource;
        double slideProgress = 0.0;
    };

    SceneAssembler(int frameWidth, int frameHeight, QString qmlScenePath);
    ~SceneAssembler();

    SceneAssembler(const SceneAssembler&) = delete;
    SceneAssembler& operator=(const SceneAssembler&) = delete;

    // Loads the QML scene, sets the static properties, and initializes the
    // QQuickRenderControl/QRhi/offscreen-texture chain. Returns false (with
    // *errorMessage set) on any failure -- callers should treat that the
    // same way main_cloudrag.cpp previously treated its own inline setup
    // failures (log and abort the job), see main_cloudrag.cpp for the
    // exact prior error strings this preserves.
    bool initialize(const StaticProperties& staticProps, QString* errorMessage);

    // Renders one frame and reads it back to a CPU-side QImage
    // (Format_RGBA8888, frameWidth x frameHeight as given to the
    // constructor). Must only be called after a successful initialize().
    QImage renderFrame(const FrameProperties& frameProps);

private:
    int frameWidth_;
    int frameHeight_;
    QString qmlScenePath_;

    QQuickRenderControl renderControl_;
    QQuickWindow quickWindow_;
    QQmlEngine qmlEngine_;
    std::unique_ptr<QQmlComponent> component_;
    std::unique_ptr<QObject> rootObject_;
    QQuickItem* rootItem_ = nullptr;  // non-owning, aliases rootObject_

    QRhi* rhi_ = nullptr;  // non-owning, owned by renderControl_
    std::unique_ptr<QRhiTexture> texture_;
    std::unique_ptr<QRhiRenderBuffer> depthStencil_;
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget_;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPassDesc_;
};
