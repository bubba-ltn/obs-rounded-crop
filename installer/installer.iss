#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{6E1B0C0A-5C1D-4B53-9B8F-2A7B7C1D9F31}
AppName=Rounded Crop for OBS Studio
AppVersion={#AppVersion}
AppPublisher=Bubba
AppPublisherURL=https://github.com/bubba-ltn/obs-rounded-crop
DefaultDirName={code:GetObsDir}
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousAppDir=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\out
OutputBaseFilename=obs-rounded-crop-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
CreateUninstallRegKey=yes
UninstallFilesDir={app}\obs-plugins\64bit\rounded-crop-uninstall

[Files]
Source: "..\dist\obs-plugins\*"; DestDir: "{app}\obs-plugins"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\dist\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function GetObsDir(Param: string): string;
var
  Dir: string;
begin
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Dir) and DirExists(Dir) then
    Result := Dir
  else
    Result := ExpandConstant('{autopf64}\obs-studio');
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if FileExists(ExpandConstant('{autopf64}\obs-studio\bin\64bit\obs64.exe')) = False then
    if not DirExists(GetObsDir('')) then
      if MsgBox('OBS Studio was not found. Continue anyway and choose the folder manually?', mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
end;
