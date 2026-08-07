// RAGReel -- a GUI front end for video_factory_cloudrag_poc.exe aimed at
// non-engineer teammates: no CLI flags, no CLOUD_RAG_URL/CLOUD_RAG_API_KEY
// environment variables to set up by hand. The generation exe itself is
// untouched; this only builds the same argv/environment a command-line user
// would have, from GUI fields (see ProcessRunner).
//
// Uses QApplication (not QGuiApplication) solely so QFileDialog (Qt6::Widgets)
// works from NativeDialogs -- the actual UI is QtQuick/QML throughout.

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QUrl>

#include <cstdio>

#include "launcher/launcher_settings.h"
#include "launcher/namespace_lister.h"
#include "launcher/native_dialogs.h"
#include "launcher/process_runner.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("RAGReel"));
    QApplication::setApplicationName(QStringLiteral("RAGReel"));

    LauncherSettings settings;
    ProcessRunner processRunner(&settings);
    NativeDialogs nativeDialogs;
    NamespaceLister namespaceLister(&settings);

    QQmlApplicationEngine engine;
    // Launcher.qml's QtQuick/QtQuick.Window imports resolve against this
    // directory (see the matching CMakeLists.txt post-build copy of Qt6's
    // qml/QtQuick tree) instead of the Qt SDK's own absolute install path,
    // which won't exist on a teammate's machine this gets copied to.
    engine.addImportPath(QApplication::applicationDirPath() + QStringLiteral("/qml"));
    QQmlContext* rootContext = engine.rootContext();
    rootContext->setContextProperty(QStringLiteral("launcherSettings"), &settings);
    rootContext->setContextProperty(QStringLiteral("processRunner"), &processRunner);
    rootContext->setContextProperty(QStringLiteral("nativeDialogs"), &nativeDialogs);
    rootContext->setContextProperty(QStringLiteral("namespaceLister"), &namespaceLister);

    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                      [](const QList<QQmlError>& warnings) {
                          for (const QQmlError& e : warnings) {
                              std::fprintf(stderr, "QML: %s\n", e.toString().toUtf8().constData());
                          }
                      });

    // Launcher.qml (and its sibling SettingsTab/CloudRagTab/HoudiniTab/
    // LabeledField components, resolved implicitly by directory adjacency)
    // is copied next to the exe by CMakeLists.txt's post-build step -- a
    // runtime-relative path instead of the compile-time absolute
    // source-tree path this used to be, so the exe actually works once
    // copied/installed anywhere else.
    const QString launcherQmlPath =
        QApplication::applicationDirPath() + QStringLiteral("/qml/Launcher.qml");
    engine.load(QUrl::fromLocalFile(launcherQmlPath));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "Failed to load QML: %s\n", launcherQmlPath.toUtf8().constData());
        return 1;
    }

    return QApplication::exec();
}
