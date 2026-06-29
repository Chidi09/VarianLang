; ============================================================================
;  Varian — Windows installer (Inno Setup)
;
;  Produces a clean, standard setup wizard for the Varian toolchain:
;    - installs vn.exe + the standard library (vn_modules) + bundled runtime
;      DLLs into Program Files\Varian
;    - adds Varian to the system PATH (so `vn` works in any terminal)
;    - sets VARIAN_HOME
;    - Start Menu shortcuts + documentation link
;    - a proper uninstaller (appears in "Apps & features" / Add-Remove Programs)
;
;  Build:  iscc /DSourcePath=..\dist\varian-windows-x64 installer\varian.iss
;  (the dist payload is produced by the AppVeyor Windows job / `make deploy`.)
; ============================================================================

#define MyAppName "Varian"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "VarianLang"
#define MyAppURL "https://github.com/Chidi09/VarianLang"
#define MyAppExeName "vn.exe"

; Where the built payload lives. Override on the command line with /DSourcePath=...
#ifndef SourcePath
  #define SourcePath "..\dist\varian-windows-x64"
#endif

[Setup]
; A stable, unique AppId keeps upgrades/uninstalls clean across versions.
AppId={{46339166-0A4B-4584-B836-32363FE560FF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\Varian
DefaultGroupName=Varian
DisableProgramGroupPage=yes
LicenseFile={#SourcePath}\LICENSE
OutputDir=output
OutputBaseFilename=varian-setup-{#MyAppVersion}-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupIconFile=assets\varian.ico
WizardImageFile=assets\wizard-large.bmp
WizardSmallImageFile=assets\wizard-small.bmp
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
ChangesEnvironment=yes
PrivilegesRequired=admin
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; Branding shown on the welcome page — the whole Varian ecosystem in one binary.
WelcomeLabel2=This will install [name/ver] on your computer.%n%nVarian is one language for the whole stack: the Zenith web framework, the Lumen frontend, the Kiln build tool, the Constellation package registry, and the Meridian ORM — all in a single native binary.%n%nIt is recommended that you close all other applications before continuing.

[Tasks]
Name: "addtopath"; Description: "Add Varian to the system &PATH (recommended)"; GroupDescription: "System integration:"

[Files]
Source: "{#SourcePath}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Varian Shell"; Filename: "{cmd}"; Parameters: "/K ""echo Varian {#MyAppVersion} ready.  Try: vn --help && set PATH={app};%PATH%"""; IconFilename: "{app}\{#MyAppExeName}"; Comment: "Open a terminal with Varian on PATH"
Name: "{group}\Varian Documentation"; Filename: "{#MyAppURL}#readme"
Name: "{group}\{cm:UninstallProgram,Varian}"; Filename: "{uninstallexe}"

[Registry]
; System PATH (only appended when not already present — see NeedsAddPath).
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}'))
; VARIAN_HOME so the stdlib resolves even if vn.exe is invoked by full path.
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "VARIAN_HOME"; ValueData: "{app}"; \
    Flags: preservestringtype

[Run]
Filename: "{#MyAppURL}#readme"; Description: "Open the Varian documentation"; \
    Flags: postinstall skipifsilent shellexec nowait

[Code]
{ Append to PATH only if the directory isn't already on it (avoids duplicates
  across re-installs/upgrades). }
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  { look for the exact dir, case-insensitively, wrapped in ; on both sides }
  Result := Pos(';' + Lowercase(Param) + ';', ';' + Lowercase(OrigPath) + ';') = 0;
end;

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
