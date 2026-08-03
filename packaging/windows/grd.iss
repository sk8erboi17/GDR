#define AppVersion "0.2.9"
#define AppName "GRD"
#define AppPublisher "sk8erboi17"

[Setup]
AppId={{056A433B-F033-42F8-8B50-7ECB800AAB08}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\GRD
DefaultGroupName=GRD
UninstallDisplayIcon={app}\grd.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\..\dist\installer
OutputBaseFilename=GRD-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes

[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\..\dist\windows\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\GRD"; Filename: "{app}\grd.exe"
Name: "{autodesktop}\GRD"; Filename: "{app}\grd.exe"; Tasks: desktopicon

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""GRD Remote Desktop TCP"" dir=in action=allow protocol=TCP localport=47990 profile=private"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""GRD Remote Desktop Video UDP"" dir=in action=allow protocol=UDP localport=47990 profile=private"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""GRD Discovery UDP"" dir=in action=allow protocol=UDP localport=47989 profile=private"; Flags: runhidden waituntilterminated
Filename: "{app}\grd.exe"; Description: "{cm:LaunchProgram,GRD}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""GRD Remote Desktop TCP"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""GRD Remote Desktop Video UDP"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""GRD Discovery UDP"""; Flags: runhidden waituntilterminated
