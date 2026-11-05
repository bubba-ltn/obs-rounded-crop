#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

; Installs to the standard OBS plugin folder: %ProgramData%\obs-studio\plugins\obs-rounded-crop
; (works with OBS 28+ wherever OBS itself is installed).

[Setup]
AppId={{6E1B0C0A-5C1D-4B53-9B8F-2A7B7C1D9F31}
AppName=Rounded Crop for OBS Studio
AppVersion={#AppVersion}
AppPublisher=Bubba
AppPublisherURL=https://github.com/bubba-ltn/obs-rounded-crop
DefaultDirName={commonappdata}\obs-studio\plugins\obs-rounded-crop
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\out
OutputBaseFilename=obs-rounded-crop-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes

[Files]
Source: "..\dist\obs-rounded-crop\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if MsgBox('Please close OBS Studio before continuing. Continue?', mbConfirmation, MB_YESNO) = IDNO then
    Result := False;
end;
