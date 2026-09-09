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
AppMutex=Global\BAFX.UserInstaller.v1
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
DisableDirPage=yes
UsePreviousAppDir=no
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
; Earlier alpha installers only contained English. Re-detect on every run so
; upgrades cannot inherit that obsolete choice on a Chinese Windows install.
UsePreviousLanguage=no
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

[CustomMessages]
english.OriginalUserTempUnavailable=The original user temporary directory is unavailable.
chinesesimplified.OriginalUserTempUnavailable=无法访问原始用户的临时目录。
english.PowerShellFailedWithExitCode=%1 failed with exit code %2.
chinesesimplified.PowerShellFailedWithExitCode=%1失败，退出代码为 %2。
english.PowerShellCouldNotStart=%1 could not be started.
chinesesimplified.PowerShellCouldNotStart=无法启动“%1”操作。
english.PowerShellCouldNotStartWin32=%1 could not be started. Win32 error %2: %3.
chinesesimplified.PowerShellCouldNotStartWin32=无法启动“%1”操作。Win32 错误 %2：%3。
english.PowerShellOutput=PowerShell output: %1
chinesesimplified.PowerShellOutput=PowerShell 输出：%1
english.OutputCaptureDetail=Output capture detail: %1
chinesesimplified.OutputCaptureDetail=输出捕获详情：%1
english.DetailedInstallerLog=Detailed installer log: %1
chinesesimplified.DetailedInstallerLog=详细安装日志：%1
english.CaptureOriginalUserContext=Capturing the original user context
chinesesimplified.CaptureOriginalUserContext=捕获原始用户上下文
english.PrepareMachineInstallation=Preparing the machine installation
chinesesimplified.PrepareMachineInstallation=准备计算机级安装
english.RegisterPackage=Registering the package
chinesesimplified.RegisterPackage=注册程序包
english.FinalizeMachineInstallation=Finalizing the machine installation
chinesesimplified.FinalizeMachineInstallation=完成计算机级安装
english.RollbackPendingInstallation=Rolling back the pending installation
chinesesimplified.RollbackPendingInstallation=回滚未完成的安装
english.RemoveMachineIdentity=Removing the machine identity
chinesesimplified.RemoveMachineIdentity=移除计算机身份
english.RollbackRecovery=Rollback also failed. The installation files and recovery state were retained; reopen Control Center to repair the installation.
chinesesimplified.RollbackRecovery=回滚也失败了。安装文件和恢复状态已保留；请重新打开控制中心修复安装。
english.FinalizeRepair=The package was committed, but final cleanup needs another repair pass from Control Center.
chinesesimplified.FinalizeRepair=程序包已提交，但最终清理仍需在控制中心中再次执行修复。
english.ProtectedInstallDirectoryRequired=For security, this installer must use the protected Program Files directory: %1
chinesesimplified.ProtectedInstallDirectoryRequired=为保证安装安全，本安装器必须使用受保护的 Program Files 目录：%1

