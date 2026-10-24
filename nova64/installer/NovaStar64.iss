; NovaStar64 installer for Windows -- built with Inno Setup 6.3 or later
; (https://jrsoftware.org/isinfo.php).
;
;   iscc /DAppVersion=0.1.0 /DSourceDir=..\build\RelWithDebInfo /DOutputDir=..\..\dist installer\NovaStar64.iss
;
; The setup wizard asks where to install and whether to add a desktop shortcut, and
; offers to start the program at the end. It installs for the current user by default
; (no administrator rights needed) and for everyone if the user picks that. Settings,
; games and saves go in %LOCALAPPDATA%\NovaStar64, not in the install folder, so
; uninstalling or reinstalling never touches them.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\RelWithDebInfo"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
; AppId identifies this program to Windows so a newer installer upgrades it in place.
; Never change it.
AppId={{9155E91A-813B-4CB8-A225-1E500E8B8B1B}
AppName=NovaStar64
AppVersion={#AppVersion}
AppVerName=NovaStar64 {#AppVersion}
AppPublisher=Varga Bálint
AppPublisherURL=https://github.com/V-B0/NovaStar64
AppSupportURL=https://github.com/V-B0/NovaStar64/issues
AppUpdatesURL=https://github.com/V-B0/NovaStar64/releases
DefaultDirName={autopf}\NovaStar64
DefaultGroupName=NovaStar64
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
SetupIconFile=..\assets\icon\Novastar.ico
UninstallDisplayIcon={app}\nova64.exe
OutputDir={#OutputDir}
OutputBaseFilename=NovaStar64-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; No portable.txt here on purpose: its absence is what makes an installed copy keep
; its data in %LOCALAPPDATA%\NovaStar64 (see SelectDataDirectory in main.cpp).
Source: "{#SourceDir}\nova64.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\NovaStar64"; Filename: "{app}\nova64.exe"
Name: "{autodesktop}\NovaStar64"; Filename: "{app}\nova64.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\nova64.exe"; Description: "{cm:LaunchProgram,NovaStar64}"; Flags: nowait postinstall skipifsilent
