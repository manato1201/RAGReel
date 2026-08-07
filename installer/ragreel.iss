; RAGReel installer -- packages RAGReel.exe (GUI launcher) together with
; video_factory_cloudrag_poc.exe (the actual generation engine it drives via
; QProcess) and every DLL/plugin both need. Built from build/engine, which
; must already contain a Release build of both targets (run cmake --build
; first -- see docs/technical-reference.md).
;
; Per the "各自のPC内のローカルフォルダに出力" decision, every install gets
; its own {app}\output\ dashboard; manifest.json/videos\ are seeded only if
; missing so re-running this installer over an existing install never wipes
; someone's already-generated videos.

#define MyAppName "RAGReel"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "RAGReel"
#define MyAppExeName "RAGReel.exe"
#define BuildDir "..\build\engine"

[Setup]
AppId={{16D15ECD-5C8C-4677-9B08-E7386D851014}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=RAGReel-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
SetupIconFile=..\engine\assets\ragreel.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; --- Executables + all DLL dependencies (always overwrite on update) ---
Source: "{#BuildDir}\RAGReel.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\video_factory_cloudrag_poc.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; --- Qt runtime plugins (platform integration + TLS backend) ---
Source: "{#BuildDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- QML sources both exes load at runtime (see appRelativePath() in
;     main_cloudrag.cpp / main_launcher.cpp) ---
Source: "{#BuildDir}\qml\*"; DestDir: "{app}\qml"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Mermaid diagram theme config ---
Source: "{#BuildDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Local dashboard shell: always refresh the static UI files, but never
;     touch manifest.json/videos\ if they already exist from a prior run ---
Source: "{#BuildDir}\output\index.html"; DestDir: "{app}\output"; Flags: ignoreversion
Source: "{#BuildDir}\output\video.html"; DestDir: "{app}\output"; Flags: ignoreversion
Source: "{#BuildDir}\output\random.html"; DestDir: "{app}\output"; Flags: ignoreversion
Source: "{#BuildDir}\output\styles.css"; DestDir: "{app}\output"; Flags: ignoreversion
Source: "{#BuildDir}\output\app.js"; DestDir: "{app}\output"; Flags: ignoreversion
Source: "{#BuildDir}\output\manifest.json"; DestDir: "{app}\output"; Flags: onlyifdoesntexist

[Dirs]
Name: "{app}\output\videos"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
