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
    // 0.0-1.0, driven by parsing "Rendered frame N / M" lines from the
    // subprocess's own log output (see the readyReadStandardOutput handler)
    // -- Render is by far the dominant phase of a job, so frame-count
    // progress is a reasonable stand-in for overall job progress.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)

public:
    explicit ProcessRunner(LauncherSettings* settings, QObject* parent = nullptr);
    ~ProcessRunner() override;

    bool isRunning() const;
    double progress() const { return progress_; }

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
    void progressChanged();
    void outputLine(const QString& line);
    void finished(int exitCode, const QString& message);

private:
    void startProcess(const QStringList& args);
    QString exePath() const;

    LauncherSettings* settings_;
    QProcess process_;
    QString pendingLineBuffer_;
    bool cancelRequested_ = false;
    double progress_ = 0.0;
    // Native Job Object handle (Windows HANDLE, kept as void* so this header
    // doesn't have to pull in <windows.h> and its macro pollution). All
    // descendants of the subprocess we launch (ffmpeg/mermaid-cli included)
    // get assigned into this job, so cancel()/app-exit can tear down the
    // whole tree instead of just the direct child -- see process_runner.cpp.
    void* jobHandle_ = nullptr;
};
