import QtQuick

Column {
    id: root
    property bool running: false
    spacing: 16

    Component.onCompleted: namespaceLister.refresh()

    Text {
        text: "Cloud RAGクエリから生成"
        color: "#f5f2e8"
        font.family: "Yu Gothic UI"
        font.pixelSize: 20
        font.bold: true
    }

    LabeledField {
        id: dbKeyField
        width: 260
        label: "データベース (dbKey)"
        text: launcherSettings.lastDbKey
    }

    // APIキーに紐づくdbKey一覧 -- 実際にRAG検索は行わず、権限チェックだけを
    // 突いて取得する無料の呼び出し（CloudRagClient::listAllowedNamespaces
    // 参照）。クリックで上のdbKeyフィールドに反映されるだけの「候補」なので、
    // 一覧に無いdbKeyを手で入力することも引き続き可能。
    Column {
        spacing: 6
        width: parent.width

        Row {
            spacing: 10
            Text {
                text: "利用可能なデータベース (このAPIキーで):"
                color: "#c9c4b6"
                font.family: "Yu Gothic UI"
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                width: refreshLabel.width + 20
                height: 24
                radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: namespaceLister.connectionState === "checking" ? "#3a3a3a" : "#2b3226"
                border.color: "#ff9d5c"
                border.width: 1

                Text {
                    id: refreshLabel
                    anchors.centerIn: parent
                    text: namespaceLister.connectionState === "checking"
                          ? "確認中..."
                          : (namespaceLister.namespaces.length > 0 ? "更新" : "取得")
                    color: "#ff9d5c"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: namespaceLister.connectionState !== "checking"
                    onClicked: namespaceLister.refresh()
                }
            }
        }

        Text {
            visible: namespaceLister.errorMessage.length > 0
            text: namespaceLister.errorMessage
            color: "#ff6a6a"
            font.family: "Yu Gothic UI"
            font.pixelSize: 11
            wrapMode: Text.Wrap
            width: Math.min(500, root.width)
        }

        Flow {
            width: Math.min(500, root.width)
            spacing: 6

            Repeater {
                model: namespaceLister.namespaces
                delegate: Rectangle {
                    width: chipText.width + 20
                    height: 28
                    radius: 14
                    color: dbKeyField.text === modelData ? "#ff9d5c" : "#2b3226"

                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData
                        color: dbKeyField.text === modelData ? "#1a1408" : "#f5f2e8"
                        font.family: "Yu Gothic UI"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: dbKeyField.text = modelData
                    }
                }
            }
        }
    }

    Column {
        spacing: 4
        width: parent.width

        Text {
            text: "質問内容"
            color: "#c9c4b6"
            font.family: "Yu Gothic UI"
            font.pixelSize: 12
        }
        Rectangle {
            width: Math.min(600, root.width)
            height: 130
            color: "#10140f"
            border.color: topicInput.activeFocus ? "#ff9d5c" : "#2b3226"
            border.width: 1
            radius: 4

            TextEdit {
                id: topicInput
                anchors.fill: parent
                anchors.margins: 8
                color: "#f5f2e8"
                font.family: "Yu Gothic UI"
                font.pixelSize: 13
                wrapMode: TextEdit.Wrap
                clip: true
                selectByMouse: true
            }
        }
    }

    Rectangle {
        width: 160
        height: 44
        radius: 4
        color: (root.running || topicInput.text.trim().length === 0) ? "#3a3a3a" : "#ff9d5c"

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
            enabled: !root.running && topicInput.text.trim().length > 0
            onClicked: {
                launcherSettings.lastDbKey = dbKeyField.text
                processRunner.runCloudRagQuery(topicInput.text, dbKeyField.text)
            }
        }
    }
}
