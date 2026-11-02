; Inno Setup 6 script for the Trino ODBC driver.
;
; Build:  ISCC.exe /DAppVersion=1.0.0 /DDistDir=..\..\dist\windows-x64 /DArch=x64 installer.iss
; (packaging\windows\build.ps1 -Installer does this.) DistDir must contain trinoodbc.dll and its
; runtime DLLs (libcurl.dll, zlib1.dll, zstd.dll, lz4.dll, ...) as assembled by build.ps1.
;
; What the installer does:
;   * copies the driver and its dependencies to {autopf}\TrinoODBC (Program Files)
;   * registers the driver in HKLM\SOFTWARE\ODBC\ODBCINST.INI (Driver, Setup, APILevel 2,
;     ConnectFunctions YYN, DriverODBCVer 03.80, FileUsage 0, SQLLevel 1, UsageCount) and in the
;     "ODBC Drivers" list; uninstalling removes both
;   * installs the Visual C++ runtime when vc_redist.<arch>.exe was placed in DistDir
;     (build.ps1 -DownloadVcRedist)
; Installing over an existing version replaces the files (the ODBC Administrator must be closed).
;
; Branding: the Trino logo (packaging/assets/make_icons.sh -> packaging/assets/generated) is the setup
; icon (SetupIconFile), the wizard images (WizardImageFile 164x314, WizardSmallImageFile 55x58) and
; the Add/Remove Programs icon ({app}\trinoodbc.ico). /DAssetsDir=<dir> selects another folder.
; Trino and the Trino logo are trademarks of the Trino Software Foundation; this driver is an
; independent project, not affiliated with or endorsed by it. The file is UTF-8 with a BOM (Türkiye).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\..\dist\windows-x64"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef AssetsDir
  #define AssetsDir "..\assets\generated"
#endif
; Driver default of the TimeZone key (kDefaultSessionTimeZone in src/core/config.h).
#define DefaultTimeZone "Europe/Istanbul"
#define DriverName "Trino ODBC Driver"
#define AppName "Trino ODBC Driver"
#define AppPublisher "Trino ODBC project"
#define AppUrl "https://github.com/AhmetFurkanDEMIR/TrinoODBC"

[Setup]
AppId={{7E1C2D8A-5B3F-4C2E-9A1D-6F0B2C3D4E5F}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} ({#Arch})
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} installer
DefaultDirName={autopf}\TrinoODBC
DisableDirPage=no
DisableProgramGroupPage=yes
DisableReadyPage=no
OutputBaseFilename=TrinoODBC-{#AppVersion}-{#Arch}
OutputDir=..\..\dist
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
; Windows 10 version 1903 (build 18362) or later (Windows 11, Server 2022): the driver resolves IANA
; time zones through the combined system icu.dll that Windows ships since 1903, loaded from System32
; only. Older systems (Windows 7/8.x, early Windows 10, Server 2016/2019) lack it. Keep in sync with
; docs/INSTALL-WINDOWS.md and register_driver.ps1.
MinVersion=10.0.18362
UninstallDisplayName={#AppName} ({#Arch})
UninstallDisplayIcon={app}\trinoodbc.ico
SetupIconFile={#AssetsDir}\trinoodbc.ico
WizardImageFile={#AssetsDir}\installer-welcome.bmp
WizardSmallImageFile={#AssetsDir}\installer-small.bmp
VersionInfoCopyright=Licensed under the Apache License, Version 2.0
WizardStyle=modern
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
#if Arch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#elif Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
; 32-bit driver: installs to Program Files (x86) and registers under the WOW6432Node ODBC view,
; which is exactly what 32-bit applications and the 32-bit ODBC Administrator read.
ArchitecturesAllowed=x86compatible
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nIt is recommended that you close the ODBC Data Source Administrator and applications that use the driver before continuing.%n%nTrino and the Trino logo are trademarks of the Trino Software Foundation. This driver is an independent project and is not affiliated with or endorsed by the Trino Software Foundation.
FinishedLabelNoIcons=Setup has finished installing [name] on your computer.%n%nCreate a DSN in the ODBC Data Source Administrator. Time zone: the DSN dialog pre-fills {#DefaultTimeZone} (UTC+3, Türkiye / Istanbul), stored as the TimeZone key; pick or type another IANA zone, or 'system' for the Windows zone.
FinishedLabel=Setup has finished installing [name] on your computer.%n%nCreate a DSN in the ODBC Data Source Administrator. Time zone: the DSN dialog pre-fills {#DefaultTimeZone} (UTC+3, Türkiye / Istanbul), stored as the TimeZone key; pick or type another IANA zone, or 'system' for the Windows zone.

[Files]
; Trino logo for the Add/Remove Programs entry (UninstallDisplayIcon).
Source: "{#AssetsDir}\trinoodbc.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\trinoodbc.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#DistDir}\trinoodbc.pdb"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Runtime dependencies produced by vcpkg (curl, zlib, zstd, lz4, simdjson, ...).
Source: "{#DistDir}\*.dll"; DestDir: "{app}"; Excludes: "trinoodbc.dll"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#DistDir}\register_driver.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#DistDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#DistDir}\vc_redist.{#Arch}.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[Registry]
; Driver list entry.
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers"; Flags: uninsdeletekeyifempty
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers"; ValueType: string; ValueName: "{#DriverName}"; ValueData: "Installed"; Flags: uninsdeletevalue
; Driver specification section (removed as a whole on uninstall).
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "Driver"; ValueData: "{app}\trinoodbc.dll"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "Setup"; ValueData: "{app}\trinoodbc.dll"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "Description"; ValueData: "{#AppName} {#AppVersion} (ODBC 3.8, Unicode)"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "APILevel"; ValueData: "2"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "ConnectFunctions"; ValueData: "YYN"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "DriverODBCVer"; ValueData: "03.80"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "FileUsage"; ValueData: "0"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: string; ValueName: "SQLLevel"; ValueData: "1"
Root: HKLM; Subkey: "SOFTWARE\ODBC\ODBCINST.INI\{#DriverName}"; ValueType: dword; ValueName: "UsageCount"; ValueData: 1

[Run]
Filename: "{tmp}\vc_redist.{#Arch}.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."; Check: VcRedistBundled; Flags: waituntilterminated
Filename: "{sys}\odbcad32.exe"; Description: "Open the ODBC Data Source Administrator"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\docs"

[Code]
function VcRedistBundled(): Boolean;
begin
  Result := FileExists(ExpandConstant('{tmp}\vc_redist.{#Arch}.exe'));
end;

// Guard against two setups running at once (both would try to replace the same DLL).
function InitializeSetup(): Boolean;
begin
  Result := True;
  if CheckForMutexes('TrinoODBCSetupMutex') then begin
    MsgBox('Another Trino ODBC driver setup is already running.', mbError, MB_OK);
    Result := False;
    exit;
  end;
  CreateMutex('TrinoODBCSetupMutex');
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if MsgBox('User and system DSNs that reference the {#DriverName} stop working after the driver is removed.' + #13#10 +
            'Continue with the uninstall?', mbConfirmation, MB_YESNO) = IDNO then
    Result := False;
end;
