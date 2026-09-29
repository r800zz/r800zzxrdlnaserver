#define MyAppName "r800zzXRdlnaServer"
#define MyAppVersion "0.3"
#define MyAppPublisher "R800ZZ"
#define MyAppURL "https://vr180g.com/"
#define MyAppExeName "r800zz_dlna_server.exe"

[Setup]
; Keep this AppId unchanged across releases so upgrades use the same application entry.
AppId={{EE1DD1D7-2445-4348-983F-5AC57679B381}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2026 R800ZZ
AppComments=AI Passthrough DLNA Server for r800zzvrplayer

; Install for the current user so administrator privileges are not required.
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; main.cpp creates this mutex while the server GUI is running.
AppMutex=Local\R800ZZ_XR_DLNA_SERVER_SINGLE_INSTANCE

; build.bat places the current server, unified worker, models and runtime DLLs here.
OutputDir=..\dist
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-win64-setup
SetupIconFile=..\resources\r800zzXR_dlnaServer_logo.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}

Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
AllowNoIcons=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

; Windows version metadata for the installer EXE.
VersionInfoVersion=0.3.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (C) 2026 R800ZZ

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Install only files in build_cuda_ep\bin itself.
; Do NOT recurse into bin\dml: that directory is stale output from the old
; dedicated DirectML worker path and contains duplicate/old runtime DLLs.
Source: "..\build_cuda_ep\bin\*"; DestDir: "{app}"; Excludes: "*.pdb,*.ilk,*.exp,*.lib"; Flags: ignoreversion

; Optional third-party license/notice files for public binary distribution.
Source: "..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{userprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

