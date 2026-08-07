#include "native_dialogs.h"

#include <QDir>
#include <QFileDialog>

QString NativeDialogs::pickHoudiniMarkdownFile() {
    return QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Houdiniチュートリアルのmdファイルを選択"), QDir::homePath(),
        QStringLiteral("チュートリアルMarkdown (*.md)"));
}
