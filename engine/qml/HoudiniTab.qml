import QtQuick

Column {
    id: root
    property bool running: false
    property string mdPath: ""
    spacing: 16

    Text {
        text: "Houdiniチュートリアルから生成"
        color: "#f5f2e8"
        font.family: "Yu Gothic UI"
        font.pixelSize: 20
        font.bold: true
    }
    Text {
        text: "Houdiniで生成したチュートリアルの .md ファイルを選んでください。"
              + "同じ場所にある同名の .json / _screenshots.json は自動的に使われます。"
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
                text: "ファイルを選択..."
                color: "#f5f2e8"
                font.family: "Yu Gothic UI"
                font.pixelSize: 13
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    const picked = nativeDialogs.pickHoudiniMarkdownFile()
                    if (picked.length > 0) {
                        root.mdPath = picked
                    }
                }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.mdPath.length > 0 ? root.mdPath : "ファイル未選択"
            color: root.mdPath.length > 0 ? "#f5f2e8" : "#c9c4b6"
            font.family: "Yu Gothic UI"
            font.pixelSize: 12
            elide: Text.ElideMiddle
            width: 400
        }
    }

    Rectangle {
        width: 160
        height: 44
        radius: 4
        color: (root.running || root.mdPath.length === 0) ? "#3a3a3a" : "#ff9d5c"

        Text {
            anchors.centerIn: parent
            text: root.running ? "実行中..." : "動画を生成"
            color: "#1a1408"
            font.family: "Yu Gothic UI"
            font.pixelSize: 14
            font.bold: true
        }
        MouseArea {
            anchors.fill: parent
            enabled: !root.running && root.mdPath.length > 0
            onClicked: processRunner.runHoudiniTutorial(root.mdPath)
        }
    }
}
