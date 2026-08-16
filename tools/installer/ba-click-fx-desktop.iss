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
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
UninstallLogging=yes
Uninstallable=yes
UninstallDisplayName={#ProductName}
UninstallDisplayIcon={app}\BAFX.ControlCenter.exe
ChangesEnvironment=no
ChangesAssociations=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
; Keep the installer self-contained. This translation is pinned from
; jrsoftware/issrc commit 5680c948e1de07e71cbd27cad7d4f5e75223afba.
Name: "chinesesimplified"; MessagesFile: "{#SourcePath}\ChineseSimplified.isl"

[Files]
Source: "{#StageRoot}\ba-click-fx-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\BAFX.ControlCenter.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\SUPPORT.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\Identity\*"; DestDir: "{app}\Identity"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\Installer\*"; DestDir: "{app}\Installer"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\ba-click-fx-desktop\BAFX Control Center"; Filename: "{app}\BAFX.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\BAFX Control Center"; Filename: "{app}\BAFX.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\ba-click-fx-desktop\{cm:UninstallProgram,{#ProductName}}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\BAFX.ControlCenter.exe"; Description: "{cm:LaunchProgram,BAFX Control Center}"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallDelete]
Type: filesandordirs; Name: "{app}\Identity"
Type: filesandordirs; Name: "{app}\Installer"

[Code]
var
  UserContextPath: String;
  MachineStatePath: String;
  RegistrationResultPath: String;
  RollbackResultPath: String;
  RecoveryRequired: Boolean;
  LastPowerShellFailureSummary: String;
  LastPowerShellRawOutput: String;
  LastPowerShellOutputError: String;

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
    DeleteFile(UserContextPath + '.diagnostic.txt');
  end;
  if RegistrationResultPath <> '' then
  begin
    DeleteFile(RegistrationResultPath);
    DeleteFile(RegistrationResultPath + '.diagnostic.txt');
  end;
  if RollbackResultPath <> '' then
  begin
    DeleteFile(RollbackResultPath);
    DeleteFile(RollbackResultPath + '.diagnostic.txt');
  end;
end;

procedure ResetPowerShellDiagnostics;
begin
  LastPowerShellFailureSummary := '';
  LastPowerShellRawOutput := '';
  LastPowerShellOutputError := '';
end;

procedure RememberPowerShellRawOutput(const S: String);
var
  Remaining: Integer;
begin
  if Length(LastPowerShellRawOutput) >= 2400 then
  begin
    Exit;
  end;
  Remaining := 2400 - Length(LastPowerShellRawOutput);
  if LastPowerShellRawOutput = '' then
  begin
    LastPowerShellRawOutput := Copy(S, 1, Remaining);
  end
  else if Remaining > 2 then
  begin
    LastPowerShellRawOutput := LastPowerShellRawOutput + #13#10 +
      Copy(S, 1, Remaining - 2);
  end;
end;

procedure HandlePowerShellOutput(
  const S: String;
  const Error, FirstLine: Boolean);
var
  FailurePrefix: String;
  JsonPrefix: String;
begin
  FailurePrefix := 'BAFX_INSTALL_FAILURE: ';
  JsonPrefix := 'BAFX_INSTALL_DIAGNOSTIC_JSON: ';
  if FirstLine then
  begin
    Log('PowerShell output follows:');
  end;
  if Error then
  begin
    // Error reports output-capture failure, not a line written to stderr.
    LastPowerShellOutputError := S;
    Log('PowerShell output capture failed: ' + S);
    Exit;
  end;
  Log('  ' + S);
  RememberPowerShellRawOutput(S);
  if Pos(FailurePrefix, S) = 1 then
  begin
    LastPowerShellFailureSummary :=
      Copy(S, Length(FailurePrefix) + 1, Length(S));
  end
  else if Pos(JsonPrefix, S) = 1 then
  begin
    Log('PowerShell structured diagnostic captured.');
  end;
end;

procedure LoadPowerShellDiagnostic(const DiagnosticPath: String);
var
  I: Integer;
  Lines: TArrayOfString;
begin
  if not FileExists(DiagnosticPath) then
  begin
    Log('PowerShell diagnostic sidecar was not created: ' + DiagnosticPath);
    Exit;
  end;
  if not LoadStringsFromFile(DiagnosticPath, Lines) then
  begin
    LastPowerShellOutputError :=
      'The PowerShell diagnostic sidecar could not be read.';
    Log(LastPowerShellOutputError + ' Path: ' + DiagnosticPath);
    Exit;
  end;
  Log('PowerShell diagnostic sidecar follows: ' + DiagnosticPath);
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    HandlePowerShellOutput(Lines[I], False, I = 0);
  end;
end;

function RunPowerShell(
  const ScriptPath: String;
  const Arguments: String;
  const AsOriginalUser: Boolean;
  const DiagnosticPath: String;
  var ExitCode: Integer): Boolean;