[Files]
Source: "{#StageRoot}\ba-click-fx-desktop.exe"; DestDir: "{app}\.staging\current"; Flags: ignoreversion
Source: "{#StageRoot}\BAFX.ControlCenter.exe"; DestDir: "{app}\.staging\current"; Flags: ignoreversion
Source: "{#StageRoot}\LICENSE.txt"; DestDir: "{app}\.staging\current"; Flags: ignoreversion
Source: "{#StageRoot}\SUPPORT.md"; DestDir: "{app}\.staging\current"; Flags: ignoreversion
#ifdef IncludeSpout2Notice
Source: "{#StageRoot}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}\.staging\current"; Flags: ignoreversion
#endif
  Source: "{#StageRoot}\Identity\*"; DestDir: "{app}\.staging\current\Identity"; Flags: ignoreversion recursesubdirs createallsubdirs
  Source: "{#StageRoot}\Installer\*"; DestDir: "{app}\.staging\current\Installer"; Flags: ignoreversion recursesubdirs createallsubdirs
  ; Keep the current recovery implementation available before [Files] is
  ; copied. This is required when the live/staged payload belongs to an older
  ; installer that does not understand the current journal schema.
  Source: "{#StageRoot}\Installer\installer-diagnostics.ps1"; DestDir: "{tmp}"; Flags: dontcopy
  Source: "{#StageRoot}\Installer\protected-paths.ps1"; DestDir: "{tmp}"; Flags: dontcopy
  Source: "{#StageRoot}\Installer\install-machine.ps1"; DestDir: "{tmp}"; Flags: dontcopy
  Source: "{#StageRoot}\Installer\register-user-package.ps1"; DestDir: "{tmp}"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\ba-click-fx-desktop\BAFX Control Center"; Filename: "{app}\BAFX.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\BAFX Control Center"; Filename: "{app}\BAFX.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\ba-click-fx-desktop\{cm:UninstallProgram,{#ProductName}}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\BAFX.ControlCenter.exe"; Description: "{cm:LaunchProgram,BAFX Control Center}"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent runasoriginaluser; Check: MachineInstallationCompleted

[Code]
const
  INVALID_FILE_ATTRIBUTES = $FFFFFFFF;

function GetFileAttributesW(lpFileName: String): Cardinal;
external 'GetFileAttributesW@kernel32.dll stdcall';

var
  UserContextPath: String;
  MachineStatePath: String;
  RegistrationResultPath: String;
  RollbackResultPath: String;
  PayloadRoot: String;
  InstallerRoot: String;
  CurrentRecoveryRoot: String;
  RecoveryRequired: Boolean;
  RollbackRetainedRecovery: Boolean;
  SetupFailureExitCode: Integer;
  MachineInstallationSucceeded: Boolean;
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

function CreateOriginalUserStatePath(): String; forward;
function RunPowerShell(
  const ScriptPath: String;
  const Arguments: String;
  const AsOriginalUser: Boolean;
  const DiagnosticPath: String;
  var ExitCode: Integer): Boolean; forward;
function FormatPowerShellFailure(
  const Description: String;
  const Started: Boolean;
  const ExitCode: Integer): String; forward;
function ResolveRollbackScript(
  const InstallRoot: String;
  const StagedInstallerRoot: String;
  const ScriptName: String): String; forward;
function ResolveRestoredRollbackScript(
  const InstallRoot: String;
  const ScriptName: String): String; forward;
function ExtractCurrentRecoveryScripts(const InstallRoot: String): Boolean; forward;
function ResolveCurrentRecoveryScript(const ScriptName: String): String; forward;
procedure CleanupCurrentRecoveryScripts; forward;
function SafeDeleteFile(const Path: String): Boolean; forward;
function SafeDeleteTree(const Path: String): Boolean; forward;
procedure RaiseInstallerFailure(
  const FailureText: String;
  const ExitCode: Integer); forward;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  InstallRoot: String;
  ProtectedRoot: String;
  ExistingInstallerRoot: String;
  ExistingPendingPath: String;
  ExistingScript: String;
  ExistingRegisterScript: String;
  RecoveryArguments: String;
  CleanupArguments: String;
  CommittedStatePresent: Boolean;
  ExitCode: Integer;
begin
  // A failure before a pending journal exists can be safely retried after the
  // staged input is removed, so report the completed-rollback code.
  SetupFailureExitCode := 1002;
  Log('PrepareToInstall entered.');
  // QuoteArgument cannot safely terminate a quoted Windows path with '\\':
  // the runtime parser treats the final slash as escaping the closing quote.
  // Keep the root canonical without a trailing slash, and add separators only
  // when constructing child paths.
  InstallRoot := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  ProtectedRoot := RemoveBackslashUnlessRoot(
    ExpandConstant('{autopf}\ba-click-fx-desktop'));
  if CompareText(InstallRoot, ProtectedRoot) <> 0 then
  begin
    // Machine signing executes privileged payloads from {app}. Reject a
    // command-line directory override before any files reach that directory.
    Result := FmtMessage(
      CustomMessage('ProtectedInstallDirectoryRequired'), [ProtectedRoot]);
    Exit;
  end;

  PayloadRoot := AddBackslash(InstallRoot) + '.staging\current';
  InstallerRoot := AddBackslash(PayloadRoot) + 'Installer';
  ExistingInstallerRoot := AddBackslash(InstallRoot) + 'Installer\';
  ExistingPendingPath := ExistingInstallerRoot + 'PREPARE-STATE.json';
  // A first installation has no previous state or live recovery scripts. Keep
  // that distinction through rollback so the coordinator does not attempt to
  // restore a package that never existed.
  CommittedStatePresent :=
    FileExists(ExistingInstallerRoot + 'INSTALL-STATE.json') or
    FileExists(ExistingInstallerRoot + 'INSTALL-STATE.json.bak');
  Log('PrepareToInstall pending path: ' + ExistingPendingPath);
  if FileExists(ExistingPendingPath) then
  begin
    Log('PrepareToInstall found a pending transaction.');
  end
  else
  begin
    Log('PrepareToInstall found no pending transaction.');
  end;
  if FileExists(ExistingPendingPath) and
    (not ExtractCurrentRecoveryScripts(InstallRoot)) then
  begin
    SetupFailureExitCode := 1001;
    Result := 'The current recovery scripts could not be prepared.';
    Exit;
  end;
  ExistingScript := ResolveCurrentRecoveryScript('install-machine.ps1');
  if ExistingScript = '' then
  begin
    ExistingScript := ResolveRollbackScript(
      InstallRoot,
      InstallerRoot,
      'install-machine.ps1');
  end;
  ExistingRegisterScript := ResolveCurrentRecoveryScript(
    'register-user-package.ps1');
  if ExistingRegisterScript = '' then
  begin
    ExistingRegisterScript := ResolveRollbackScript(
      InstallRoot,
      InstallerRoot,
      'register-user-package.ps1');
  end;
  if FileExists(ExistingPendingPath) then
  begin
    if not FileExists(ExistingScript) then
    begin
      Result := 'A pending installation exists but its recovery script is missing.';
      Exit;
    end;
    if not FileExists(ExistingRegisterScript) then
    begin
      Result := 'A pending installation exists but its user-package recovery script is missing.';
      Exit;
    end;
    // Recover an interrupted transaction before Inno copies the next payload.
    // The persistent Installer directory belongs to the previous release; the
    // staging directory is intentionally left intact until this succeeds.
    UserContextPath := CreateOriginalUserStatePath();
    RegistrationResultPath := CreateOriginalUserStatePath();
    MachineStatePath := ExistingPendingPath;
    RecoveryArguments :=
      '-Phase Rollback' +
      ' -InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
      ' -UserContextPath ' + QuoteArgument(UserContextPath) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -RegistrationResultPath ' + QuoteArgument(RegistrationResultPath) +
      ' -ProductVersion ' + QuoteArgument('{#ProductVersion}') +
      ' -PackageVersion ' + QuoteArgument('{#PackageVersion}');
    if not RunPowerShell(
      ExistingRegisterScript,
      '-InstallDirectory ' + QuoteArgument(InstallRoot) +
        ' -MachineStatePath ' + QuoteArgument(ExistingPendingPath) +
        ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
        ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
        ' -RollbackAction RemoveNew',
      True,
      RegistrationResultPath + '.diagnostic.txt',
      ExitCode) then
    begin
      SetupFailureExitCode := 1001;
      Result := FormatPowerShellFailure(
        CustomMessage('RollbackPendingInstallation'), False, ExitCode);
      Exit;
    end;
    if ExitCode <> 0 then
    begin
      SetupFailureExitCode := 1001;
      Result := FormatPowerShellFailure(
        CustomMessage('RollbackPendingInstallation'), True, ExitCode);
      Exit;
    end;
    if not RunPowerShell(
      ExistingScript,
      RecoveryArguments,
      False,
      '',
      ExitCode) then
    begin
      SetupFailureExitCode := 1001;
      Result := FormatPowerShellFailure(
        CustomMessage('RollbackPendingInstallation'), False, ExitCode);
      Exit;
    end;
    if ExitCode <> 0 then
    begin
      SetupFailureExitCode := 1001;
      Result := FormatPowerShellFailure(
        CustomMessage('RollbackPendingInstallation'), True, ExitCode);
      Exit;
    end;
    if FileExists(ExistingPendingPath) and CommittedStatePresent then
    begin
      ExistingRegisterScript := ResolveRestoredRollbackScript(
        InstallRoot,
        'register-user-package.ps1');
      if ExistingRegisterScript = '' then
      begin
        SetupFailureExitCode := 1001;
        Result := 'The restored user-package recovery script is missing.';
        Exit;
      end;
      if not RunPowerShell(
        ExistingRegisterScript,
        '-InstallDirectory ' + QuoteArgument(InstallRoot) +
          ' -MachineStatePath ' + QuoteArgument(ExistingPendingPath) +
          ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
          ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
          ' -RollbackAction RestorePrevious',
        True,
        RegistrationResultPath + '.diagnostic.txt',
        ExitCode) then
      begin
        SetupFailureExitCode := 1001;
        Result := FormatPowerShellFailure(
          CustomMessage('RollbackPendingInstallation'), False, ExitCode);
        Exit;
      end;
      if ExitCode <> 0 then
      begin
        SetupFailureExitCode := 1001;
        Result := FormatPowerShellFailure(
          CustomMessage('RollbackPendingInstallation'), True, ExitCode);
        Exit;
      end;
    end;
    if FileExists(ExistingPendingPath) then
    begin
      ExistingScript := ResolveRestoredRollbackScript(
        InstallRoot,
        'install-machine.ps1');
      if (ExistingScript = '') and (not CommittedStatePresent) then
      begin
        // Before the first commit, the staged transaction script is the only
        // recovery code that can remove the new certificate and package.
        ExistingScript := ResolveRollbackScript(
          InstallRoot,
          InstallerRoot,
          'install-machine.ps1');
      end;
      if ExistingScript = '' then
      begin
        SetupFailureExitCode := 1001;
        Result := 'The pending cleanup script is missing; recovery state was retained.';
        Exit;
      end;
      CleanupArguments := RecoveryArguments;
      StringChangeEx(CleanupArguments, '-Phase Rollback',
        '-Phase RollbackCleanup', True);
      if not RunPowerShell(
        ExistingScript,
        CleanupArguments,
        False,
        '',
        ExitCode) then
      begin
        SetupFailureExitCode := 1001;
        Result := FormatPowerShellFailure(
          CustomMessage('RollbackPendingInstallation'), False, ExitCode);
        Exit;
      end;
      if ExitCode <> 0 then
      begin
        SetupFailureExitCode := 1001;
        Result := FormatPowerShellFailure(
          CustomMessage('RollbackPendingInstallation'), True, ExitCode);
        Exit;
      end;
    end;
    if FileExists(ExistingPendingPath) then
    begin
      SetupFailureExitCode := 1001;
      Result := 'The recovered pending installation journal was retained for repair.';
      Exit;
    end;
  end;
  if not SafeDeleteTree(PayloadRoot) then
  begin
    SetupFailureExitCode := 1001;
    Result := 'The previous installer staging directory could not be removed.';
    Exit;
  end;
  SetupFailureExitCode := 0;
  Result := '';
end;

function MachineInstallationCompleted(): Boolean;
begin
  Result := MachineInstallationSucceeded;
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
    RaiseInstallerFailure(CustomMessage('OriginalUserTempUnavailable'), 1002);
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

function IsReparsePointPath(const Path: String): Boolean;
var
  Attributes: Cardinal;
begin
  Result := False;
  if not FileOrDirExists(Path) then
  begin
    Exit;
  end;
  // FindFirst treats a bare filesystem root (for example C:\) as a search
  // pattern. GetFileAttributesW reads the node itself, including roots, so a
  // failed lookup can remain fail-closed without confusing the root with a
  // reparse point.
  Attributes := GetFileAttributesW(Path);
  if Attributes = INVALID_FILE_ATTRIBUTES then
  begin
    // An inaccessible path is not safe to delete blindly. Fail closed so a
    // junction cannot redirect DelTree outside the protected install root.
    Log('Could not inspect installer deletion path: ' + Path);
    Result := True;
    Exit;
  end;
  Result := (Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0;
end;

function AssertNoReparsePointPath(const Path: String): Boolean;
var
  CurrentPath: String;
  ParentPath: String;
begin
  Result := False;
  CurrentPath := RemoveBackslashUnlessRoot(Path);
  if CurrentPath = '' then
  begin
    Exit;
  end;
  while CurrentPath <> '' do
  begin
    if FileOrDirExists(CurrentPath) and IsReparsePointPath(CurrentPath) then
    begin
      Exit;
    end;
    ParentPath := RemoveBackslashUnlessRoot(ExtractFileDir(CurrentPath));
    if (ParentPath = '') or SameText(ParentPath, CurrentPath) then
    begin
      Break;
    end;
    CurrentPath := ParentPath;
  end;
  Result := True;
end;

function AssertNoReparsePointTree(const Path: String): Boolean;
var
  FindRec: TFindRec;
  ChildPath: String;
begin
  Result := False;
  if not AssertNoReparsePointPath(Path) then
  begin
    Exit;
  end;
  if not FileOrDirExists(Path) then
  begin
    Result := True;
    Exit;
  end;
  if not FindFirst(AddBackslash(Path) + '*', FindRec) then
  begin
    // FindFirst normally exposes "." and ".." even for an empty directory;
    // a false result therefore means that the tree could not be inspected.
    Log('Could not enumerate installer deletion tree: ' + Path);
    Exit;
  end;
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        ChildPath := AddBackslash(Path) + FindRec.Name;
        if (FindRec.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0 then
        begin
          Log('Installer deletion tree contains a reparse point: ' + ChildPath);
          Exit;
        end;
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          if not AssertNoReparsePointTree(ChildPath) then
          begin
            Exit;
          end;
        end;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
  Result := True;
end;

function SafeDeleteTree(const Path: String): Boolean;
begin
  if not FileOrDirExists(Path) then
  begin
    Result := True;
    Exit;
  end;
  if not AssertNoReparsePointTree(Path) then
  begin
    Log('Refusing to delete an installer tree containing a reparse point: ' +
      Path);
    Result := False;
    Exit;
  end;
  Result := DelTree(Path, True, True, True);
end;

function SafeDeleteFile(const Path: String): Boolean;
begin
  if not FileOrDirExists(Path) then
  begin
    Result := True;
    Exit;
  end;
  if not AssertNoReparsePointPath(Path) then
  begin
    Log('Refusing to delete an installer file behind a reparse point: ' + Path);
    Result := False;
    Exit;
  end;
  if not DeleteFile(Path) then
  begin
    Result := False;
    Exit;
  end;
  Result := not FileOrDirExists(Path);
end;

procedure CleanupUncommittedInstallArtifacts;
var
  InstallRoot: String;
  PendingPath: String;
  StagingRoot: String;
begin
  // A failed first install can stop before a journal is created. In that case
  // the staged payload is only installer input and must not be left behind.
  // Once a journal remains, it is recovery evidence and is deliberately kept.
  if RecoveryRequired then
  begin
    Exit;
  end;
  InstallRoot := AddBackslash(ExpandConstant('{app}'));
  PendingPath := InstallRoot + 'Installer\PREPARE-STATE.json';
  if FileExists(PendingPath) then
  begin
    Exit;
  end;
  StagingRoot := InstallRoot + '.staging\current';
  if not SafeDeleteTree(StagingRoot) then
  begin
    Log('The uncommitted installer staging directory could not be removed: ' +
      StagingRoot);
    // Retain a nonzero result when cleanup itself is blocked. Otherwise Inno
    // would report a successful install while protected staged files remain.
    RecoveryRequired := True;
    SetupFailureExitCode := 1001;
  end;
end;

function CleanupFirstInstallPayload(const InstallRoot: String): Boolean;
var
  Index: Integer;
  Path: String;
begin
  // There is no committed state to tell the uninstaller which files belong to
  // this product. This path is used only after a pending first-install
  // transaction has been rolled back successfully; never touch the user's
  // data directory here.
  Result := True;
  for Index := 0 to 4 do
  begin
    case Index of
      0: Path := AddBackslash(InstallRoot) + 'ba-click-fx-desktop.exe';
      1: Path := AddBackslash(InstallRoot) + 'BAFX.ControlCenter.exe';
      2: Path := AddBackslash(InstallRoot) + 'LICENSE.txt';
      3: Path := AddBackslash(InstallRoot) + 'SUPPORT.md';
      4: Path := AddBackslash(InstallRoot) + 'THIRD-PARTY-NOTICES.txt';
    end;
    if not SafeDeleteFile(Path) then
    begin
      Result := False;
    end;
  end;
  if not SafeDeleteTree(AddBackslash(InstallRoot) + 'Identity') then
  begin
    Result := False;
  end;
  if not SafeDeleteTree(AddBackslash(InstallRoot) + 'Installer') then
  begin
    Result := False;
  end;
  if not SafeDeleteTree(AddBackslash(InstallRoot) + '.rollback') then
  begin
    Result := False;
  end;
  if not SafeDeleteTree(AddBackslash(InstallRoot) + '.staging') then
  begin
    Result := False;
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
    Result := FmtMessage(
      CustomMessage('PowerShellFailedWithExitCode'), [Description, IntToStr(ExitCode)]);
  end
  else
  begin
    if ExitCode >= 0 then
    begin
      Result := FmtMessage(
        CustomMessage('PowerShellCouldNotStartWin32'), [
          Description, IntToStr(ExitCode), SysErrorMessage(ExitCode)]);
    end
    else
    begin
      Result := FmtMessage(
        CustomMessage('PowerShellCouldNotStart'), [Description]);
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
      FmtMessage(CustomMessage('PowerShellOutput'), [LastPowerShellRawOutput]);
  end;
  if LastPowerShellOutputError <> '' then
  begin
    Result := Result + #13#10 +
      FmtMessage(CustomMessage('OutputCaptureDetail'), [LastPowerShellOutputError]);
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
    Result := Result + #13#10#13#10 +
      FmtMessage(CustomMessage('DetailedInstallerLog'), [LogPath]);
  end;
end;

procedure ShowRecoveryFailure(
  const FailureText: String;
  const RecoveryText: String);
begin
  // The rollback evidence is still present. A caller must repair this
  // transaction before another install can safely run.
  RecoveryRequired := True;
  SetupFailureExitCode := 1001;
  SuppressibleMsgBox(
    IncludeInstallerLog(FailureText + #13#10#13#10 + RecoveryText),
    mbError,
    MB_OK,
    IDOK);
end;

procedure ShowRetainedRecovery(
  const FailureText: String;
  const RecoveryText: String);
begin
  RecoveryRequired := True;
  SetupFailureExitCode := 1001;
  SuppressibleMsgBox(
    IncludeInstallerLog(FailureText + #13#10#13#10 + RecoveryText),
    mbError,
    MB_OK,
    IDOK);
end;

procedure RaiseInstallerFailure(
  const FailureText: String;
  const ExitCode: Integer);
begin
  SetupFailureExitCode := ExitCode;
  if ExitCode = 1001 then
  begin
    RecoveryRequired := True;
  end;
  // Keep the fatal notification, but make the custom exit code explicit so a
  // RaiseException path can never be reported as a successful install.
  RaiseException(IncludeInstallerLog(FailureText));
end;

procedure ShowRollbackOutcome(
  const FailureText: String);
begin
  if RollbackRetainedRecovery then
  begin
    ShowRetainedRecovery(
      FailureText,
      CustomMessage('FinalizeRepair'));
  end
  else
  begin
    ShowRecoveryFailure(
      FailureText,
      CustomMessage('RollbackRecovery'));
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
    RaiseInstallerFailure(
      FormatPowerShellFailure(Description, False, ExitCode),
      1002);
  end;
  if ExitCode <> 0 then
  begin
    RaiseInstallerFailure(
      FormatPowerShellFailure(Description, True, ExitCode),
      1002);
  end;
end;

procedure RunBestEffortRollback(
  const InstallRoot: String;
  const InstallerRoot: String;
  const CommonArguments: String;
  var Succeeded: Boolean);
var
  ExitCode: Integer;
  ScriptPath: String;
  CommittedStatePresent: Boolean;
begin
  Succeeded := True;
  RollbackRetainedRecovery := False;
  // A first install has no live recovery scripts. Remember whether a committed
  // state pair existed before rollback so that this case can use the staged
  // transaction script without weakening upgrade recovery checks.
  CommittedStatePresent :=
    FileExists(AddBackslash(InstallRoot) + 'Installer\INSTALL-STATE.json') or
    FileExists(AddBackslash(InstallRoot) + 'Installer\INSTALL-STATE.json.bak');
  if not FileExists(MachineStatePath) then
  begin
    Exit;
  end;

  ScriptPath := ResolveRollbackScript(
    InstallRoot,
    InstallerRoot,
    'register-user-package.ps1');
  if ScriptPath = '' then
  begin
    Succeeded := False;
  end
  else if not RunPowerShell(
    ScriptPath,
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -ResultPath ' + QuoteArgument(RollbackResultPath) +
      ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
      ' -RollbackAction RemoveNew',
    True,
    RollbackResultPath + '.diagnostic.txt',
    ExitCode) then
  begin
    Succeeded := False;
  end
  else if ExitCode <> 0 then
  begin
    Succeeded := False;
    RollbackRetainedRecovery := ExitCode = 1001;
  end;

  if not Succeeded then
  begin
    Exit;
  end;

  ScriptPath := ResolveRollbackScript(
    InstallRoot,
    InstallerRoot,
    'install-machine.ps1');
  if ScriptPath = '' then
  begin
    Succeeded := False;
  end
  else if not RunPowerShell(
    ScriptPath,
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
    RollbackRetainedRecovery := ExitCode = 1001;
  end;

  if not Succeeded then
  begin
    Exit;
  end;

  if FileExists(MachineStatePath) and CommittedStatePresent then
  begin
    ScriptPath := ResolveRestoredRollbackScript(
      InstallRoot,
      'register-user-package.ps1');
    if ScriptPath = '' then
    begin
      Succeeded := False;
    end
    else if not RunPowerShell(
      ScriptPath,
      '-InstallDirectory ' + QuoteArgument(InstallRoot) +
        ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
        ' -ResultPath ' + QuoteArgument(RollbackResultPath) +
        ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
        ' -RollbackAction RestorePrevious',
      True,
      RollbackResultPath + '.diagnostic.txt',
      ExitCode) then
    begin
      Succeeded := False;
    end
    else if ExitCode <> 0 then
    begin
      Succeeded := False;
      RollbackRetainedRecovery := ExitCode = 1001;
    end;
  end;

  if not Succeeded then
  begin
    Exit;
  end;

  if FileExists(MachineStatePath) then
  begin
    ScriptPath := ResolveRestoredRollbackScript(
      InstallRoot,
      'install-machine.ps1');
    if (ScriptPath = '') and (not CommittedStatePresent) then
    begin
      // Before the first commit, the staged script is the only recovery code;
      // use it to remove the pending package and certificate after rollback.
      ScriptPath := ResolveRollbackScript(
        InstallRoot,
        InstallerRoot,
        'install-machine.ps1');
    end;
    if ScriptPath = '' then
    begin
      Succeeded := False;
      RollbackRetainedRecovery := True;
      Exit;
    end;
    if not RunPowerShell(
      ScriptPath,
      '-Phase RollbackCleanup ' + CommonArguments,
      False,
      '',
      ExitCode) then
    begin
      Succeeded := False;
      RollbackRetainedRecovery := True;
      Exit;
    end;
    if ExitCode <> 0 then
    begin
      Succeeded := False;
      RollbackRetainedRecovery := ExitCode = 1001;
      Exit;
    end;
  end;

  if FileExists(MachineStatePath) then
  begin
    Succeeded := False;
    RollbackRetainedRecovery := True;
  end;
end;

function ResolveRollbackScript(
  const InstallRoot: String;
  const StagedInstallerRoot: String;
  const ScriptName: String): String;
var
  LivePath: String;
  StagedPath: String;
  CurrentPath: String;
begin
  // The embedded current script is available before [Files] is copied. It is
  // the only implementation guaranteed to understand this installer's journal.
  CurrentPath := ResolveCurrentRecoveryScript(ScriptName);
  if CurrentPath <> '' then
  begin
    Result := CurrentPath;
    Exit;
  end;
  // The staged directory is captured by the pending transaction and therefore
  // carries the recovery code that understands its schema. Prefer it across
  // release boundaries; the live copy is only a fallback for old installs.
  StagedPath := AddBackslash(StagedInstallerRoot) + ScriptName;
  if FileExists(StagedPath) then
  begin
    Result := StagedPath;
    Exit;
  end;
  LivePath := AddBackslash(AddBackslash(InstallRoot) + 'Installer') + ScriptName;
  if FileExists(LivePath) then
  begin
    Result := LivePath;
  end
  else
  begin
    Result := '';
  end;
end;

function ResolveRestoredRollbackScript(
  const InstallRoot: String;
  const ScriptName: String): String;
var
  LivePath: String;
begin
  if CurrentRecoveryRoot <> '' then
  begin
    LivePath := AddBackslash(CurrentRecoveryRoot) + ScriptName;
    if FileExists(LivePath) then
    begin
      Result := LivePath;
      Exit;
    end;
  end;
  // Rollback restores the previous Installer directory before the old user
  // package is registered again. Never fall back to the still-staged newer
  // scripts after that point because their recovery schema may differ.
  LivePath := AddBackslash(AddBackslash(InstallRoot) + 'Installer') + ScriptName;
  if FileExists(LivePath) then
  begin
    Result := LivePath;
  end
  else
  begin
    Result := '';
  end;
end;

function ResolveCurrentRecoveryScript(const ScriptName: String): String;
var
  Candidate: String;
begin
  Result := '';
  if CurrentRecoveryRoot = '' then
  begin
    Exit;
  end;
  Candidate := AddBackslash(CurrentRecoveryRoot) + ScriptName;
  if FileExists(Candidate) then
  begin
    Result := Candidate;
  end;
end;

function ExtractCurrentRecoveryScripts(const InstallRoot: String): Boolean;
var
  TempRoot: String;
  RecoveryRoot: String;
  SourcePath: String;
  DestinationPath: String;
begin
  Result := False;
  TempRoot := AddBackslash(ExpandConstant('{tmp}'));
  try
    ExtractTemporaryFile('installer-diagnostics.ps1');
    ExtractTemporaryFile('protected-paths.ps1');
    ExtractTemporaryFile('install-machine.ps1');
    ExtractTemporaryFile('register-user-package.ps1');
  except
    Log('Could not extract the current recovery scripts: ' +
      GetExceptionMessage());
    Exit;
  end;
  RecoveryRoot := AddBackslash(InstallRoot) + '.recovery-current';
  if not SafeDeleteTree(RecoveryRoot) then
  begin
    Log('Could not clear the previous current recovery directory: ' +
      RecoveryRoot);
    Exit;
  end;
  if not ForceDirectories(RecoveryRoot) then
  begin
    Log('Could not create the current recovery directory: ' + RecoveryRoot);
    Exit;
  end;
  if not AssertNoReparsePointPath(RecoveryRoot) then
  begin
    Log('The current recovery directory contains a reparse point: ' +
      RecoveryRoot);
    Exit;
  end;
  CurrentRecoveryRoot := AddBackslash(RecoveryRoot);
  SourcePath := TempRoot + 'installer-diagnostics.ps1';
  DestinationPath := CurrentRecoveryRoot + 'installer-diagnostics.ps1';
  if not CopyFile(SourcePath, DestinationPath, False) then
  begin
    Exit;
  end;
  SourcePath := TempRoot + 'protected-paths.ps1';
  DestinationPath := CurrentRecoveryRoot + 'protected-paths.ps1';
  if not CopyFile(SourcePath, DestinationPath, False) then
  begin
    Exit;
  end;
  SourcePath := TempRoot + 'install-machine.ps1';
  DestinationPath := CurrentRecoveryRoot + 'install-machine.ps1';
  if not CopyFile(SourcePath, DestinationPath, False) then
  begin
    Exit;
  end;
  SourcePath := TempRoot + 'register-user-package.ps1';
  DestinationPath := CurrentRecoveryRoot + 'register-user-package.ps1';
  if not CopyFile(SourcePath, DestinationPath, False) then
  begin
    Exit;
  end;
  if not AssertNoReparsePointPath(CurrentRecoveryRoot) then
  begin
    Log('The copied current recovery scripts are not in a protected tree.');
    Exit;
  end;
  if (ResolveCurrentRecoveryScript('installer-diagnostics.ps1') = '') or
    (ResolveCurrentRecoveryScript('protected-paths.ps1') = '') or
    (ResolveCurrentRecoveryScript('install-machine.ps1') = '') or
    (ResolveCurrentRecoveryScript('register-user-package.ps1') = '') then
  begin
    Log('The extracted recovery script set is incomplete.');
    CurrentRecoveryRoot := '';
    Exit;
  end;
  Result := True;
end;

procedure CleanupCurrentRecoveryScripts;
begin
  if CurrentRecoveryRoot = '' then
  begin
    Exit;
  end;
  if not SafeDeleteTree(CurrentRecoveryRoot) then
  begin
    Log('The current recovery directory could not be removed: ' +
      CurrentRecoveryRoot);
    RecoveryRequired := True;
    SetupFailureExitCode := 1001;
  end;
  CurrentRecoveryRoot := '';
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

  SetupFailureExitCode := 1002;

  InstallRoot := ExpandConstant('{app}');
  PayloadRoot := AddBackslash(InstallRoot) + '.staging\current';
  InstallerRoot := AddBackslash(PayloadRoot) + 'Installer';
  UserContextPath := CreateOriginalUserStatePath();
  MachineStatePath := AddBackslash(InstallRoot) + 'Installer\PREPARE-STATE.json';
  RegistrationResultPath := CreateOriginalUserStatePath();
  RollbackResultPath := CreateOriginalUserStatePath();

  RequirePowerShellSuccess(
    CustomMessage('CaptureOriginalUserContext'),
    AddBackslash(InstallerRoot) + 'capture-user-context.ps1',
    '-OutputPath ' + QuoteArgument(UserContextPath),
    True,
    UserContextPath + '.diagnostic.txt');

  CommonArguments :=
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
    ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
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
      CustomMessage('PrepareMachineInstallation'), False, ExitCode);
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('PrepareMachineInstallation'), True, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase CommitFiles ' + CommonArguments,
    False,
    '',
    ExitCode) then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('FinalizeMachineInstallation'), False, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('FinalizeMachineInstallation'), True, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'register-user-package.ps1',
    '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -ResultPath ' + QuoteArgument(RegistrationResultPath),
    True,
    RegistrationResultPath + '.diagnostic.txt',
    ExitCode) then
  begin
    LogRegistrationResult;
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('RegisterPackage'), False, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;
  LogRegistrationResult;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('RegisterPackage'), True, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;

  if not RunPowerShell(
    AddBackslash(InstallerRoot) + 'install-machine.ps1',
    '-Phase Finalize ' + CommonArguments,
    False,
    '',
    ExitCode) then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('FinalizeMachineInstallation'), False, ExitCode);
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;
  if ExitCode <> 0 then
  begin
    PrimaryFailure := FormatPowerShellFailure(
      CustomMessage('FinalizeMachineInstallation'), True, ExitCode);
    if ExitCode = 1001 then
    begin
      ShowRetainedRecovery(
        PrimaryFailure,
        CustomMessage('FinalizeRepair'));
      Exit;
    end;
    RunBestEffortRollback(
      InstallRoot,
      InstallerRoot,
      CommonArguments,
      RollbackSucceeded);
    if not RollbackSucceeded then
    begin
      ShowRollbackOutcome(PrimaryFailure);
      Exit;
    end;
    RaiseInstallerFailure(PrimaryFailure, 1002);
  end;
  SetupFailureExitCode := 0;
  MachineInstallationSucceeded := True;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if RecoveryRequired then
  begin
    Result := 1001;
  end
  else if SetupFailureExitCode <> 0 then
  begin
    Result := SetupFailureExitCode;
  end;
end;

procedure DeinitializeSetup;
begin
  CleanupUncommittedInstallArtifacts;
  CleanupCurrentRecoveryScripts;
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
  StagedInstallerRoot: String;
  InstallStatePath: String;
  UninstallCompleteMarkerPath: String;
  CommonArguments: String;
  ScriptPath: String;
begin
  if CurUninstallStep <> usUninstall then
  begin
    Exit;
  end;
  InstallRoot := ExpandConstant('{app}');
  SetupFailureExitCode := 1001;
  PayloadRoot := AddBackslash(InstallRoot) + '.staging\current';
  InstallerRoot := AddBackslash(InstallRoot) + 'Installer';
  StagedInstallerRoot := AddBackslash(PayloadRoot) + 'Installer';
  MachineStatePath := AddBackslash(InstallerRoot) + 'PREPARE-STATE.json';
  InstallStatePath := AddBackslash(InstallerRoot) + 'INSTALL-STATE.json';
  UninstallCompleteMarkerPath :=
    AddBackslash(InstallerRoot) + 'UNINSTALL-COMPLETE.json';
  UserContextPath := CreateOriginalUserStatePath();
  RegistrationResultPath := CreateOriginalUserStatePath();
  if FileExists(MachineStatePath) then
  begin
    CommonArguments :=
      '-InstallDirectory ' + QuoteArgument(InstallRoot) +
      ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
      ' -UserContextPath ' + QuoteArgument(UserContextPath) +
      ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
      ' -RegistrationResultPath ' + QuoteArgument(RegistrationResultPath) +
      ' -ProductVersion ' + QuoteArgument('{#ProductVersion}') +
      ' -PackageVersion ' + QuoteArgument('{#PackageVersion}');
    ScriptPath := ResolveRollbackScript(
      InstallRoot,
      StagedInstallerRoot,
      'register-user-package.ps1');
    if ScriptPath = '' then
    begin
      ShowRecoveryFailure(
        'A pending installation exists but its user-package recovery script is missing.',
        CustomMessage('RollbackRecovery'));
      Exit;
    end;
    if not RunPowerShell(
      ScriptPath,
      '-InstallDirectory ' + QuoteArgument(InstallRoot) +
        ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
        ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
        ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
        ' -RollbackAction RemoveNew',
      True,
      RegistrationResultPath + '.diagnostic.txt',
      ExitCode) then
    begin
      ShowRecoveryFailure(
        FormatPowerShellFailure(
          CustomMessage('RollbackPendingInstallation'), False, ExitCode),
        CustomMessage('RollbackRecovery'));
      Exit;
    end;
    if ExitCode <> 0 then
    begin
      if ExitCode = 1001 then
      begin
        ShowRetainedRecovery(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), True, ExitCode),
          CustomMessage('FinalizeRepair'));
      end
      else
      begin
        ShowRecoveryFailure(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), True, ExitCode),
          CustomMessage('RollbackRecovery'));
      end;
      Exit;
    end;
    ScriptPath := ResolveRollbackScript(
      InstallRoot,
      StagedInstallerRoot,
      'install-machine.ps1');
    if ScriptPath = '' then
    begin
      ShowRecoveryFailure(
        'A pending installation exists but its machine recovery script is missing.',
        CustomMessage('RollbackRecovery'));
      Exit;
    end;
    if not RunPowerShell(
      ScriptPath,
      '-Phase Rollback ' + CommonArguments,
      False,
      '',
      ExitCode) then
    begin
      ShowRecoveryFailure(
        FormatPowerShellFailure(
          CustomMessage('RollbackPendingInstallation'), False, ExitCode),
        CustomMessage('RollbackRecovery'));
      Exit;
    end;
    if ExitCode <> 0 then
    begin
      if ExitCode = 1001 then
      begin
        ShowRetainedRecovery(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), True, ExitCode),
          CustomMessage('FinalizeRepair'));
      end
      else
      begin
        ShowRecoveryFailure(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), True, ExitCode),
          CustomMessage('RollbackRecovery'));
      end;
      Exit;
    end;
    if FileExists(MachineStatePath) then
    begin
      ScriptPath := ResolveRestoredRollbackScript(
        InstallRoot,
        'register-user-package.ps1');
      if ScriptPath = '' then
      begin
        ShowRecoveryFailure(
          'The restored user-package recovery script is missing.',
          CustomMessage('RollbackRecovery'));
        Exit;
      end;
      if not RunPowerShell(
        ScriptPath,
        '-InstallDirectory ' + QuoteArgument(InstallRoot) +
          ' -MachineStatePath ' + QuoteArgument(MachineStatePath) +
          ' -ResultPath ' + QuoteArgument(RegistrationResultPath) +
          ' -PayloadDirectory ' + QuoteArgument(PayloadRoot) +
          ' -RollbackAction RestorePrevious',
        True,
        RegistrationResultPath + '.diagnostic.txt',
        ExitCode) then
      begin
        ShowRecoveryFailure(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), False, ExitCode),
          CustomMessage('RollbackRecovery'));
        Exit;
      end;
      if ExitCode <> 0 then
      begin
        ShowRecoveryFailure(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), True, ExitCode),
          CustomMessage('RollbackRecovery'));
        Exit;
      end;
    end;
    if FileExists(MachineStatePath) then
    begin
      // RollbackCleanup owns package, certificate, staging, and journal
      // deletion. Keeping that operation in the machine script makes a
      // cleanup failure recoverable instead of silently discarding evidence.
      ScriptPath := ResolveRestoredRollbackScript(
        InstallRoot,
        'install-machine.ps1');
      if ScriptPath = '' then
      begin
        ShowRetainedRecovery(
          'The pending cleanup script is missing; recovery state was retained.',
          CustomMessage('FinalizeRepair'));
        Exit;
      end;
      if not RunPowerShell(
        ScriptPath,
        '-Phase RollbackCleanup ' + CommonArguments,
        False,
        '',
        ExitCode) then
      begin
        ShowRetainedRecovery(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), False, ExitCode),
          CustomMessage('FinalizeRepair'));
        Exit;
      end;
      if ExitCode <> 0 then
      begin
        ShowRetainedRecovery(
          FormatPowerShellFailure(
            CustomMessage('RollbackPendingInstallation'), True, ExitCode),
          CustomMessage('FinalizeRepair'));
        Exit;
      end;
    end;
    if FileExists(MachineStatePath) then
    begin
      ShowRetainedRecovery(
        'The pending rollback journal was retained for repair.',
        CustomMessage('FinalizeRepair'));
      Exit;
    end;
    if not FileExists(InstallStatePath) then
    begin
      // A first installation can fail before a committed state exists. The
      // machine rollback and cleanup removed its package and certificate, so
      // Inno can now delete only the copied application payload.
      Log('Pending first installation rolled back; no committed state remains.');
      if not CleanupFirstInstallPayload(InstallRoot) then
      begin
        RaiseInstallerFailure(
          'The rolled-back first-install payload could not be removed safely.',
          1001);
      end;
      SetupFailureExitCode := 0;
      Exit;
    end;
  end;
  if (not FileExists(InstallStatePath)) and
    (not FileExists(MachineStatePath)) and
    (not DirExists(InstallerRoot)) then
  begin
    // Prepare can fail before the persistent recovery scripts are copied. In
    // that case there is no committed identity to unregister; remove only the
    // known installer payload and leave the user's data directory untouched.
    Log('No committed install state or recovery directory remains; cleaning first-install payload.');
    if not CleanupFirstInstallPayload(InstallRoot) then
    begin
      RaiseInstallerFailure(
        'The incomplete first-install payload could not be removed safely.',
        1001);
    end;
    SetupFailureExitCode := 0;
    Exit;
  end;
  if not RunPowerShell(
    ExpandConstant('{app}\Installer\unregister-machine.ps1'),
    '-InstallDirectory ' + QuoteArgument(InstallRoot),
    False,
    '',
    ExitCode) then
  begin
    RaiseInstallerFailure(
      FormatPowerShellFailure(
        CustomMessage('RemoveMachineIdentity'), False, ExitCode),
      1001);
  end;
  if ExitCode <> 0 then
  begin
    RaiseInstallerFailure(
      FormatPowerShellFailure(
        CustomMessage('RemoveMachineIdentity'), True, ExitCode),
      1001);
  end;
  if not FileExists(UninstallCompleteMarkerPath) then
  begin
    RaiseInstallerFailure(
      'The uninstall script completed without its protected completion marker.',
      1001);
  end;
  if not SafeDeleteTree(InstallerRoot) then
  begin
    RaiseInstallerFailure(
      'The installer recovery directory could not be removed after uninstall.',
      1001);
  end;
  SetupFailureExitCode := 0;
end;
