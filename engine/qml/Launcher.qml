import QtQuick
import QtQuick.Window

// GUI front end for video_factory_cloudrag_poc.exe (see main_launcher.cpp /
// ProcessRunner). Hand-rolled controls (Rectangle/TextInput/MouseArea)
// instead of QtQuick.Controls, deliberately -- this project loads QML from
// absolute file paths rather than qt_add_qml_module, and pulling in
// QtQuick.Controls would mean bundling+deploying its style plugin alongside
// an app whose whole point is being trivial to hand a teammate. Palette
// matches CloudRagScene.qml's "KISARAGI" branding.
Window {
    id: root
    width: 900
    height: 640
    minimumWidth: 700
    minimumHeight: 480
    visible: true
    title: "RAGReel"
    color: "#0c0f0d"

    readonly property string uiFontFamily: "Yu Gothic UI"
    readonly property string monoFontFamily: "Consolas"
    readonly property color bgColor: "#0c0f0d"
    readonly property color panelColor: "#10140f"
    readonly property color borderColor: "#2b3226"
    readonly property color textColor: "#f5f2e8"
    readonly property color subTextColor: "#c9c4b6"
    readonly property color accentColor: "#ff9d5c"
    readonly property color accentDarkColor: "#ff8a45"

    property int currentTab: 0
    property bool running: processRunner.running

    // ── ログパネル用の行モデル ────────────────────────────────────────
    ListModel { id: logModel }

    function appendLog(line) {
        logModel.append({ text: line });
        logListView.positionViewAtEnd();
    }

    Connections {
        target: processRunner
        function onOutputLine(line) { appendLog(line); }
        function onFinished(exitCode, message) {
            appendLog("── " + message + " ──");
        }
    }

    // ── レイアウト ────────────────────────────────────────────────────
    Row {
        anchors.fill: parent

        // サイドバー（タブ切り替え）
        Rectangle {
            width: 200
            height: parent.height
            color: root.panelColor
            border.color: root.borderColor
            border.width: 1

            Column {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 12
                spacing: 6

                Text {
                    text: "RAGREEL"
                    color: root.accentColor
                    font.family: root.uiFontFamily
                    font.pixelSize: 14
                    font.bold: true
                    bottomPadding: 12
                }

                Repeater {
                    model: ["はじめに", "設定", "Cloud RAGクエリ", "Houdiniチュートリアル"]
                    delegate: Rectangle {
                        width: parent.width
                        height: 44
                        radius: 4
                        color: root.currentTab === index ? root.accentDarkColor : "transparent"
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            color: root.currentTab === index ? "#1a1408" : root.textColor
                            font.family: root.uiFontFamily
                            font.pixelSize: 14
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.currentTab = index
                        }
                    }
                }
            }
        }

        // メインコンテンツ
        Column {
            width: parent.width - 200
            height: parent.height

            Item {
                width: parent.width
                height: parent.height * 0.55

                WelcomeTab {
                    anchors.fill: parent
                    anchors.margins: 20
                    visible: root.currentTab === 0
                }
                SettingsTab {
                    anchors.fill: parent
                    anchors.margins: 20
                    visible: root.currentTab === 1
                }
                CloudRagTab {
                    anchors.fill: parent
                    anchors.margins: 20
                    visible: root.currentTab === 2
                    running: root.running
                }
                HoudiniTab {
                    anchors.fill: parent
                    anchors.margins: 20
                    visible: root.currentTab === 3
                    running: root.running
                }
            }

            // ── ログパネル ──────────────────────────────────────────
            Rectangle {
                width: parent.width
                height: parent.height * 0.45
                color: "#000000"
                border.color: root.borderColor
                border.width: 1

                Row {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 8
                    spacing: 12

                    Text {
                        text: root.running ? "● 実行中..." : "実行ログ"
                        color: root.running ? root.accentColor : root.subTextColor
                        font.family: root.uiFontFamily
                        font.pixelSize: 12
                    }
                    Text {
                        visible: root.running
                        text: "[キャンセル]"
                        color: "#ff6a6a"
                        font.family: root.uiFontFamily
                        font.pixelSize: 12
                        MouseArea { anchors.fill: parent; onClicked: processRunner.cancel() }
                    }
                }

                ListView {
                    id: logListView
                    anchors.top: parent.top
                    anchors.topMargin: 30
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    clip: true
                    model: logModel
                    delegate: Text {
                        width: logListView.width
                        text: model.text
                        color: root.subTextColor
                        font.family: root.monoFontFamily
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    // ── Cloud RAG接続ランプ（右上に常時表示） ─────────────────────────
    // namespaceLister.refresh()（Cloud RAGクエリタブの「更新」ボタン、および
    // 起動時の自動取得）の結果をそのまま流用した接続確認インジケータ。
    Row {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 14
        spacing: 6
        z: 10

        Rectangle {
            id: statusLamp
            width: 10
            height: 10
            radius: 5
            anchors.verticalCenter: parent.verticalCenter
            color: {
                switch (namespaceLister.connectionState) {
                case "ok": return "#5fc76a";
                case "error": return "#ff5f57";
                case "checking": return "#ffcf5c";
                case "unconfigured": return "#5a5a5a";
                default: return "#5a5a5a";
                }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: {
                switch (namespaceLister.connectionState) {
                case "ok": return "Cloud RAG接続OK";
                case "error": return "接続エラー";
                case "checking": return "確認中...";
                case "unconfigured": return "APIキー未設定";
                default: return "未確認";
                }
            }
            color: root.subTextColor
            font.family: root.uiFontFamily
            font.pixelSize: 11
        }
    }
}
