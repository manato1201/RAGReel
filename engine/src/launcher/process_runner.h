#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class LauncherSettings;

// Wraps video_factory_cloudrag_poc.exe in a QProcess so the GUI never shells
// out raw CLI flags for the user to get wrong. The exe itself is completely
// unmodified -- this only builds the same argv/environment a command-line
// user would have typed, from GUI field values instead.
class ProcessRunner : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

public:
    explicit ProcessRunner(LauncherSettings* settings, QObject* parent = nullptr);

    bool isRunning() const;

    // topic/dbKey become video_factory_cloudrag_poc's two positional args.
    Q_INVOKABLE void runCloudRagQuery(const QString& topic, const QString& dbKey);

    // mdPath is the Houdini tutorial markdown file the user picked; the
    // matching .json and _screenshots.json are located by the same naming
    // convention video_factory_bridge.py writes them with (same basename,
    // sibling directory) -- the screenshots manifest is optional and simply
    // omitted from argv if it isn't found next to the other two.
    Q_INVOKABLE void runHoudiniTutorial(const QString& mdPath);

    Q_INVOKABLE void cancel();

signals:
    void runningChanged();
    void outputLine(const QString& line);
    void finished(int exitCode, const QString& message);

private:
    void startProcess(const QStringList& args);
    QString exePath() const;

    LauncherSettings* settings_;
    QProcess process_;
    QString pendingLineBuffer_;
};
