#include "native_dialogs.h"

#include <QDir>
#include <QFileDialog>

QString NativeDialogs::pickHoudiniMarkdownFile() {
    return QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Houdiniチュートリアルのmdファイルを選択"), QDir::homePath(),
        QStringLiteral("チュートリアルMarkdown (*.md)"));
}

QStringList NativeDialogs::pickHoudiniMarkdownFiles() {
    return QFileDialog::getOpenFileNames(
        nullptr, QStringLiteral("Houdiniチュートリアルのmdファイルを選択（複数可）"), QDir::homePath(),
        QStringLiteral("チュートリアルMarkdown (*.md)"));
}
