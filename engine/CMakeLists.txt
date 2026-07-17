# Phase 1: minimal headless QML render -> FFmpeg mux proof of concept.
# No LLM/RAG integration yet (see docs/architecture/video-factory-design.md §8, Phase 1).
#
# The PoC loads TutorialScene.qml from an absolute filesystem path (see
# TUTORIAL_SCENE_QML_PATH below) rather than via qt_add_qml_module's resource
# system, to avoid qrc path-mapping pitfalls for a throwaway single-scene PoC.
# A real qt_add_qml_module setup belongs to Phase 2+ once SceneAssembler
# grows a data-driven template.

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Network Qml Quick)
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
    src/encode/video_encoder.cpp
    src/encode/video_encoder.h
    src/ragclient/cloud_rag_client.cpp
    src/ragclient/cloud_rag_client.h
    src/narration/narration_engine.cpp
    src/narration/narration_engine.h
    src/manifest/manifest_writer.cpp
    src/manifest/manifest_writer.h
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

target_compile_definitions(video_factory_cloudrag_poc PRIVATE
    CLOUD_RAG_SCENE_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/CloudRagScene.qml"
    MERMAID_THEME_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/mermaid_theme.json"
    # Absolute path to web/public so a generated video can publish itself
    # into the dashboard (ManifestWriter) regardless of the exe's cwd.
    WEB_PUBLIC_DIR="${CMAKE_SOURCE_DIR}/web/public"
)

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

# qopensslbackend.dll (in tls/ above) needs libssl at runtime; libcrypto is
# already picked up transitively but libssl is only a plugin-time dependency.
add_custom_command(TARGET video_factory_cloudrag_poc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_qt6_install_root}/bin/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:video_factory_cloudrag_poc>"
)
