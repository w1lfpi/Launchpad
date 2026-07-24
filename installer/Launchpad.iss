#define MyAppName "Windows Launchpad"
#define MyAppPublisher "w1lfpi"
#define MyAppExeName "Launchpad.exe"
#define MyAppUserModelId "DaniilGorchakov.WindowsLaunchpad"

#ifndef MyAppVersion
  #define MyAppVersion "0.3.0"
#endif

#ifndef SourceExe
  #define SourceExe "..\out\build\launchpad-release\Launchpad.exe"
#endif

#ifndef OutputDirectory
  #define OutputDirectory "..\dist"
#endif

[Setup]
AppId={{669C332F-47D6-4D2A-8AF8-F04A680CC468}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/w1lfpi/Launchpad
AppSupportURL=https://github.com/w1lfpi/Launchpad/issues
AppUpdatesURL=https://github.com/w1lfpi/Launchpad/releases
DefaultDirName={localappdata}\Programs\Windows Launchpad
DefaultGroupName=Launchpad
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir={#OutputDirectory}
OutputBaseFilename=WindowsLaunchpad-{#MyAppVersion}-Setup-x64
SetupIconFile=..\src\assets\launchpad.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
CloseApplications=force
CloseApplicationsFilter={#MyAppExeName}
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousTasks=yes
ChangesAssociations=no
ChangesEnvironment=no
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany=Daniil Gorchakov
VersionInfoDescription=Windows Launchpad installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.AdditionalOptions=Additional options:
english.AutostartTask=Start Launchpad with Windows
english.DesktopIconTask=Create a desktop shortcut
english.RunLaunchpad=Launch Launchpad
russian.AdditionalOptions=Дополнительные параметры:
russian.AutostartTask=Запускать Launchpad вместе с Windows
russian.DesktopIconTask=Создать ярлык на рабочем столе
russian.RunLaunchpad=Запустить Launchpad

[Tasks]
Name: "autostart"; Description: "{cm:AutostartTask}"; GroupDescription: "{cm:AdditionalOptions}"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:DesktopIconTask}"; GroupDescription: "{cm:AdditionalOptions}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{app}\install-launchpad.cmd"
Type: files; Name: "{app}\install-launchpad.ps1"
Type: files; Name: "{app}\uninstall-launchpad.cmd"
Type: files; Name: "{app}\uninstall-launchpad.ps1"

[Icons]
Name: "{autoprograms}\Launchpad"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"; AppUserModelID: "{#MyAppUserModelId}"
Name: "{userdesktop}\Launchpad"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"; AppUserModelID: "{#MyAppUserModelId}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "WindowsLaunchpad"; ValueData: """{app}\{#MyAppExeName}"" --background"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:RunLaunchpad}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--shutdown"; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "StopWindowsLaunchpad"

[Code]
function GetWindowThreadProcessId(
  WindowHandle: HWND;
  var ProcessId: DWORD): DWORD;
external 'GetWindowThreadProcessId@user32.dll stdcall';

procedure StopRunningLaunchpad;
var
  ResultCode: Integer;
  InstalledExecutable: String;
  WindowHandle: HWND;
  ProcessId: DWORD;
begin
  WindowHandle :=
    FindWindowByClassName('WindowsLaunchpad.Window');
  if WindowHandle = 0 then
    exit;

  InstalledExecutable :=
    ExpandConstant('{app}\{#MyAppExeName}');
  if FileExists(InstalledExecutable) then
  begin
    Exec(
      InstalledExecutable,
      '--shutdown',
      ExpandConstant('{app}'),
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode);
    Sleep(500);
  end;

  { Versions older than 0.3.0 do not understand --shutdown. }
  WindowHandle :=
    FindWindowByClassName('WindowsLaunchpad.Window');
  if WindowHandle <> 0 then
  begin
    ProcessId := 0;
    GetWindowThreadProcessId(WindowHandle, ProcessId);
    if ProcessId <> 0 then
    begin
      Exec(
        ExpandConstant('{sys}\taskkill.exe'),
        '/PID ' + IntToStr(ProcessId) + ' /T /F',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode);
    end;
  end;

  { ARM64 Windows may briefly retain the x64 image in XtaCache. }
  Sleep(1500);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningLaunchpad;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopRunningLaunchpad;
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and
     (not WizardIsTaskSelected('autostart')) then
  begin
    RegDeleteValue(
      HKCU,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'WindowsLaunchpad');
  end;
  if (CurStep = ssPostInstall) and
     (not WizardIsTaskSelected('desktopicon')) then
  begin
    DeleteFile(
      ExpandConstant('{userdesktop}\Launchpad.lnk'));
  end;
end;
