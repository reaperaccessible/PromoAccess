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
  #define MyAppVersion "1.05"
#endif

; Root of the source tree: /DSourceDir=path
#ifndef SourceDir
  #define SourceDir "."
#endif

; Where the built executable lives, relative to SourceDir or absolute.
#ifndef BinDir
  #define BinDir SourceDir + "\build\Release"
#endif

; Windows wants four numbers where we display two: 1.01 -> 1.0.1.0. Passed in
; rather than derived, because "1.01" cannot be split into a quad by the
; preprocessor without pretending the minor is not two digits.
#ifndef MyAppVersionQuad
  #define MyAppVersionQuad "1.0.5.0"
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
; The brand belongs in the filename: what people keep in their Downloads folder,
; and what a screen reader spells out before they open it, should say who made
; it. The version-less twin of this file is what the permanent download link and
; the in-app updater both look for — see the note in Updater.cpp.
OutputBaseFilename=ReaperAccessible-PromoAccess-Installer_{#MyAppVersion}
OutputDir={#OutputDir}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

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
VersionInfoVersion={#MyAppVersionQuad}
VersionInfoDescription={#MyAppName} Setup

; Upgrading while the program is running would silently fail to replace the
; locked executable: Setup would report success and the next launch would still
; be the old version. Restart Manager closes it first.
CloseApplications=yes
RestartApplications=no

[Languages]
; One licence page per language. The GPL has no official translation — only the
; English text is binding — so the French page says so plainly and explains the
; terms, while LICENSE, the text that governs, is installed with the program.
Name: "english"; MessagesFile: "compiler:Default.isl"; LicenseFile: "{#SourceDir}\LICENSE"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"; LicenseFile: "{#SourceDir}\Docs\LICENCE-fr.txt"

[CustomMessages]
; Inno's own CreateDesktopIcon carries an "&" accelerator — "Créer une icône sur
; le &Bureau". The tasks list draws its text raw, so the ampersand is READ OUT,
; which is exactly what a screen-reader user does not need. Overridden here
; without it, in both languages.
english.CreateDesktopIcon=Create a desktop shortcut
french.CreateDesktopIcon=Créer une icône sur le Bureau

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

; The marker the in-app updater looks for. Its absence means this copy was
; unpacked by hand, and the updater then leaves it alone rather than moving the
; program somewhere the user did not choose.
Source: "{#BinDir}\{#MyAppExeName}"; DestDir: "{app}"; AfterInstall: CreateInstalledMarker; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "{cm:AppDesc}"
Name: "{group}\{cm:ManualName}"; Filename: "{app}\Docs\manual_fr.html"; Languages: french
Name: "{group}\{cm:ManualName}"; Filename: "{app}\Docs\manual_en.html"; Languages: english
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

; The in-app updater runs this installer as "/SILENT /AUTOUPDATE=1", which skips
; the checkbox above. This entry fires only on that path and puts the program
; back up even in silent mode — as the original, non-elevated user, and with
; /fromupdate so the new instance pulls itself to the foreground and the screen
; reader lands in it rather than behind the installer.
Filename: "{app}\{#MyAppExeName}"; Parameters: "/fromupdate"; Flags: nowait runasoriginaluser; Check: IsAutoUpdate

[UninstallDelete]
; Docs is written by Setup, so Setup removes it. Nothing else is created inside
; {app}: the cache, the favourites and the shopping list live in the user's
; profile and are deliberately left alone, so reinstalling does not throw away a
; year of favourites.
Type: filesandordirs; Name: "{app}\Docs"
Type: files; Name: "{app}\installed.txt"

[Code]
// Written after the executable is in place. The updater reads nothing but its
// presence.
procedure CreateInstalledMarker();
begin
  SaveStringToFile(ExpandConstant('{app}\installed.txt'), 'Installed via setup', False);
end;

// True when the in-app updater launched us with /AUTOUPDATE=1.
function IsAutoUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:AUTOUPDATE|0}') = '1';
end;
