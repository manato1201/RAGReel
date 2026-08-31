#include "process_runner.h"
#include "launcher_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
// Matches main_cloudrag.cpp's own per-frame render-loop log line ("Rendered
// frame 120 / 3367"), the only per-iteration progress signal the subprocess
// currently emits.
const QRegularExpression& renderProgressRegex() {
    static const QRegularExpression re(QStringLiteral("^Rendered frame (\\d+) / (\\d+)$"));
    return re;
}
} // namespace

ProcessRunner::ProcessRunner(LauncherSettings* settings, QObject* parent)
    : QObject(parent), settings_(settings) {
#ifdef Q_OS_WIN
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE guarantees every process ever
    // assigned to this job dies the moment the job's last handle closes
    // (RAGReel exiting, including a crash) or TerminateJobObject() is
    // called (cancel(), below) -- unlike process_.kill(), which only signals
    // the direct child and leaves any ffmpeg/mermaid-cli grandchild it
    // already spawned running as an orphan.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            jobHandle_ = job;
        } else {
            CloseHandle(job);
        }
    }
#endif

    // Merge stderr into stdout: video_factory_cloudrag_poc.exe's logLine()
    // writes everything (including ffmpeg's own chatter) to stderr, but a
    // single ordered stream is simpler for the log panel than juggling two.
    process_.setProcessChannelMode(QProcess::MergedChannels);

#ifdef Q_OS_WIN
    connect(&process_, &QProcess::started, this, [this]() {
        if (!jobHandle_) return;
        // PROCESS_SET_QUOTA + PROCESS_TERMINATE is the minimum access
        // AssignProcessToJobObject requires; any grandchild the assigned
        // process later spawns is automatically part of the job too
        // (Windows 8+ supports nested jobs, so this succeeds even if the
        // child inherited an outer job of its own).
        HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                   static_cast<DWORD>(process_.processId()));
        if (proc) {
            AssignProcessToJobObject(static_cast<HANDLE>(jobHandle_), proc);
            CloseHandle(proc);
        }
    });
#endif

    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
        pendingLineBuffer_ += QString::fromUtf8(process_.readAllStandardOutput());
        int newlineIndex;
        while ((newlineIndex = pendingLineBuffer_.indexOf(QLatin1Char('\n'))) >= 0) {
            QString line = pendingLineBuffer_.left(newlineIndex);
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
            }
            pendingLineBuffer_.remove(0, newlineIndex + 1);

            const QRegularExpressionMatch m = renderProgressRegex().match(line);
            if (m.hasMatch()) {
                const double frame = m.captured(1).toDouble();
                const double total = m.captured(2).toDouble();
                if (total > 0.0) {
                    progress_ = std::clamp(frame / total, 0.0, 1.0);
                    emit progressChanged();
                }
            }

            emit outputLine(line);
        }
    });

    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        emit outputLine(QStringLiteral("[launcher] 起動エラー: %1").arg(process_.errorString()));
        // QProcess::finished() is documented to NOT be emitted when start()
        // fails (FailedToStart) -- without this, running (bound to
        // isRunning()/process_.state()) never re-notifies QML after the
        // optimistic runningChanged() in startProcess(), leaving the GUI
        // stuck showing "実行中..." until the app is restarted.
        if (error == QProcess::FailedToStart) {
            cancelRequested_ = false;
            emit runningChanged();
            emit finished(-1, QStringLiteral("起動に失敗しました: %1").arg(process_.errorString()));
        }
    });

    connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                if (!pendingLineBuffer_.isEmpty()) {
                    emit outputLine(pendingLineBuffer_);
                    pendingLineBuffer_.clear();
                }
                // A user-requested cancel() calls kill(), which always
                // reports CrashExit -- without this check that read
                // identically to a genuine ffmpeg/engine crash, telling a
                // non-engineer user their intentional cancellation was an
                // abnormal failure.
                const QString message = (status == QProcess::CrashExit && cancelRequested_)
                    ? QStringLiteral("キャンセルしました")
                    : status == QProcess::CrashExit
                        ? QStringLiteral("動画生成プロセスが異常終了しました")
                        : (exitCode == 0
                               ? QStringLiteral("完了しました")
                               : QStringLiteral("エラーで終了しました（終了コード %1）").arg(exitCode));
                cancelRequested_ = false;
                emit runningChanged();
                emit finished(exitCode, message);
            });
}

