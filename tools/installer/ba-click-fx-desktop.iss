#ifndef StageRoot
  #error StageRoot must be provided by the packaging script
#endif
#ifndef OutputRoot
  #error OutputRoot must be provided by the packaging script
#endif
#ifndef ProductVersion
  #error ProductVersion must be provided by the packaging script
#endif
#ifndef NumericVersion
  #error NumericVersion must be provided by the packaging script
#endif
#ifndef PackageVersion
  #error PackageVersion must be provided by the packaging script
#endif
#ifndef OutputBaseName
  #error OutputBaseName must be provided by the packaging script
#endif

#define ProductName "ba-click-fx-desktop"
#define PublisherName "ba-click-fx-desktop contributors"
#define PowerShellPath "{sys}\WindowsPowerShell\v1.0\powershell.exe"

[Setup]
AppId={{573A6AF4-CC20-4AF9-88C1-9AD1FC68BC1E}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppVerName={#ProductName} {#ProductVersion}
AppPublisher={#PublisherName}
AppPublisherURL=https://github.com/CialloKing/ba-click-fx-desktop
AppSupportURL=https://github.com/CialloKing/ba-click-fx-desktop/issues
AppUpdatesURL=https://github.com/CialloKing/ba-click-fx-desktop/releases
VersionInfoVersion={#NumericVersion}
VersionInfoTextVersion={#ProductVersion}
VersionInfoCompany={#PublisherName}
VersionInfoDescription=BAFX click effects installer
VersionInfoProductName={#ProductName}
DefaultDirName={autopf}\ba-click-fx-desktop
DefaultGroupName=ba-click-fx-desktop
DisableProgramGroupPage=yes
LicenseFile={#StageRoot}\LICENSE.txt
OutputDir={#OutputRoot}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
Uninstallable=yes
UninstallDisplayName={#ProductName}
UninstallDisplayIcon={app}\BAFX.ControlCenter.exe
ChangesEnvironment=no
ChangesAssociations=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageRoot}\ba-click-fx-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\BAFX.ControlCenter.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\SUPPORT.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\Identity\*"; DestDir: "{app}\Identity"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\Installer\*"; DestDir: "{app}\Installer"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\ba-click-fx-desktop\BAFX Control Center"; Filename: "{app}\BAFX.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\ba-click-fx-desktop\Uninstall ba-click-fx-desktop"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\BAFX.ControlCenter.exe"; Description: "Launch BAFX Control Center"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallDelete]
Type: filesandordirs; Name: "{app}\Identity"
Type: filesandordirs; Name: "{app}\Installer"

[Code]
var
  UserContextPath: String;
  MachineStatePath: String;
  RegistrationResultPath: String;

function QuoteArgument(const Value: String): String;
var
  Escaped: String;
begin
  Escaped := Value;
  StringChangeEx(Escaped, '"', '""', True);
  Result := '"' + Escaped + '"';
end;

function RunPowerShell(
  const ScriptPath: String;
  const Arguments: String;
  const AsOriginalUser: Boolean;
  var ExitCode: Integer): Boolean;
var
  Parameters: String;
begin
  Parameters := '-NoLogo -NoProfile -ExecutionPolicy Bypass -File ' +
    QuoteArgument(ScriptPath) + ' ' + Arguments;
  if AsOriginalUser then
  begin
    Result := ExecAsOriginalUser(
      ExpandConstant('{#PowerShellPath}'),
      Parameters,
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ExitCode);
  end
  else
  begin
    Result := Exec(
      ExpandConstant('{#PowerShellPath}'),
      Parameters,
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ExitCode);
  end;
end;

procedure RequirePowerShellSuccess(
  const Description: String;
  const ScriptPath: String;
  const Arguments: String;
  const AsOriginalUser: Boolean);
var
  ExitCode: Integer;
begin
  if not RunPowerShell(ScriptPath, Arguments, AsOriginalUser, ExitCode) then
  begin
    RaiseException(Description + ' could not be started.');
  end;
  if ExitCode <> 0 then
  begin
    RaiseException(Description + ' failed with exit code ' + IntToStr(ExitCode) + '.');
  end;
end;

procedure RunBestEffortRollback(
  const InstallRoot: String;
  const InstallerRoot: String;
  const CommonArguments: String);
var
  ExitCode: Integer;
begin
  if not FileExists(MachineStatePath) then
  begin
    Exit;
  end;

  RunPowerShell(
    AddBackslash(InstallerRoot) + 'register-user-package.ps1',
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
      ' -Rollback',
    True,
    ExitCode);
  RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Rollback ' + CommonArguments,
    False,
    ExitCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  InstallRoot: String;
  InstallerRoot: String;
  CommonArguments: String;
  ExitCode: Integer;
begin
  if CurStep <> ssPostInstall then
  begin
    Exit;
  end;

  InstallRoot := ExpandConstant('{app}');
  InstallerRoot := AddBackslash(InstallRoot) + 'Installer';
  UserContextPath := ExpandConstant('{tmp}\bafx-user-context.json');
  MachineStatePath := AddBackslash(InstallerRoot) + 'PREPARE-STATE.json';
  RegistrationResultPath := ExpandConstant('{tmp}\bafx-registration-result.json');

  RequirePowerShellSuccess(
    'Capturing the original user context',
    AddBackslash(InstallerRoot) + 'capture-user-context.ps1',
    '-OutputPath ' + QuoteArgument(UserContextPath),
    True);

  CommonArguments :=
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
    ' -UserContextPath ' + QuoteArgument(UserContextPath) +
    ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
    ' -RegistrationResultPath ' + QuoteArgument(RegistrationResultPath) +
    ' -ProductVersion ' + QuoteArgument('{#ProductVersion}') +
    ' -PackageVersion ' + QuoteArgument('{#PackageVersion}');

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Prepare ' + CommonArguments,
    False,
    ExitCode) then
  begin
    RaiseException('Preparing the machine installation could not be started.');
  end;
  if ExitCode <> 0 then
  begin
    RunBestEffortRollback(InstallRoot, InstallerRoot, CommonArguments);
    RaiseException('Preparing the machine installation failed with exit code ' +
      IntToStr(ExitCode) + '.');
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'register-user-package.ps1',
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -ResultPath ' + QuoteArgument(RegistrationResultPath),
    True,
    ExitCode) then
  begin
    RunBestEffortRollback(InstallRoot, InstallerRoot, CommonArguments);
    RaiseException('Registering the package could not be started.');
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Finalize ' + CommonArguments,
    False,
    ExitCode) then
  begin
    RunBestEffortRollback(InstallRoot, InstallerRoot, CommonArguments);
    RaiseException('Finalizing the machine installation could not be started.');
  end;
  if ExitCode <> 0 then
  begin
    RunBestEffortRollback(InstallRoot, InstallerRoot, CommonArguments);
    RaiseException('Package registration or finalization failed; rollback was attempted.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ExitCode: Integer;
begin
  if CurUninstallStep <> usUninstall then
  begin
    Exit;
  end;
  if not RunPowerShell(
    ExpandConstant('{app}\Installer\unregister-machine.ps1'),
    '-InstallDirectory ' + QuoteArgument(ExpandConstant('{app}')),
    False,
    ExitCode) then
  begin
    RaiseException('The identity uninstaller could not be started.');
  end;
  if ExitCode <> 0 then
  begin
    RaiseException('The identity uninstaller failed with exit code ' + IntToStr(ExitCode) + '.');
  end;
end;