var
  ContextName: String;
  Parameters: String;
begin
  ResetPowerShellDiagnostics;
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
  Result := False;
  Log('PowerShell [' + ContextName + '] starting: ' + ScriptPath + ' ' + Arguments);
  try
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
      Result := ExecAndLogOutput(
        ExpandConstant('{#PowerShellPath}'),
        Parameters,
        '',
        SW_SHOWNORMAL,
        ewWaitUntilTerminated,
        ExitCode,
        @HandlePowerShellOutput);
    end;
  except
    LastPowerShellOutputError := GetExceptionMessage;
    Log('PowerShell [' + ContextName + '] execution raised: ' +
      LastPowerShellOutputError);
    Result := False;
  end;
  if AsOriginalUser and (DiagnosticPath <> '') and
    (FileExists(DiagnosticPath) or not Result or (ExitCode <> 0)) then
  begin
    LoadPowerShellDiagnostic(DiagnosticPath);
  end;
  if Result then
  begin
    Log('PowerShell [' + ContextName + '] exited with code ' + IntToStr(ExitCode) +
      ': ' + ScriptPath);
  end
  else
  begin
    if ExitCode >= 0 then
    begin
      Log('PowerShell [' + ContextName + '] could not be started: ' +
        ScriptPath + '; Win32 error ' + IntToStr(ExitCode) + ': ' +
        SysErrorMessage(ExitCode));
    end
    else
    begin
      Log('PowerShell [' + ContextName + '] could not be started: ' + ScriptPath);
    end;
  end;
end;

function DecodePowerShellFailureSummary(const Value: String): String;
begin
  Result := Value;
  StringChangeEx(Result, '%3B', ';', True);
  StringChangeEx(Result, '%3D', '=', True);
  StringChangeEx(Result, '%25', '%', True);
end;

function FormatPowerShellFailure(
  const Description: String;
  const Started: Boolean;
  const ExitCode: Integer): String;
begin
  if Started then
  begin
    Result := Description + ' failed with exit code ' + IntToStr(ExitCode) + '.';
  end
  else
  begin
    Result := Description + ' could not be started.';
    if ExitCode >= 0 then
    begin
      Result := Result + ' Win32 error ' + IntToStr(ExitCode) + ': ' +
        SysErrorMessage(ExitCode) + '.';
    end;
  end;
  if LastPowerShellFailureSummary <> '' then
  begin
    Result := Result + #13#10 +
      DecodePowerShellFailureSummary(LastPowerShellFailureSummary);
  end;
  if (LastPowerShellFailureSummary = '') and
    (LastPowerShellRawOutput <> '') then
  begin
    Result := Result + #13#10 +
      'PowerShell output: ' + LastPowerShellRawOutput;
  end;
  if LastPowerShellOutputError <> '' then
  begin
    Result := Result + #13#10 +
      'Output capture detail: ' + LastPowerShellOutputError;
  end;
end;

function IncludeInstallerLog(const MessageText: String): String;
var
  LogPath: String;
begin
  Result := MessageText;
  LogPath := ExpandConstant('{log}');
  if LogPath <> '' then
  begin
    Result := Result + #13#10#13#10 + 'Detailed installer log: ' + LogPath;
  end;
end;

procedure ShowRecoveryFailure(
  const FailureText: String;
  const RecoveryText: String);
begin
  RecoveryRequired := True;
  SuppressibleMsgBox(
    IncludeInstallerLog(FailureText + #13#10#13#10 + RecoveryText),
    mbError,
    MB_OK,
    IDOK);
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
  const AsOriginalUser: Boolean;
  const DiagnosticPath: String);
var
  ExitCode: Integer;
