; Inno Setup script for the Windows installer. Built by .github/workflows/build.yml:
;   iscc /DAppVersion=1.2.3 /DSourceDir=..\dist packaging\treefern.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif

[Setup]
AppId={{6F1C2B9E-4D0A-4C5E-9B7A-2E8F3D1A7C40}
AppName=TreeFern
AppVersion={#AppVersion}
AppPublisher=TreeFern
AppPublisherURL=https://github.com/DocRountree/treefern
DefaultDirName={autopf}\TreeFern
DefaultGroupName=TreeFern
UninstallDisplayIcon={app}\treefern.exe
LicenseFile=..\LICENSE
OutputDir=..
OutputBaseFilename=TreeFern-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install by default (no admin prompt); users may choose all-users.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocjson"; Description: "Open .json cladograms with TreeFern (adds to ""Open with"")"; GroupDescription: "File associations:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TreeFern"; Filename: "{app}\treefern.exe"
Name: "{autodesktop}\TreeFern"; Filename: "{app}\treefern.exe"; Tasks: desktopicon

[Registry]
; Registers TreeFern under "Open with" for .json without taking over the default handler.
Root: HKA; Subkey: "Software\Classes\Applications\treefern.exe\shell\open\command"; ValueType: string; ValueData: """{app}\treefern.exe"" ""%1"""; Flags: uninsdeletekey; Tasks: assocjson
Root: HKA; Subkey: "Software\Classes\.json\OpenWithList\treefern.exe"; Flags: uninsdeletekey; Tasks: assocjson

[Run]
Filename: "{app}\treefern.exe"; Description: "{cm:LaunchProgram,TreeFern}"; Flags: nowait postinstall skipifsilent
