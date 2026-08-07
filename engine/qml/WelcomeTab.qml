import QtQuick

// 初めてRAGReelを開いたユーザー向けのガイド。設定/Cloud RAGクエリ/Houdini
// チュートリアルの各タブは操作画面そのものであり、何をどの順番で押せば
// いいかの説明が無かった -- このタブはその説明だけを担う、操作を伴わない
// 静的なヘルプ画面。
Flickable {
    id: root
    property bool running: false // 他タブと同じ interface を持たせるためだけの未使用プロパティ
    contentWidth: width
    contentHeight: content.height
    clip: true

    Column {
        id: content
        width: root.width
        spacing: 18

        Text {
            text: "はじめに"
            color: "#f5f2e8"
            font.family: "Yu Gothic UI"
            font.pixelSize: 20
            font.bold: true
        }
        Text {
            text: "RAGReelは、Cloud RAGへの質問やHoudiniで生成したチュートリアルから、"
                  + "自動でナレーション付きの解説動画を作るツールです。左のメニューから使いたい機能を選んでください。"
            color: "#c9c4b6"
            font.family: "Yu Gothic UI"
            font.pixelSize: 12
            wrapMode: Text.Wrap
            width: Math.min(600, root.width)
        }

        Column {
            spacing: 10
            width: parent.width

            Text {
                text: "使い方（3ステップ）"
                color: "#ff9d5c"
                font.family: "Yu Gothic UI"
                font.pixelSize: 14
                font.bold: true
            }

            Repeater {
                model: [
                    {
                        title: "① 設定タブでAPIキーを入力",
                        body: "「設定」タブを開き、Cloud RAG URLとAPIキーを入力してください。"
                              + "一度入力すれば次回以降は自動的に使われます。右上のランプが緑色（Cloud RAG接続OK）になれば準備完了です。"
                    },
                    {
                        title: "② 生成方法を選ぶ",
                        body: "質問文から動画を作りたい場合は「Cloud RAGクエリ」タブへ。"
                              + "Houdiniで生成済みのチュートリアル(.md)から動画を作りたい場合は「Houdiniチュートリアル」タブへ進んでください。"
                    },
                    {
                        title: "③ 内容を入力して「動画を生成」を押す",
                        body: "質問文の入力、またはファイルの選択をしたら「動画を生成」ボタンを押します。"
                              + "下のログパネルに進捗が表示され、完了すると同じフォルダのoutputにMP4と、"
                              + "そこに書き出されるダッシュボード(index.html)から動画を確認できます。"
                    }
                ]
                delegate: Row {
                    spacing: 12
                    width: parent.width

                    Rectangle {
                        width: 26; height: 26
                        radius: 13
                        color: "#2b3226"
                        border.color: "#ff9d5c"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: (index + 1).toString()
                            color: "#ff9d5c"
                            font.family: "Yu Gothic UI"
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }
                    Column {
                        width: Math.min(560, root.width - 60)
                        spacing: 2
                        Text {
                            text: modelData.title
                            color: "#f5f2e8"
                            font.family: "Yu Gothic UI"
                            font.pixelSize: 13
                            font.bold: true
                        }
                        Text {
                            text: modelData.body
                            color: "#c9c4b6"
                            font.family: "Yu Gothic UI"
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            width: parent.width
                        }
                    }
                }
            }
        }

        Rectangle {
            width: Math.min(600, root.width)
            height: helpColumn.height + 24
            radius: 6
            color: "#10140f"
            border.color: "#2b3226"
            border.width: 1

            Column {
                id: helpColumn
                x: 16; y: 12
                width: parent.width - 32
                spacing: 6

                Text {
                    text: "困ったときは"
                    color: "#8fbf7a"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 13
                    font.bold: true
                }
                Text {
                    text: "・右上のランプが赤（接続エラー）/グレー（APIキー未設定）の場合は「設定」タブのURL・APIキーを確認してください。"
                    color: "#c9c4b6"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    width: parent.width
                }
                Text {
                    text: "・動画生成には数分かかることがあります（ナレーション音声合成・図の生成・レンダリングを行うため）。"
                    color: "#c9c4b6"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    width: parent.width
                }
                Text {
                    text: "・実行中に止めたい場合は、下のログパネル上部の「[キャンセル]」を押してください。"
                    color: "#c9c4b6"
                    font.family: "Yu Gothic UI"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    width: parent.width
                }
            }
        }
    }
}
