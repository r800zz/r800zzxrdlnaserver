#define MyAppName "r800zzXRdlnaServer"
#define MyAppVersion "0.2"
#define MyAppPublisher "R800ZZ"
#define MyAppURL "https://vr180g.com/"
#define MyAppExeName "r800zz_dlna_server.exe"

[Setup]
AppId={{EE1DD1D7-2445-4348-983F-5AC57679B381}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2026 R800ZZ

; Install for the current user so administrator privileges are not required.
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; build.bat places the server, worker, models and required DLLs in this folder.
OutputDir=..\dist
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-win64-setup
SetupIconFile=..\resources\r800zzXR_dlnaServer_logo.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

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
VersionInfoVersion=0.1.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Keep every runtime file beside the EXEs. This includes FFmpeg, ONNX Runtime,
; CUDA/cuDNN DLLs and the FP16/FP32 RVM models copied by build.bat.
Source: "..\build_cuda_ep\bin\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
