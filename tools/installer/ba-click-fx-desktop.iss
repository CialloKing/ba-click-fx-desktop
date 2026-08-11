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
  RecoveryRequired: Boolean;

function QuoteArgument(const Value: String): String;
var
  Escaped: String;
begin
  Escaped := Value;
  StringChangeEx(Escaped, '"', '""', True);
  Result := '"' + Escaped + '"';
end;

function CreateOriginalUserStatePath(): String;
var
  TempRoot: String;
begin
  // Inno's protected setup temp directory is read-only for the original user
  // after elevation. A unique file in inherited TEMP keeps the setup directory
  // protected while ExecAsOriginalUser writes its short-lived result.
  TempRoot := GetEnv('TEMP');
  if TempRoot = '' then
  begin
    RaiseException('The original user temporary directory is unavailable.');
  end;
  Result := GenerateUniqueName(TempRoot, '.json');
end;

procedure DeleteTransientState;
begin
  if UserContextPath <> '' then
  begin
    DeleteFile(UserContextPath);
  end;
  if RegistrationResultPath <> '' then
  begin
    DeleteFile(RegistrationResultPath);
  end;
end;

function RunPowerShell(
  const ScriptPath: String;
  const Arguments: String;
  const AsOriginalUser: Boolean;
  var ExitCode: Integer): Boolean;
var
  ContextName: String;
  Parameters: String;
begin
  if AsOriginalUser then
  begin
    ContextName := 'original-user';
  end
  else
  begin
    ContextName := 'elevated';
  end;
  Parameters := '-NoLogo -NoProfile -ExecutionPolicy Bypass -File ' +
    QuoteArgument(ScriptPath) + ' ' + Arguments;
  ExitCode := -1;
  Log('PowerShell [' + ContextName + '] starting: ' + ScriptPath + ' ' + Arguments);
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
  if Result then
  begin
    Log('PowerShell [' + ContextName + '] exited with code ' + IntToStr(ExitCode) +
      ': ' + ScriptPath);
  end
  else
  begin
    Log('PowerShell [' + ContextName + '] could not be started: ' + ScriptPath);
  end;
end;

procedure LogRegistrationResult;
var
  I: Integer;
  Lines: TArrayOfString;
begin
  if not FileExists(RegistrationResultPath) then
  begin
    Log('Package registration result file was not created: ' + RegistrationResultPath);
    Exit;
  end;
  if not LoadStringsFromFile(RegistrationResultPath, Lines) then
  begin
    Log('Package registration result file could not be read: ' + RegistrationResultPath);
    Exit;
  end;
  Log('Package registration result follows:');
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Log('  ' + Lines[I]);
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
  const CommonArguments: String;
  var Succeeded: Boolean);
var
  ExitCode: Integer;
begin
  Succeeded := True;
  if not FileExists(MachineStatePath) then
  begin
    Exit;
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'register-user-package.ps1',
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
      ' -Rollback',
    True,
    ExitCode) then
  begin
    Succeeded := False;
  end
  else if ExitCode <> 0 then
  begin
    Succeeded := False;
  end;

  if not Succeeded then
  begin
    Exit;
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Rollback ' + CommonArguments,
    False,
    ExitCode) then
  begin
    Succeeded := False;
  end
  else if ExitCode <> 0 then
  begin
    Succeeded := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  InstallRoot: String;
  InstallerRoot: String;
  CommonArguments: String;
  ExitCode: Integer;
  RollbackSucceeded: Boolean;
begin
  if CurStep <> ssPostInstall then
  begin
    Exit;
  end;

  InstallRoot := ExpandConstant('{app}');
  InstallerRoot := AddBackslash(InstallRoot) + 'Installer';
  UserContextPath := CreateOriginalUserStatePath();
  MachineStatePath := AddBackslash(InstallerRoot) + 'PREPARE-STATE.json';
  RegistrationResultPath := CreateOriginalUserStatePath();

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
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      RecoveryRequired := True;
      SuppressibleMsgBox(
        'Preparation failed. The installation files and recovery state were retained; reopen Control Center to repair the installation.',
        mbError,
        MB_OK,
        IDOK);
      Exit;
    end;
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
    LogRegistrationResult;
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    RaiseException('Registering the package could not be started.');
  end;
  LogRegistrationResult;
  if ExitCode <> 0 then
  begin
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      RecoveryRequired := True;
      SuppressibleMsgBox(
        'Package registration failed. The installation files and recovery state were retained; reopen Control Center to repair the installation.',
        mbError,
        MB_OK,
        IDOK);
      Exit;
    end;
    RaiseException('Registering the package failed with exit code ' + IntToStr(ExitCode) + '.');
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Finalize ' + CommonArguments,
    False,
    ExitCode) then
  begin
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    RaiseException('Finalizing the machine installation could not be started.');
  end;
  if ExitCode <> 0 then
  begin
    if FileExists(AddBackslash(InstallerRoot) + 'INSTALL-STATE.json') and
      not FileExists(MachineStatePath) then
    begin
      RecoveryRequired := True;
      SuppressibleMsgBox(
        'The package was committed, but final cleanup needs another repair pass from Control Center.',
        mbError,
        MB_OK,
        IDOK);
      Exit;
    end;
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      RecoveryRequired := True;
      SuppressibleMsgBox(
        'Finalization failed. The installation files and recovery state were retained; reopen Control Center to repair the installation.',
        mbError,
        MB_OK,
        IDOK);
      Exit;
    end;
    RaiseException('Package registration or finalization failed; rollback was attempted.');
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if RecoveryRequired then
  begin
    // Recovery data must remain in place, but unattended callers still need a
    // reliable failure signal instead of treating the repair as successful.
    Result := 1001;
  end;
end;

procedure DeinitializeSetup;
begin
  DeleteTransientState;
end;

procedure DeinitializeUninstall;
begin
  DeleteTransientState;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ExitCode: Integer;
  InstallRoot: String;
  InstallerRoot: String;
  MachineStatePath: String;
begin
  if CurUninstallStep <> usUninstall then
  begin
    Exit;
  end;
  InstallRoot := ExpandConstant('{app}');
  InstallerRoot := AddBackslash(InstallRoot) + 'Installer';
  MachineStatePath := AddBackslash(InstallerRoot) + 'PREPARE-STATE.json';
  RegistrationResultPath := CreateOriginalUserStatePath();
  if FileExists(MachineStatePath) then
  begin
    if not RunPowerShell(
      AddBackslash(InstallerRoot) + 'register-user-package.ps1',
      '-InstallDirectory ' + QuoteArgument(InstallRoot) +
        ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
        ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
        ' -Rollback',
      True,
      ExitCode) then
    begin
      RaiseException('The pending package rollback could not be started.');
    end;
    if ExitCode <> 0 then
    begin
      RaiseException('The pending package rollback failed with exit code ' + IntToStr(ExitCode) + '.');
    end;
  end;
  if not RunPowerShell(
    ExpandConstant('{app}\Installer\unregister-machine.ps1'),
    '-InstallDirectory ' + QuoteArgument(InstallRoot),
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
