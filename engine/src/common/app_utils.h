#pragma once

#include <QCoreApplication>
#include <QLatin1Char>
#include <QString>

#include <cstdio>

// Tiny utilities shared by main_cloudrag.cpp and engine/src/ingest/
// script_composer.cpp (IMPROVEMENT_PLAN.md Phase 2 split this file out so
// both could use the same logging/path-resolution helpers instead of each
// having their own copy). Header-only: these are a few lines each and
// don't warrant a .cpp of their own.

// Runtime-computed replacement for what used to be compile-time absolute
// source-tree paths (see the git history around RAGReel distribution) --
// those only ever resolved on the machine that built the exe, which broke
// the moment the binary was copied/installed anywhere else. CMakeLists.txt
// copies the actual files this resolves to right next to each built exe,
// so applicationDirPath() is always the correct root.
inline QString appRelativePath(const QString& relativePath) {
    return QCoreApplication::applicationDirPath() + QLatin1Char('/') + relativePath;
}

// Note: qDebug()/qCritical() output does not reliably reach stderr when
// this console-subsystem exe is launched with redirected stdio in this
// environment (observed while debugging Phase 1 of the original PoC), so
// diagnostics here use std::fprintf(stderr, ...) directly instead.
inline void logLine(const QString& msg) {
    std::fprintf(stderr, "%s\n", msg.toUtf8().constData());
    std::fflush(stderr);
}
