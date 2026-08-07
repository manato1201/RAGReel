#include "process_runner.h"
#include "launcher_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

ProcessRunner::ProcessRunner(LauncherSettings* settings, QObject* parent)
    : QObject(parent), settings_(settings) {
    // Merge stderr into stdout: video_factory_cloudrag_poc.exe's logLine()
    // writes everything (including ffmpeg's own chatter) to stderr, but a
    // single ordered stream is simpler for the log panel than juggling two.
    process_.setProcessChannelMode(QProcess::MergedChannels);

    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
        pendingLineBuffer_ += QString::fromUtf8(process_.readAllStandardOutput());
        int newlineIndex;
        while ((newlineIndex = pendingLineBuffer_.indexOf(QLatin1Char('\n'))) >= 0) {
            QString line = pendingLineBuffer_.left(newlineIndex);
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
            }
            pendingLineBuffer_.remove(0, newlineIndex + 1);
            emit outputLine(line);
        }
    });

    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit outputLine(QStringLiteral("[launcher] 起動エラー: %1").arg(process_.errorString()));
    });

    connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                if (!pendingLineBuffer_.isEmpty()) {
                    emit outputLine(pendingLineBuffer_);
                    pendingLineBuffer_.clear();
                }
                const QString message = status == QProcess::CrashExit
                    ? QStringLiteral("動画生成プロセスが異常終了しました")
                    : (exitCode == 0
                           ? QStringLiteral("完了しました")
                           : QStringLiteral("エラーで終了しました（終了コード %1）").arg(exitCode));
                emit runningChanged();
                emit finished(exitCode, message);
            });
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
    process_.start(exe, args);
    emit runningChanged();
}

void ProcessRunner::runCloudRagQuery(const QString& topic, const QString& dbKey) {
    startProcess({topic, dbKey});
}

void ProcessRunner::runHoudiniTutorial(const QString& mdPath) {
    QString base = mdPath;
    if (base.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) {
        base.chop(3);
    }
    const QString jsonPath = base + QStringLiteral(".json");
    const QString screenshotsPath = base + QStringLiteral("_screenshots.json");

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
    // kill() rather than terminate(): the child may itself be mid-spawn of
    // an ffmpeg/mermaid-cli grandchild, and a clean shutdown path for that
    // isn't implemented -- immediate termination is the reliable option.
    process_.kill();
}
