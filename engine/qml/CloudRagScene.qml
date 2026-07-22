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
//   slideProgress  - 0..1 within the current slide's own on-screen window;
//                    drives the fade in/out transition at slide boundaries.
Rectangle {
    id: root
    width: 1280
    height: 720
    color: "#0c0f0d"

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
    property real slideProgress: 0.0

    readonly property string uiFontFamily: "Yu Gothic UI"
    readonly property string monoFontFamily: "Consolas"

    // "解説" (explainer) / "図解" (diagram) / "コード例" (code example) --
    // a lightweight per-slide category, standing in for the reference
    // design's per-chapter category badge (e.g. "FUNCTIONAL DESIGN").
    readonly property string slideKind: slideCodeBlock.length > 0
        ? "コード例"
        : (slideDiagramSource.length > 0 ? "図解" : "解説")

    // Fades slide content in/out over the first/last 12% of its on-screen
    // window -- a clean transition between chapters without needing to
    // cross-fade old/new content simultaneously.
    readonly property real slideOpacity: {
        const fadeWindow = 0.12;
        return Math.max(0, Math.min(1, Math.min(slideProgress / fadeWindow,
                                                 (1 - slideProgress) / fadeWindow)));
    }

    readonly property int footerHeight: 108
    readonly property int leftPanelWidth: 420

    // ===== Left info panel =====
    Rectangle {
        id: leftPanel
        x: 0; y: 0
        width: root.leftPanelWidth
        height: root.height - root.footerHeight
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
            font.pixelSize: 12
            font.bold: true
            font.letterSpacing: 2
        }

        Text {
            id: chapterCounter
            x: 48; y: 176
            text: (root.slideIndex + 1) + " / " + root.slideCount
            color: "#ff9d5c"
            font.family: root.monoFontFamily
            font.pixelSize: 14
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
                font.pixelSize: 13
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

            Row {
                spacing: 10
                visible: root.slideBullet1.length > 0
                Rectangle { width: 7; height: 7; color: "#ff9d5c"; y: 6 }
                Text {
                    width: leftPanel.width - 96 - 20
                    text: root.slideBullet1
                    color: "#c9c4b6"
                    font.family: root.uiFontFamily
                    font.pixelSize: 14
                    wrapMode: Text.WordWrap
                }
            }
            Row {
                spacing: 10
                visible: root.slideBullet2.length > 0
                Rectangle { width: 7; height: 7; color: "#ff9d5c"; y: 6 }
                Text {
                    width: leftPanel.width - 96 - 20
                    text: root.slideBullet2
                    color: "#c9c4b6"
                    font.family: root.uiFontFamily
                    font.pixelSize: 14
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // ===== Right visual panel =====
    Rectangle {
        id: rightPanel
        x: root.leftPanelWidth; y: 0
        width: root.width - root.leftPanelWidth
        height: root.height - root.footerHeight
        color: "#000000"
        clip: true

        // Fallback visual when a slide has neither a diagram nor a code
        // block: a soft abstract gradient rather than a blank void.
        Rectangle {
            anchors.fill: parent
            visible: root.slideDiagramSource.length === 0 && root.slideCodeBlock.length === 0
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#2a2f2a" }
                GradientStop { position: 0.5; color: "#3d4238" }
                GradientStop { position: 1.0; color: "#5c5340" }
            }
        }

        Image {
            id: diagramImage
            anchors.fill: parent
            anchors.margins: 56
            visible: root.slideDiagramSource.length > 0
            source: root.slideDiagramSource
            fillMode: Image.PreserveAspectFit
            asynchronous: false
        }

        // Code-editor mockup, echoing the reference design's real
        // screen-capture chapters (Blender UI + Python script).
        Rectangle {
            id: codePanel
            anchors.fill: parent
            anchors.margins: 48
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
                    font.pixelSize: 12
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
                    font.pixelSize: 16
                    lineHeight: 1.5
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }

    // ===== Footer: chapter label + segmented timeline + metadata =====
    Item {
        id: footer
        x: 0; y: root.height - root.footerHeight
        width: root.width
        height: root.footerHeight

        Rectangle { x: 0; y: 0; width: parent.width; height: 1; color: "#2b3226" }

        Text {
            x: 48; y: 20
            text: "CHAPTER " + String(root.slideIndex + 1).padStart(2, "0") + " / " + root.slideKind
            color: "#f5f2e8"
            font.family: root.uiFontFamily
            font.pixelSize: 13
            font.bold: true
            opacity: root.slideOpacity
        }

        Item {
            id: timeline
            x: 48; y: 52
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
            x: 48; y: 80
            text: root.metadataLine
            color: "#7d8378"
            font.family: root.monoFontFamily
            font.pixelSize: 12
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 48
            y: 80
            text: "AUTO-GENERATED"
            color: "#ff9d5c"
            font.family: root.uiFontFamily
            font.pixelSize: 12
            font.bold: true
            font.letterSpacing: 1
        }
    }
}
