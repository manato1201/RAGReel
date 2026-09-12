import QtQuick

Column {
    id: root
    spacing: 16

    Text {
        text: "設定"
        color: "#f5f2e8"
        font.family: "Yu Gothic UI"
        font.pixelSize: 20
        font.bold: true
    }
    Text {
        text: "Cloud RAGへの接続情報です。ご自身のAPIキーを入力してください（一度入力すれば次回以降は自動的に使われます）。"
        color: "#c9c4b6"
        font.family: "Yu Gothic UI"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        width: Math.min(480, root.width)
    }

    LabeledField {
        width: 480
        label: "Cloud RAG URL"
        text: launcherSettings.apiUrl
        onEditingFinished: launcherSettings.apiUrl = text
    }
    LabeledField {
        width: 480
        label: "APIキー"
        text: launcherSettings.apiKey
        passwordMode: true
        onEditingFinished: launcherSettings.apiKey = text
    }

    Text {
        text: "※ 入力欄からフォーカスが外れると自動的に保存されます"
        color: "#8fbf7a"
        font.family: "Yu Gothic UI"
        font.pixelSize: 11
    }

    Text {
        text: "共有ギャラリー（省略可）"
        color: "#f5f2e8"
        font.family: "Yu Gothic UI"
        font.pixelSize: 16
        font.bold: true
    }
    Text {
        text: "設定すると、生成した動画が自動的にCloudflare上の共有ギャラリーにもアップロードされます（Houdiniのブラウザタブなどから閲覧可能）。空欄のままなら今まで通りローカルのみで動作します。"
        color: "#c9c4b6"
        font.family: "Yu Gothic UI"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        width: Math.min(480, root.width)
    }

    LabeledField {
        width: 480
        label: "ギャラリーURL"
        text: launcherSettings.galleryUploadUrl
        onEditingFinished: launcherSettings.galleryUploadUrl = text
    }
    LabeledField {
        width: 480
        label: "アップロードトークン"
        text: launcherSettings.galleryUploadToken
        passwordMode: true
        onEditingFinished: launcherSettings.galleryUploadToken = text
    }

    // 「Cloud RAGクエリ」タブまで移動しなくても、URL/APIキーを入力した直後に
    // 接続確認できるように -- namespaceLister.refresh()自体は既存(右上ランプ・
    // クエリタブのdbKey一覧取得と共用)、ここはその呼び出し口を増やすだけ。
    Row {
        spacing: 10

        Rectangle {
            width: testLabel.width + 24
            height: 32
            radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: namespaceLister.connectionState === "checking" ? "#3a3a3a" : "#2b3226"
            border.color: "#ff9d5c"
            border.width: 1

            Text {
                id: testLabel
                anchors.centerIn: parent
                text: namespaceLister.connectionState === "checking" ? "確認中..." : "接続テスト"
                color: "#ff9d5c"
                font.family: "Yu Gothic UI"
                font.pixelSize: 12
            }
            MouseArea {
                anchors.fill: parent
                enabled: namespaceLister.connectionState !== "checking"
                onClicked: namespaceLister.refresh()
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: {
                switch (namespaceLister.connectionState) {
                case "ok": return "✓ 接続OK（利用可能なデータベース " + namespaceLister.namespaces.length + " 件）";
                case "error": return "✗ " + namespaceLister.errorMessage;
                case "checking": return "";
                case "unconfigured": return "URL/APIキーを入力してから押してください";
                default: return "";
                }
            }
            color: namespaceLister.connectionState === "ok" ? "#8fbf7a"
                 : namespaceLister.connectionState === "error" ? "#ff6a6a"
                 : "#c9c4b6"
            font.family: "Yu Gothic UI"
            font.pixelSize: 12
            wrapMode: Text.Wrap
            width: Math.min(400, root.width - 200)
        }
    }
}
