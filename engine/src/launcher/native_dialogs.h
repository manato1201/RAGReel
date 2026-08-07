#pragma once

#include <QObject>
#include <QString>

// Thin wrapper around QFileDialog (Qt6::Widgets) so Launcher.qml can open a
// native "pick a file" dialog without the project taking on QtQuick.Dialogs
// (which would need its own plugin bundled/deployed). QFileDialog is already
// available for free -- Qt6::Widgets is present in the vcpkg install this
// project already uses (qtdeclarative pulls it in), just not previously
// linked by any target.
class NativeDialogs : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Returns an empty string if the user cancelled.
    Q_INVOKABLE QString pickHoudiniMarkdownFile();
};
