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
}
