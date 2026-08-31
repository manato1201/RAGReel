import QtQuick

// Batch-processes several Houdini-tutorial .md files without an external
// script: add files, press start, they run one at a time (TutorialQueue
// auto-advances via ProcessRunner's existing one-job-at-a-time model).
Column {
    id: root
    property bool running: false
    spacing: 14

    Text {
        text: "Houdiniチュートリアル（キュー処理）"
        color: "#f5f2e8"
        font.family: "Yu Gothic UI"
        font.pixelSize: 20
        font.bold: true
    }
    Text {
        text: "複数のチュートリアル.mdファイルをまとめて追加し、順番に自動で動画生成します。"
              + "1件ずつ処理されるため、実行中に他のタブから生成を開始することはできません。"
        color: "#c9c4b6"
        font.family: "Yu Gothic UI"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        width: Math.min(600, root.width)
    }

    Row {
        spacing: 12

        Rectangle {
            width: 150
            height: 40
            radius: 4
            color: "#2b3226"
            Text {
                anchors.centerIn: parent
                text: "ファイルを追加..."
                color: "#f5f2e8"
                font.family: "Yu Gothic UI"
                font.pixelSize: 13
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    const picked = nativeDialogs.pickHoudiniMarkdownFiles()
                    if (picked.length > 0) {
                        tutorialQueue.addFiles(picked)
                    }
                }
            }
        }

        Rectangle {
            width: 130
            height: 40
            radius: 4
            property bool canStart: !tutorialQueue.active && !root.running
            color: canStart ? "#ff9d5c" : "#3a3a3a"
            Text {
                anchors.centerIn: parent
                text: tutorialQueue.active ? "処理中..." : "キュー開始"
                color: "#1a1408"
                font.family: "Yu Gothic UI"
                font.pixelSize: 13
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                enabled: parent.canStart
                onClicked: tutorialQueue.start()
            }
        }

        Rectangle {
            visible: tutorialQueue.active
            width: 100
            height: 40
            radius: 4
            color: "#2b3226"
            border.color: "#ff6a6a"
            border.width: 1
            Text {
                anchors.centerIn: parent
                text: "キュー停止"
                color: "#ff6a6a"
                font.family: "Yu Gothic UI"
                font.pixelSize: 12
            }
            MouseArea { anchors.fill: parent; onClicked: tutorialQueue.stop() }
        }

        Rectangle {
            width: 110
            height: 40
            radius: 4
            color: "#2b3226"
            Text {
                anchors.centerIn: parent
                text: "完了分を削除"
                color: "#c9c4b6"
                font.family: "Yu Gothic UI"
                font.pixelSize: 12
            }
            MouseArea { anchors.fill: parent; onClicked: tutorialQueue.clearFinished() }
        }
    }

    Text {
        visible: tutorialQueue.items.length === 0
        text: "キューは空です。「ファイルを追加...」から.mdファイルを選んでください（複数選択可）。"
        color: "#c9c4b6"
        font.family: "Yu Gothic UI"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        width: Math.min(600, root.width)
    }

    ListView {
        visible: tutorialQueue.items.length > 0
        width: Math.min(640, root.width)
        height: 220
        clip: true
        spacing: 4
        model: tutorialQueue.items

        delegate: Rectangle {
            width: ListView.view.width
            height: 40
            radius: 4
            color: "#10140f"
            border.color: modelData.status === "running" ? "#ff9d5c" : "#2b3226"
            border.width: 1

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 140
                text: modelData.name
                color: "#f5f2e8"
                font.family: "Yu Gothic UI"
                font.pixelSize: 12
                elide: Text.ElideMiddle
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 40
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    switch (modelData.status) {
                    case "pending": return "待機中";
                    case "running": return "● 実行中";
                    case "done": return "✓ 完了";
                    case "error": return "✗ 失敗";
                    default: return modelData.status;
                    }
                }
                color: modelData.status === "running" ? "#ff9d5c"
                     : modelData.status === "done" ? "#8fbf7a"
                     : modelData.status === "error" ? "#ff6a6a"
                     : "#c9c4b6"
                font.family: "Yu Gothic UI"
                font.pixelSize: 11
            }

            Text {
                visible: modelData.status !== "running"
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "×"
                color: "#8a8a8a"
                font.pixelSize: 14
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: tutorialQueue.removeAt(index)
                }
            }
        }
    }
}
