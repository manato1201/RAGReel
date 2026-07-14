import QtQuick

// Phase 1 PoC scene: static content only, no data-driven ShotList yet
// (see docs/architecture/video-factory-design.md §8, Phase 1 vs Phase 2 scope).
// `progress` (0.0-1.0) is set from C++ once per rendered frame so the output
// video is visibly not a single frame duplicated N times.
Rectangle {
    id: root
    width: 1280
    height: 720
    color: "#1b1f2a"

    property real progress: 0.0

    Rectangle {
        id: dot
        width: 64
        height: 64
        radius: 32
        color: "#ff7a45"
        y: (root.height - height) / 2
        x: (root.width - width) * root.progress
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 160
        text: "Video Factory — Phase 1 PoC"
        color: "white"
        font.pixelSize: 48
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 520
        text: "headless QQuickRenderControl -> FFmpeg mux"
        color: "#9aa4b2"
        font.pixelSize: 24
    }
}
