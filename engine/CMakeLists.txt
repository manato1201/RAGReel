# Phase 1: minimal headless QML render -> FFmpeg mux proof of concept.
# No LLM/RAG integration yet (see docs/architecture/video-factory-design.md §8, Phase 1).
#
# The PoC loads TutorialScene.qml from an absolute filesystem path (see
# TUTORIAL_SCENE_QML_PATH below) rather than via qt_add_qml_module's resource
# system, to avoid qrc path-mapping pitfalls for a throwaway single-scene PoC.
# A real qt_add_qml_module setup belongs to Phase 2+ once SceneAssembler
# grows a data-driven template.

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Network Qml Quick Widgets)
find_package(FFMPEG REQUIRED)

qt_standard_project_setup()

add_executable(video_factory_poc
    src/main.cpp
    src/encode/video_encoder.cpp
    src/encode/video_encoder.h
)

target_include_directories(video_factory_poc PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(video_factory_poc PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(video_factory_poc PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Qml
    Qt6::Quick
    ${FFMPEG_LIBRARIES}
)

target_compile_definitions(video_factory_poc PRIVATE
    TUTORIAL_SCENE_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/TutorialScene.qml"
)

# Phase 2 PoC: Cloud RAG query -> narration (TTS) -> headless render ->
# FFmpeg mux (see docs/architecture/video-factory-design.md §8, Phase 2).
add_executable(video_factory_cloudrag_poc
    src/main_cloudrag.cpp
    src/common/app_utils.h
    src/encode/video_encoder.cpp
    src/encode/video_encoder.h
    src/ingest/script_composer.cpp
    src/ingest/script_composer.h
    src/ragclient/cloud_rag_client.cpp
    src/ragclient/cloud_rag_client.h
    src/narration/narration_engine.cpp
    src/narration/narration_engine.h
    src/manifest/manifest_writer.cpp
    src/manifest/manifest_writer.h
    src/orchestrator/job_pipeline.h
    src/orchestrator/orchestrator.h
    src/orchestrator/resource_budget_manager.cpp
    src/orchestrator/resource_budget_manager.h
    src/scene/scene_assembler.cpp
    src/scene/scene_assembler.h
    assets/ragreel.rc
)

target_include_directories(video_factory_cloudrag_poc PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_directories(video_factory_cloudrag_poc PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(video_factory_cloudrag_poc PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    ${FFMPEG_LIBRARIES}
    # NarrationEngine uses plain Win32 SAPI COM (no ATL/sphelper.h -- see
    # engine/src/narration/narration_engine.cpp); ole32/oleaut32 are the
    # standard COM import libs, always present with the Windows SDK.
    ole32
    oleaut32
)

# No compile-time path definitions here anymore -- CloudRagScene.qml,
# mermaid_theme.json, and the local "output" publish directory are all
# resolved at runtime relative to applicationDirPath() (see
# appRelativePath() in main_cloudrag.cpp) so the exe still works once
# copied/installed somewhere other than this build tree. The POST_BUILD
# commands below copy the actual files that path resolution expects to find.

# Qt loads platform integration (platforms/) and TLS backend (tls/) modules
# as runtime plugins, so they aren't picked up by the automatic DLL
# dependency deployment above -- copy them next to each executable
# explicitly, or QGuiApplication / QSslSocket fail at startup with no
# platform plugin / "No functional TLS backend was found" respectively.
get_filename_component(_qt6_install_root "${Qt6_DIR}/../.." ABSOLUTE)
set(_qt6_plugins_dir "${_qt6_install_root}/Qt6/plugins")

function(deploy_qt_plugin_dir target plugin_subdir)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_qt6_plugins_dir}/${plugin_subdir}"
            "$<TARGET_FILE_DIR:${target}>/${plugin_subdir}"
    )
endfunction()

deploy_qt_plugin_dir(video_factory_poc platforms)
deploy_qt_plugin_dir(video_factory_cloudrag_poc platforms)
deploy_qt_plugin_dir(video_factory_cloudrag_poc tls)

# Standalone smoke test for ResourceBudgetManager (IMPROVEMENT_PLAN.md
# Phase 1). No Qt/GPU/GoogleTest dependency -- just the standard library --
# so it can run in CI (Phase 5) before any GPU-capable runner or test
# framework is available. Exits non-zero on failure; see the file for what
# it checks. `add_test` registers it with ctest so `ctest --test-dir build`
# picks it up once Phase 5 wires a ctest-based CI step; running the exe
# directly works today without that.
add_executable(resource_budget_manager_test
    tests/resource_budget_manager_test.cpp
    src/orchestrator/resource_budget_manager.cpp
    src/orchestrator/resource_budget_manager.h
)
target_include_directories(resource_budget_manager_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if (MSVC)
    # This target deliberately has no Qt dependency (see the file comment:
    # exercised without GPU/Qt so it can run in a minimal CI runner), so it
    # doesn't inherit the "-utf-8" compile option Qt6's CMake config attaches
    # to targets that link Qt6::Core -- without it, MSVC misreads this
    # repo's UTF-8-with-Japanese-comments source files under the default
    # (non-UTF-8) system code page (C4819).
    target_compile_options(resource_budget_manager_test PRIVATE -utf-8)
endif()
enable_testing()
add_test(NAME resource_budget_manager_test COMMAND resource_budget_manager_test)

# GUI front end for video_factory_cloudrag_poc.exe aimed at non-engineer
# teammates -- see docs/technical-reference.md launcher section. Deliberately
# built on hand-rolled QtQuick (Rectangle/TextInput) rather than
# QtQuick.Controls, so the only QML plugin it needs deployed is the base
# QtQuick/QtQuick.Window tree (copied explicitly below). Also links
# CloudRagClient directly (NamespaceLister's dbKey picker), which is why it
# needs Qt6::Network + the tls plugin like video_factory_cloudrag_poc.exe;
# QFileDialog (Qt6::Widgets) is the only other extra module and has no
# plugin of its own to copy.
add_executable(RAGReel
    src/main_launcher.cpp
    src/launcher/launcher_settings.cpp
    src/launcher/launcher_settings.h
    src/launcher/process_runner.cpp
    src/launcher/process_runner.h
    src/launcher/native_dialogs.cpp
    src/launcher/native_dialogs.h
    src/launcher/namespace_lister.cpp
    src/launcher/namespace_lister.h
    src/ragclient/cloud_rag_client.cpp
    src/ragclient/cloud_rag_client.h
    assets/ragreel.rc
)

target_include_directories(RAGReel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(RAGReel PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Widgets
)

deploy_qt_plugin_dir(RAGReel platforms)
# NamespaceLister uses CloudRagClient over HTTPS -- same TLS backend plugin
# video_factory_cloudrag_poc.exe needs (see the libssl-3-x64.dll copy below).
deploy_qt_plugin_dir(RAGReel tls)

# Launcher.qml's `import QtQuick` / `import QtQuick.Window` are resolved at
# runtime by QQmlEngine loading each module's qmldir + backing DLL from a
# search path -- unlike the DLL dependencies deployed automatically above,
# these aren't discovered by scanning the exe's PE import table, so they
# need the same explicit copy treatment as the platforms/ plugin dir.
# main_launcher.cpp adds "<app dir>/qml" as an import path to match.
add_custom_command(TARGET RAGReel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_qt6_install_root}/Qt6/qml/QtQuick"
        "$<TARGET_FILE_DIR:RAGReel>/qml/QtQuick"
)

# qopensslbackend.dll (in tls/ above) needs libssl at runtime; libcrypto is
# already picked up transitively but libssl is only a plugin-time dependency.
add_custom_command(TARGET video_factory_cloudrag_poc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_qt6_install_root}/bin/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>"
)
add_custom_command(TARGET RAGReel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_qt6_install_root}/bin/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:RAGReel>"
)

# ---------------------------------------------------------------------------
# Runtime-relative resources (see appRelativePath() in main_cloudrag.cpp and
# the matching load in main_launcher.cpp) -- copied next to each exe instead
# of baked in as compile-time absolute paths, so a copied/installed exe
# still finds them on a machine that never had this source tree checked out.
# ---------------------------------------------------------------------------

add_custom_command(TARGET video_factory_cloudrag_poc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>/qml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/CloudRagScene.qml"
        "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>/qml/CloudRagScene.qml"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/mermaid_theme.json"
        "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>/assets/mermaid_theme.json"
)

# Seeds output/ with the static dashboard shell (index.html/video.html/
# random.html/styles.css/app.js) so appRelativePath("output") is browsable
# on its own -- ManifestWriter only ever writes manifest.json and videos/,
# never these. copy_if_different never touches an already-published
# manifest.json/videos/, so re-running the build doesn't wipe a person's
# accumulated local videos.
foreach(_dashboard_file index.html video.html random.html styles.css app.js)
    add_custom_command(TARGET video_factory_cloudrag_poc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>/output"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/web/public/${_dashboard_file}"
            "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>/output/${_dashboard_file}"
    )
endforeach()

add_custom_command(TARGET RAGReel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RAGReel>/qml"
)
foreach(_launcher_qml_file Launcher.qml WelcomeTab.qml SettingsTab.qml CloudRagTab.qml HoudiniTab.qml LabeledField.qml)
    add_custom_command(TARGET RAGReel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/qml/${_launcher_qml_file}"
            "$<TARGET_FILE_DIR:RAGReel>/qml/${_launcher_qml_file}"
    )
endforeach()
