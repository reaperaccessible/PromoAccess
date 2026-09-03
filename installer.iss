; Inno Setup script for PromoAccess
;
; Build it with:
;   ISCC.exe /DMyAppVersion=1.00 /DSourceDir=<repo> /DOutputDir=<repo>\Output installer.iss
;
; The defaults below make a plain "ISCC installer.iss" work from the repository
; root as well, which is what a person building by hand will type first.

#define MyAppName "PromoAccess"
#define MyAppPublisher "ReaperAccessible"
#define MyAppURL "https://reaperaccessible.fr"
#define MyAppExeName "PromoAccess.exe"

; Version is passed on the command line: /DMyAppVersion=1.00
; Keep the two-digit minor — 1.00, 1.01 — as Source/Version.h explains.
#ifndef MyAppVersion
  #define MyAppVersion "1.00"
#endif

; Root of the source tree: /DSourceDir=path
#ifndef SourceDir
  #define SourceDir "."
#endif

; Where the built executable lives, relative to SourceDir or absolute.
#ifndef BinDir
  #define BinDir SourceDir + "\build\Release"
#endif

#ifndef OutputDir
  #define OutputDir "Output"
#endif

[Setup]
AppId={{B0574274-3CCE-40EC-B912-1A74DAB399D2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputBaseFilename=PromoAccessInstaller_{#MyAppVersion}
OutputDir={#OutputDir}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile={#SourceDir}\LICENSE

; Program Files needs administrator rights, but a user without them can still
; install into their own profile rather than being turned away.
PrivilegesRequired=admin
; "commandline" as well as "dialog": it is what lets a silent install into a
; chosen folder be tested without an elevation prompt, and what an unattended
; deployment would use.
PrivilegesRequiredOverridesAllowed=dialog commandline

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

SetupIconFile={#SourceDir}\Source\PromoAccess.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoVersion=1.0.0.0
VersionInfoDescription={#MyAppName} Setup

; Upgrading while the program is running would silently fail to replace the
; locked executable: Setup would report success and the next launch would still
; be the old version. Restart Manager closes it first.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[CustomMessages]
english.AppDesc=Quebec grocery flyers, readable with a screen reader
english.ManualName=PromoAccess manual
french.AppDesc=Les circulaires d'épicerie du Québec, accessibles au lecteur d'écran
french.ManualName=Manuel de PromoAccess

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; One executable and nothing else: the build is static, so it imports only
; Windows system libraries and there is no runtime to redistribute.
Source: "{#BinDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; The manual is read at run time from Docs beside the executable. F1 opens the
; file for the interface language, so both have to be here.
Source: "{#SourceDir}\Docs\manual_fr.html"; DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "{#SourceDir}\Docs\manual_en.html"; DestDir: "{app}\Docs"; Flags: ignoreversion

Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "{cm:AppDesc}"
Name: "{group}\{cm:ManualName}"; Filename: "{app}\Docs\manual_fr.html"; Languages: french
Name: "{group}\{cm:ManualName}"; Filename: "{app}\Docs\manual_en.html"; Languages: english
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Docs is written by Setup, so Setup removes it. Nothing else is created inside
; {app}: the cache, the favourites and the shopping list live in the user's
; profile and are deliberately left alone, so reinstalling does not throw away a
; year of favourites.
Type: filesandordirs; Name: "{app}\Docs"
