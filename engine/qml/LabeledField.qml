import QtQuick

// Small reusable "label above a boxed TextInput" row, shared by SettingsTab
// and CloudRagTab. Not using QtQuick.Controls' TextField deliberately (see
// Launcher.qml's header comment) -- this is the whole styling surface those
// two tabs need.
Column {
    id: root
    property string label: ""
    property alias text: input.text
    property bool passwordMode: false
    signal editingFinished()

    spacing: 4

    Text {
        text: root.label
        color: "#c9c4b6"
        font.family: "Yu Gothic UI"
        font.pixelSize: 12
    }
    Rectangle {
        width: root.width
        height: 36
        color: "#10140f"
        border.color: input.activeFocus ? "#ff9d5c" : "#2b3226"
        border.width: 1
        radius: 4

        TextInput {
            id: input
            anchors.fill: parent
            anchors.margins: 8
            color: "#f5f2e8"
            font.family: "Yu Gothic UI"
            font.pixelSize: 13
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            echoMode: root.passwordMode ? TextInput.Password : TextInput.Normal
            selectByMouse: true
            onEditingFinished: root.editingFinished()
        }
    }
}
