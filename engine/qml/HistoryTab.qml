import QtQuick

// Lists already-generated videos from output/manifest.json (VideoHistory)
// so a finished video is one click away, instead of manually navigating to
// output/index.html or the output/ folder in Explorer.
Column {
    id: root
    property bool running: false // interface parity with the other tabs, unused here
    spacing: 14

    Component.onCompleted: videoHistory.refresh()

    Row {
        width: parent.width
        spacing: 12

        Text {
            text: "履歴"
            color: "#f5f2e8"
            font.family: "Yu Gothic UI"
            font.pixelSize: 20
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            width: refreshLabel.width + 20
            height: 26
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: "#2b3226"
            border.color: "#ff9d5c"
            border.width: 1
            Text {
                id: refreshLabel
                anchors.centerIn: parent
                text: "更新"
                color: "#ff9d5c"
                font.family: "Yu Gothic UI"
                font.pixelSize: 11
            }
            MouseArea { anchors.fill: parent; onClicked: videoHistory.refresh() }
        }

        Rectangle {
            width: folderLabel.width + 20
            height: 26
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: "#2b3226"
            border.color: "#8fbf7a"
            border.width: 1
            Text {
                id: folderLabel
                anchors.centerIn: parent
                text: "出力フォルダを開く"
                color: "#8fbf7a"
                font.family: "Yu Gothic UI"
                font.pixelSize: 11
            }
            MouseArea { anchors.fill: parent; onClicked: videoHistory.openOutputFolder() }
        }

        Rectangle {
            width: dashLabel.width + 20
            height: 26
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: "#2b3226"
            border.color: "#8fbf7a"
            border.width: 1
            Text {
                id: dashLabel
                anchors.centerIn: parent
                text: "ダッシュボードを開く"
                color: "#8fbf7a"
                font.family: "Yu Gothic UI"
                font.pixelSize: 11
            }
            MouseArea { anchors.fill: parent; onClicked: videoHistory.openDashboard() }
        }
    }

    Text {
        visible: videoHistory.entries.length === 0
        text: "まだ生成された動画はありません。「Cloud RAGクエリ」または「Houdiniチュートリアル」タブから動画を生成してください。"
        color: "#c9c4b6"
        font.family: "Yu Gothic UI"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        width: Math.min(600, root.width)
    }

    ListView {
        visible: videoHistory.entries.length > 0
        width: Math.min(640, root.width)
        height: 300
        clip: true
        spacing: 6
        model: videoHistory.entries

        delegate: Rectangle {
            width: ListView.view.width
            height: 56
            radius: 4
            color: rowArea.containsMouse && modelData.hasVideo ? "#1a2018" : "#10140f"
            border.color: "#2b3226"
            border.width: 1

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                width: parent.width - 140

                Text {
                    text: modelData.title.length > 0 ? modelData.title : "(無題)"
                    color: "#f5f2e8"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    width: parent.width
                }
                Text {
                    text: modelData.createdAt + " ・ " + Math.round(modelData.durationSec) + "秒"
                    color: "#c9c4b6"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 11
                }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.hasVideo ? "▶ 再生" : "(動画ファイルなし)"
                color: modelData.hasVideo ? "#ff9d5c" : "#5a5a5a"
                font.family: "Yu Gothic UI"
                font.pixelSize: 12
            }

            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                enabled: modelData.hasVideo
                cursorShape: modelData.hasVideo ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: videoHistory.openVideo(modelData.id)
            }
        }
    }
}
