import QtQuick

// Phase 2.6: split-screen "design process reel" video format (ref: a
// KISARAGI-style product-design portfolio video the user shared -- left
// info panel + big right-side visual + footer chapter timeline). Replaces
// the earlier single full-bleed card layout: body text is no longer shown
// on screen at all (it's still spoken via narration -- see
// stripMarkdownForNarration in main_cloudrag.cpp), traded for two short
// "at a glance" bullet facts plus a large visual (diagram / code block /
// abstract gradient) so each chapter reads instantly rather than asking the
// viewer to read a paragraph in a few seconds.
//
// Static (once-per-video) properties set from C++:
//   topic          - original question text (kept for reference, not shown
//                    directly; the per-slide heading is the on-screen title)
//   brandLabel     - dbKey, uppercased (e.g. "HOUDINI21") -- stands in for
//                    a fixed product/brand wordmark in the reference design
//   slideCount     - total slide count, for the "N / M" chapter counter
//   slideBoundaries - list of 0..1 fractions where each slide starts
//                    (excluding the implicit 0.0/1.0 ends), for the footer's
//                    segmented timeline tick marks
//   metadataLine   - "<duration> SEC / <fps> FPS / <w> × <h> / BT.709"
//
// Output size (2026-11-17): the scene is designed on a 1280-wide "stage" and the whole
// stage is scaled to the real frame (root.width x root.height, set from C++ by
// SceneAssembler), so 1920x1080 (the default) and 1280x720 look the same, just sharper.
// Text and shapes are re-rasterised at the scaled size, they are not upscaled bitmaps.
//
// Per-frame properties set from C++ (main_cloudrag.cpp's render loop):
//   progress       - 0..1 across the whole video (footer timeline fill)
//   slideIndex     - 0-based index of the currently active slide
//   slideHeading   - current slide's big headline text
//   slideBullet1 / slideBullet2 - two short "at a glance" facts
//   slideCodeBlock - non-empty if this slide's right-panel visual is a code
//                    block instead of a diagram
//   slideDiagramSource - file:// URL of a rendered Mermaid PNG; empty if
//                    this slide has no diagram (code block or gradient
//                    fallback instead)
//   slideReferenceItems - Houdini-tutorial mode only: list of
//                    {title, db, cited} objects for the "参考" slide's
//                    source-card visual; empty for every other slide.
//   slideProgress  - 0..1 within the current slide's own on-screen window;
//                    drives the fade in/out transition at slide boundaries.
Rectangle {
    id: root
    // Overwritten by SceneAssembler::initialize() with the real frame size.
    width: 1920
    height: 1080
    color: "#0c0f0d"

    // 設計の基準は 1280 幅。実際の幅に合わせて全体(stage)を拡大する。
    readonly property real designWidth: 1280
    readonly property real uiScale: width / designWidth

    property real progress: 0.0
    property string topic: ""
    property string brandLabel: ""
    property int slideIndex: 0
    property int slideCount: 1
    property var slideBoundaries: []
    property string metadataLine: ""
    property string slideHeading: ""
    property string slideBullet1: ""
    property string slideBullet2: ""
    property string slideCodeBlock: ""
    property string slideDiagramSource: ""
    property var slideReferenceItems: []
    property real slideProgress: 0.0

    readonly property string uiFontFamily: "Yu Gothic UI"
    readonly property string monoFontFamily: "Consolas"

    // "解説" (explainer) / "図解" (diagram) / "コード例" (code example) /
    // "参考資料" (reference sources) -- a lightweight per-slide category,
    // standing in for the reference design's per-chapter category badge
    // (e.g. "FUNCTIONAL DESIGN").
    readonly property string slideKind: slideCodeBlock.length > 0
        ? "コード例"
        : (slideReferenceItems.length > 0
            ? "参考資料"
            : (slideDiagramSource.length > 0 ? "図解" : "解説"))

    // Fades slide content in/out over the first/last 12% of its on-screen
    // window -- a clean transition between chapters without needing to
    // cross-fade old/new content simultaneously.
    readonly property real slideOpacity: {
        const fadeWindow = 0.12;
        return Math.max(0, Math.min(1, Math.min(slideProgress / fadeWindow,
                                                 (1 - slideProgress) / fadeWindow)));
    }

    // 右の大きな画面(Houdini のキャプチャ・図・コード)を広げるため、左を 420→380、下の帯を 108→96 にした。
    readonly property int footerHeight: 96
    readonly property int leftPanelWidth: 380

    // 設計上の大きさで描く土台。すべての部品はこの中に置く。
    Item {
        id: stage
        width: root.designWidth
        height: root.height / root.uiScale
        scale: root.uiScale
        transformOrigin: Item.TopLeft

        // ===== Left info panel =====
        Rectangle {
            id: leftPanel
            x: 0; y: 0
            width: root.leftPanelWidth
            height: stage.height - root.footerHeight
            color: "#10140f"

            Text {
                id: brandText
                x: 48; y: 48
                text: root.brandLabel.length > 0 ? root.brandLabel : "CLOUD RAG"
                color: "#f5f2e8"
                font.family: root.uiFontFamily
                font.pixelSize: 30
                font.bold: true
                font.letterSpacing: 1
            }
            Text {
                x: 48; y: brandText.y + brandText.height + 2
                text: "TUTORIAL VIDEO FACTORY"
                color: "#ff9d5c"
                font.family: root.uiFontFamily
                font.pixelSize: 13
                font.bold: true
                font.letterSpacing: 2
            }

            Text {
                id: chapterCounter
                x: 48; y: 176
                text: (root.slideIndex + 1) + " / " + root.slideCount
                color: "#ff9d5c"
                font.family: root.monoFontFamily
                font.pixelSize: 15
                font.bold: true
                opacity: root.slideOpacity
            }

            Rectangle {
                id: kindBadge
                x: 48; y: chapterCounter.y + chapterCounter.height + 10
                width: kindBadgeText.implicitWidth + 28
                height: kindBadgeText.implicitHeight + 16
                radius: 4
                color: "#ff8a45"
                opacity: root.slideOpacity

                Text {
                    id: kindBadgeText
                    anchors.centerIn: parent
                    text: root.slideKind
                    color: "#1a1408"
                    font.family: root.uiFontFamily
                    font.pixelSize: 14
                    font.bold: true
                    font.letterSpacing: 1
                }
            }

            Text {
                id: headingLabel
                x: 48; y: kindBadge.y + kindBadge.height + 24
                width: root.leftPanelWidth - 96
                text: root.slideHeading
                color: "#f5f2e8"
                font.family: root.uiFontFamily
                font.pixelSize: 32
                font.bold: true
                lineHeight: 1.25
                wrapMode: Text.WordWrap
                opacity: root.slideOpacity
            }

            Column {
                x: 48; y: headingLabel.y + headingLabel.height + 32
                width: root.leftPanelWidth - 96
                spacing: 14
                opacity: root.slideOpacity
                // 「参考」のスライドは、右のカードが同じ内容を出す。左に本文の行(「⬜ 未引用 …」)をそのまま出すと
                // 重複して、絵文字が豆腐(□)になるため、出さない
                visible: root.slideReferenceItems.length === 0

                Row {
                    spacing: 10
                    visible: root.slideBullet1.length > 0
                    Rectangle { width: 8; height: 8; color: "#ff9d5c"; y: 8 }
                    Text {
                        width: leftPanel.width - 96 - 20
                        text: root.slideBullet1
                        color: "#c9c4b6"
                        font.family: root.uiFontFamily
                        font.pixelSize: 17
                        lineHeight: 1.2
                        wrapMode: Text.WordWrap
                    }
                }
                Row {
                    spacing: 10
                    visible: root.slideBullet2.length > 0
                    Rectangle { width: 8; height: 8; color: "#ff9d5c"; y: 8 }
                    Text {
                        width: leftPanel.width - 96 - 20
                        text: root.slideBullet2
                        color: "#c9c4b6"
                        font.family: root.uiFontFamily
                        font.pixelSize: 17
                        lineHeight: 1.2
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        // ===== Right visual panel =====
        Rectangle {
            id: rightPanel
            x: root.leftPanelWidth; y: 0
            width: stage.width - root.leftPanelWidth
            height: stage.height - root.footerHeight
            color: "#000000"
            clip: true

            // Fallback visual when a slide has neither a diagram, a code block,
            // nor a reference list: a soft abstract gradient rather than a
            // blank void.
            Rectangle {
                anchors.fill: parent
                visible: root.slideDiagramSource.length === 0 && root.slideCodeBlock.length === 0
                         && root.slideReferenceItems.length === 0
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#2a2f2a" }
                    GradientStop { position: 0.5; color: "#3d4238" }
                    GradientStop { position: 1.0; color: "#5c5340" }
                }
            }

            // "参考" slide visual: a source-card list (title + database + used/
            // unused badge), replacing what used to be the empty gradient
            // fallback -- a plain source list has no diagram/code to show, but
            // an empty panel read as unfinished/broken to the user.
            Column {
                id: referenceList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 32
                visible: root.slideReferenceItems.length > 0
                spacing: 14

                // 件数が多いときは、2〜3 列にして字を小さくし、画面の外へはみ出さないようにする
                readonly property int itemCount: root.slideReferenceItems.length
                readonly property int columnCount: itemCount <= 5 ? 1 : (itemCount <= 12 ? 2 : 3)
                readonly property int titleSize: itemCount <= 5 ? 19 : (itemCount <= 8 ? 17 : 14)
                readonly property int detailSize: itemCount <= 8 ? 14 : 13
                readonly property int cardPadding: itemCount <= 8 ? 24 : 16

                Text {
                    text: "参照した情報源"
                    color: "#c9c4b6"
                    font.family: root.uiFontFamily
                    font.pixelSize: 15
                    font.bold: true
                    font.letterSpacing: 1
                }

                Grid {
                    id: refGrid
                    columns: referenceList.columnCount
                    spacing: 10
                Repeater {
                    model: root.slideReferenceItems
                    delegate: Rectangle {
                        width: (referenceList.width - refGrid.spacing * (refGrid.columns - 1)) / refGrid.columns
                        height: refCardColumn.height + referenceList.cardPadding
                        radius: 8
                        color: "#161b17"
                        border.color: modelData.cited ? "#ff9d5c" : "#2b3226"
                        border.width: 1

                        Row {
                            x: 16; y: referenceList.cardPadding / 2
                            width: parent.width - 32
                            spacing: 14

                            Rectangle {
                                width: 34; height: 34
                                radius: 17
                                anchors.verticalCenter: parent.verticalCenter
                                color: modelData.cited ? "#ff9d5c" : "#2b3226"
                                border.color: "#ff9d5c"
                                border.width: modelData.cited ? 0 : 1

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.cited ? "✓" : "–"
                                    color: modelData.cited ? "#1a1408" : "#c9c4b6"
                                    font.pixelSize: 16
                                    font.bold: true
                                }
                            }

                            Column {
                                id: refCardColumn
                                width: parent.width - 34 - 14
                                spacing: 4
                                anchors.verticalCenter: parent.verticalCenter

                                Text {
                                    width: parent.width
                                    text: modelData.title
                                    color: "#f5f2e8"
                                    font.family: root.uiFontFamily
                                    font.pixelSize: referenceList.titleSize
                                    font.bold: true
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                }
                                Row {
                                    spacing: 8
                                    Text {
                                        text: modelData.db
                                        color: "#7d8378"
                                        font.family: root.monoFontFamily
                                        font.pixelSize: referenceList.detailSize
                                    }
                                    Text {
                                        text: modelData.cited ? "チュートリアルで活用" : "検索のみ・未使用"
                                        color: modelData.cited ? "#ff9d5c" : "#7d8378"
                                        font.family: root.uiFontFamily
                                        font.pixelSize: referenceList.detailSize
                                    }
                                }
                            }
                        }
                    }
                }
                }
            }

            Image {
                id: diagramImage
                anchors.fill: parent
                anchors.margins: 20
                visible: root.slideDiagramSource.length > 0
                source: root.slideDiagramSource
                fillMode: Image.PreserveAspectFit
                // 画面より大きい元画像(Houdini のキャプチャ)を縮めるとき、文字がつぶれにくいようにする
                smooth: true
                mipmap: true
                asynchronous: false
            }

            // Code-editor mockup, echoing the reference design's real
            // screen-capture chapters (Blender UI + Python script).
            Rectangle {
                id: codePanel
                anchors.fill: parent
                anchors.margins: 20
                visible: root.slideCodeBlock.length > 0
                color: "#161b22"
                border.color: "#30363d"
                border.width: 1
                radius: 6

                Rectangle {
                    id: codeTitleBar
                    x: 0; y: 0
                    width: parent.width; height: 34
                    color: "#21262d"
                    radius: 6

                    Row {
                        x: 14
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Rectangle { width: 10; height: 10; radius: 5; color: "#ff5f57" }
                        Rectangle { width: 10; height: 10; radius: 5; color: "#febc2e" }
                        Rectangle { width: 10; height: 10; radius: 5; color: "#28c840" }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: root.slideHeading
                        color: "#7d8590"
                        font.family: root.monoFontFamily
                        font.pixelSize: 13
                    }
                }

                Flickable {
                    x: 20; y: codeTitleBar.height + 16
                    width: parent.width - 40
                    height: parent.height - codeTitleBar.height - 32
                    clip: true
                    interactive: false
                    contentWidth: width
                    contentHeight: codeText.implicitHeight
                    contentY: Math.max(0, contentHeight - height) * root.slideProgress

                    Text {
                        id: codeText
                        width: parent.width
                        text: root.slideCodeBlock
                        color: "#adbac7"
                        font.family: root.monoFontFamily
                        font.pixelSize: 19
                        lineHeight: 1.45
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
        }

        // ===== Footer: chapter label + segmented timeline + metadata =====
        Item {
            id: footer
            x: 0; y: stage.height - root.footerHeight
            width: stage.width
            height: root.footerHeight

            Rectangle { x: 0; y: 0; width: parent.width; height: 1; color: "#2b3226" }

            Text {
                x: 48; y: 14
                text: "CHAPTER " + String(root.slideIndex + 1).padStart(2, "0") + " / " + root.slideKind
                color: "#f5f2e8"
                font.family: root.uiFontFamily
                font.pixelSize: 15
                font.bold: true
                opacity: root.slideOpacity
            }

            Item {
                id: timeline
                x: 48; y: 46
                width: parent.width - 96
                height: 3

                Rectangle { anchors.fill: parent; color: "#3a4038" }
                Rectangle {
                    width: parent.width * root.progress
                    height: parent.height
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#ff9d5c" }
                        GradientStop { position: 1.0; color: "#ff6a3d" }
                    }
                }
                Repeater {
                    model: root.slideBoundaries
                    Rectangle {
                        x: timeline.width * modelData - 1
                        y: -4
                        width: 2
                        height: timeline.height + 8
                        color: "#0c0f0d"
                    }
                }
            }

            Text {
                x: 48; y: 66
                text: root.metadataLine
                color: "#7d8378"
                font.family: root.monoFontFamily
                font.pixelSize: 14
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 48
                y: 66
                text: "AUTO-GENERATED"
                color: "#ff9d5c"
                font.family: root.uiFontFamily
                font.pixelSize: 14
                font.bold: true
                font.letterSpacing: 1
            }
        }
    }
}