begin
  if not RunPowerShell(
    ScriptPath,
    Arguments,
    AsOriginalUser,
    DiagnosticPath,
    ExitCode) then
  begin
    RaiseException(IncludeInstallerLog(
      FormatPowerShellFailure(Description, False, ExitCode)));
  end;
  if ExitCode <> 0 then
  begin
    RaiseException(IncludeInstallerLog(
      FormatPowerShellFailure(Description, True, ExitCode)));
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
      ' -ResultPath ' + QuoteArgument(RollbackResultPath) +
      ' -Rollback',
    True,
    RollbackResultPath + '.diagnostic.txt',
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
    '',
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
  PrimaryFailure: String;
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
  RollbackResultPath := CreateOriginalUserStatePath();

  RequirePowerShellSuccess(
    'Capturing the original user context',
    AddBackslash(InstallerRoot) + 'capture-user-context.ps1',
    '-OutputPath ' + QuoteArgument(UserContextPath),
    True,
    UserContextPath + '.diagnostic.txt');

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
    '',
    ExitCode) then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      'Preparing the machine installation', False, ExitCode);
    RaiseException(IncludeInstallerLog(PrimaryFailure));
  end;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      'Preparing the machine installation', True, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRecoveryFailure(
        PrimaryFailure,
        'Rollback also failed. The installation files and recovery state ' +
          'were retained; reopen Control Center to repair the installation.');
      Exit;
    end;
    RaiseException(IncludeInstallerLog(PrimaryFailure));
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'register-user-package.ps1',
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -ResultPath ' + QuoteArgument(RegistrationResultPath),
    True,
    RegistrationResultPath + '.diagnostic.txt',
    ExitCode) then
  begin
    LogRegistrationResult;
    PrimaryFailure := FormatPowerShellFailure(
      'Registering the package', False, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRecoveryFailure(
        PrimaryFailure,
        'Rollback also failed. The installation files and recovery state ' +
          'were retained; reopen Control Center to repair the installation.');
      Exit;
    end;
    RaiseException(IncludeInstallerLog(PrimaryFailure));
  end;
  LogRegistrationResult;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      'Registering the package', True, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRecoveryFailure(
        PrimaryFailure,
        'Rollback also failed. The installation files and recovery state ' +
          'were retained; reopen Control Center to repair the installation.');
      Exit;
    end;
    RaiseException(IncludeInstallerLog(PrimaryFailure));
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Finalize ' + CommonArguments,
    False,
    '',
    ExitCode) then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      'Finalizing the machine installation', False, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRecoveryFailure(
        PrimaryFailure,
        'Rollback also failed. The installation files and recovery state ' +
          'were retained; reopen Control Center to repair the installation.');
      Exit;
    end;
    RaiseException(IncludeInstallerLog(PrimaryFailure));
  end;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      'Finalizing the machine installation', True, ExitCode);
    if FileExists(AddBackslash(InstallerRoot) + 'INSTALL-STATE.json') and
      not FileExists(MachineStatePath) then
    begin
      ShowRecoveryFailure(
        PrimaryFailure,
        'The package was committed, but final cleanup needs another repair ' +
          'pass from Control Center.');
      Exit;
    end;
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRecoveryFailure(
        PrimaryFailure,
        'Rollback also failed. The installation files and recovery state ' +
          'were retained; reopen Control Center to repair the installation.');
      Exit;
    end;
    RaiseException(IncludeInstallerLog(PrimaryFailure));
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
  InstallStatePath: String;
  CommonArguments: String;
begin
  if CurUninstallStep <> usUninstall then
  begin
    Exit;
  end;
  InstallRoot := ExpandConstant('{app}');
  InstallerRoot := AddBackslash(InstallRoot) + 'Installer';
  MachineStatePath := AddBackslash(InstallerRoot) + 'PREPARE-STATE.json';
  InstallStatePath := AddBackslash(InstallerRoot) + 'INSTALL-STATE.json';
  UserContextPath := CreateOriginalUserStatePath();
  RegistrationResultPath := CreateOriginalUserStatePath();
  if FileExists(MachineStatePath) then
  begin
    CommonArguments :=
      '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -UserContextPath ' + QuoteArgument(UserContextPath) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -RegistrationResultPath ' + QuoteArgument(RegistrationResultPath) +
      ' -ProductVersion ' + QuoteArgument('{#ProductVersion}') +
      ' -PackageVersion ' + QuoteArgument('{#PackageVersion}');
    if not RunPowerShell(
      AddBackslash(InstallerRoot) + 'install-machine.ps1',
      '-Phase Rollback ' + CommonArguments,
      False,
      '',
      ExitCode) then
    begin
      RaiseException(IncludeInstallerLog(FormatPowerShellFailure(
        'Rolling back the pending installation', False, ExitCode)));
    end;
    if ExitCode <> 0 then
    begin
      RaiseException(IncludeInstallerLog(FormatPowerShellFailure(
        'Rolling back the pending installation', True, ExitCode)));
    end;
    if not FileExists(InstallStatePath) then
    begin
      // A first installation can fail before a committed state exists. The
      // machine rollback removed its package, certificate, and journal, so
      // Inno can now delete the copied application files directly.
      Log('Pending first installation rolled back; no committed state remains.');
      Exit;
    end;
  end;
  if not RunPowerShell(
    ExpandConstant('{app}\Installer\unregister-machine.ps1'),
    '-InstallDirectory ' + QuoteArgument(InstallRoot),
    False,
    '',
    ExitCode) then
  begin
    RaiseException(IncludeInstallerLog(FormatPowerShellFailure(
      'Removing the machine identity', False, ExitCode)));
  end;
  if ExitCode <> 0 then
  begin
    RaiseException(IncludeInstallerLog(FormatPowerShellFailure(
      'Removing the machine identity', True, ExitCode)));
  end;
end;