ProcessRunner::~ProcessRunner() {
#ifdef Q_OS_WIN
    // Closing the job's last handle triggers JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    // so any still-running subprocess (and its ffmpeg/mermaid-cli
    // grandchildren) is torn down here even if the window was closed
    // without going through cancel() first.
    if (jobHandle_) {
        CloseHandle(static_cast<HANDLE>(jobHandle_));
    }
#endif
}

bool ProcessRunner::isRunning() const {
    return process_.state() != QProcess::NotRunning;
}

QString ProcessRunner::exePath() const {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/video_factory_cloudrag_poc.exe");
}

void ProcessRunner::startProcess(const QStringList& args) {
    if (isRunning()) {
        emit outputLine(QStringLiteral("[launcher] 既に実行中です。完了を待ってください"));
        return;
    }

    const QString exe = exePath();
    if (!QFileInfo::exists(exe)) {
        emit outputLine(QStringLiteral("[launcher] video_factory_cloudrag_poc.exe が見つかりません: %1").arg(exe));
        emit finished(-1, QStringLiteral("実行ファイルが見つかりません"));
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("CLOUD_RAG_URL"), settings_->apiUrl());
    env.insert(QStringLiteral("CLOUD_RAG_API_KEY"), settings_->apiKey());
    process_.setProcessEnvironment(env);
    process_.setWorkingDirectory(QCoreApplication::applicationDirPath());

    pendingLineBuffer_.clear();
    progress_ = 0.0;
    emit progressChanged();
    process_.start(exe, args);
    emit runningChanged();
}

void ProcessRunner::runCloudRagQuery(const QString& topic, const QString& dbKey) {
    // The QML generate button's enabled state gates on trimmed text, but
    // was passing the raw field values through -- pasted trailing
    // whitespace/newlines would reach the subprocess argv untrimmed (e.g.
    // " houdini21" no longer matching a namespace exactly) even though the
    // button looked normally enabled.
    startProcess({topic.trimmed(), dbKey.trimmed()});
}

void ProcessRunner::runHoudiniTutorial(const QString& mdPath) {
    QString base = mdPath;
    if (base.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) {
        base.chop(3);
    }
    const QString jsonPath = base + QStringLiteral(".json");
    const QString screenshotsPath = base + QStringLiteral("_screenshots.json");

    if (!QFileInfo::exists(jsonPath)) {
        emit outputLine(QStringLiteral("[launcher] 対応する.jsonが見つかりません: %1").arg(jsonPath));
        emit finished(-1, QStringLiteral("対応する.jsonファイルが見つかりません"));
        return;
    }

    QStringList args{QStringLiteral("--houdini-md"), mdPath,
                      QStringLiteral("--houdini-json"), jsonPath};
    if (QFileInfo::exists(screenshotsPath)) {
        args << QStringLiteral("--houdini-screenshots") << screenshotsPath;
    }
    startProcess(args);
}

void ProcessRunner::cancel() {
    if (!isRunning()) {
        return;
    }
    // kill() rather than terminate(): a clean shutdown path for a mid-spawn
    // ffmpeg/mermaid-cli grandchild isn't implemented -- immediate
    // termination is the reliable option. TerminateJobObject also reaches
    // any such grandchild directly (see the constructor); process_.kill()
    // alone would only signal this direct child.
    cancelRequested_ = true;
#ifdef Q_OS_WIN
    if (jobHandle_) {
        TerminateJobObject(static_cast<HANDLE>(jobHandle_), 1);
    }
#endif
    process_.kill();
}
