import QtQuick

// Phase 2.5: digest/slide-deck video format (ref: YouTube tutorial
// title-card style -- big bold headline + short body per slide, not one
// long scrolling wall of text). Each Cloud RAG answer is split into slides
// by "## heading" in main_cloudrag.cpp (splitIntoSlides); this scene renders
// whichever slide is currently active.
//
// Per-frame properties set from C++ (main_cloudrag.cpp's render loop):
//   progress      - 0..1 across the whole video (drives the top progress bar)
//   slideIndex    - 0-based index of the currently active slide
//   slideCount    - total number of slides
//   slideHeading  - current slide's big headline text
//   slideBody     - current slide's body markdown (may still need scrolling
//                   if long -- see the Flickable below)
//   slideProgress - 0..1 within the *current* slide's own on-screen window;
//                   drives both the body's auto-scroll and the fade
//                   in/out transition at slide boundaries.
Rectangle {
    id: root
    width: 1280
    height: 720

    property real progress: 0.0
    property string topic: ""
    property string sourcesText: ""
    property int slideIndex: 0
    property int slideCount: 1
    property string slideHeading: ""
    property string slideBody: ""
    property real slideProgress: 0.0
    // file:// URL of a Mermaid diagram rendered to PNG by mmdc
    // (main_cloudrag.cpp's expandDiagramSlides); empty for text slides.
    property string slideDiagramSource: ""

    // "Yu Gothic UI" ships with Windows 10/11 Japanese locale; no FontLoader
    // needed since we're referencing an installed system font by name, not
    // loading a font file.
    readonly property string uiFontFamily: "Yu Gothic UI"

    // Fades the slide content in/out over the first/last 12% of its
    // on-screen window, giving a clean cut-to-black-ish transition between
    // slides without needing to cross-fade old/new content simultaneously.
    readonly property real slideOpacity: {
        const fadeWindow = 0.12;
        return Math.max(0, Math.min(1, Math.min(slideProgress / fadeWindow,
                                                 (1 - slideProgress) / fadeWindow)));
    }

    gradient: Gradient {
        GradientStop { position: 0.0; color: "#161a24" }
        GradientStop { position: 1.0; color: "#0f1218" }
    }

    // Slim top progress bar: overall video progress, independent of slides.
    Rectangle {
        id: progressTrack
        x: 0; y: 0
        width: root.width; height: 5
        color: "#232838"

        Rectangle {
            width: progressTrack.width * root.progress
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#ff9d5c" }
                GradientStop { position: 1.0; color: "#ff6a3d" }
            }
        }
    }

    Row {
        x: 64; y: 40
        spacing: 12

        Text {
            text: "HOUDINI21 · CLOUD RAG"
            color: "#ff8a5c"
            font.family: root.uiFontFamily
            font.pixelSize: 15
            font.letterSpacing: 2
            font.bold: true
        }

        Rectangle {
            width: slideCounterText.implicitWidth + 16
            height: slideCounterText.implicitHeight + 6
            radius: height / 2
            color: "#2b3244"
            anchors.verticalCenter: parent.verticalCenter

            Text {
                id: slideCounterText
                anchors.centerIn: parent
                text: (root.slideIndex + 1) + " / " + root.slideCount
                color: "#c7cede"
                font.family: root.uiFontFamily
                font.pixelSize: 13
                font.bold: true
            }
        }
    }

    Text {
        id: topicLabel
        x: 64
        y: 72
        width: root.width - 128
        text: root.topic
        color: "#7d8798"
        font.family: root.uiFontFamily
        font.pixelSize: 16
        elide: Text.ElideRight
        maximumLineCount: 1
    }

    // The slide itself: big bold headline (title-card style) + a card panel
    // holding the shorter body text, which still gently auto-scrolls if a
    // section runs long so nothing gets clipped.
    Item {
        id: slideContent
        x: 0; y: topicLabel.y + topicLabel.height + 20
        width: root.width
        height: root.height - y
        opacity: root.slideOpacity

        Text {
            id: headingLabel
            x: 64
            width: root.width - 128
            text: root.slideHeading
            color: "#f5f7fa"
            font.family: root.uiFontFamily
            font.pixelSize: 46
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Rectangle {
            id: card
            x: 56
            y: headingLabel.y + headingLabel.height + 24
            width: root.width - 112
            height: slideContent.height - y - 96
            radius: 18
            color: "#1b202c"
            border.color: "#2b3244"
            border.width: 1
            visible: bodyLabel.text.length > 0 || root.slideDiagramSource.length > 0

            Image {
                id: diagramImage
                anchors.fill: parent
                anchors.margins: 24
                visible: root.slideDiagramSource.length > 0
                source: root.slideDiagramSource
                fillMode: Image.PreserveAspectFit
                asynchronous: false
            }

            Flickable {
                id: bodyFlick
                x: 32; y: 28
                width: card.width - 64
                height: card.height - 56
                clip: true
                interactive: false
                contentWidth: width
                contentHeight: bodyLabel.implicitHeight
                contentY: Math.max(0, contentHeight - height) * root.slideProgress
                visible: root.slideDiagramSource.length === 0

                Text {
                    id: bodyLabel
                    width: bodyFlick.width
                    text: root.slideBody
                    textFormat: Text.MarkdownText
                    color: "#d7dce3"
                    font.family: root.uiFontFamily
                    font.pixelSize: 22
                    lineHeight: 1.35
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    Rectangle {
        x: 64
        y: root.height - 76
        width: root.width - 128
        height: 1
        color: "#2b3244"
    }

    Text {
        x: 64
        y: root.height - 58
        width: root.width - 128
        text: root.sourcesText
        color: "#7d8798"
        font.family: root.uiFontFamily
        font.pixelSize: 14
        wrapMode: Text.WordWrap
    }
}
