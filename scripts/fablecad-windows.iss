[Setup]
AppId=FableCAD.2027
AppName=FableCAD 2027
AppVersion=2.2.10
AppPublisher=FableCAD
DefaultDirName={localappdata}\Programs\FableCAD 2027
DefaultGroupName=FableCAD 2027
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=FableCAD-2027-2.2.10-Windows-x64
SetupIconFile=..\librecad\res\images\fablecad.ico
UninstallDisplayIcon={app}\FableCAD.exe
LicenseFile=..\licenses\gpl-2.0.txt
Compression=lzma2
SolidCompression=yes

[Files]
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\FableCAD 2027"; Filename: "{app}\FableCAD.exe"

[Run]
Filename: "{app}\FableCAD.exe"; Description: "Launch FableCAD 2027"; Flags: nowait postinstall skipifsilent
