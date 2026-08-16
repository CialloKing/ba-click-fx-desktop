function ConvertTo-BafxInstallerSingleLine
{
    param(
        [AllowNull()]
        [object]$Value,

        [int]$MaximumLength = 1200
    )

    $text = ([string]$Value -replace '[\r\n]+', ' ').Trim()
    if ($MaximumLength -gt 3 -and $text.Length -gt $MaximumLength)
    {
        return $text.Substring(0, $MaximumLength - 3) + '...'
    }
    return $text
}

function ConvertTo-BafxInstallerSummaryValue
{
    param(
        [AllowNull()]
        [object]$Value,

        [int]$MaximumLength = 1200
    )

    $text = ConvertTo-BafxInstallerSingleLine `
        -Value $Value `
        -MaximumLength $MaximumLength
    # The summary is a semicolon-delimited recovery aid. Escape its delimiters
    # so exception text cannot impersonate another field in support logs.
    return $text.Replace('%', '%25').Replace(';', '%3B').Replace('=', '%3D')
}

function Get-BafxInstallerHResult
{
    param(
        [AllowNull()]
        [Exception]$Exception
    )

    if ($null -eq $Exception)
    {
        return '0x00000000'
    }
    return '0x{0:X8}' -f (
        [uint32]([int64]$Exception.HResult -band [int64]0xFFFFFFFFL))
}

function New-BafxInstallerExceptionDetail
{
    param(
        [Parameter(Mandatory = $true)]
        [Exception]$Exception
    )

    return [ordered]@{
        message = ConvertTo-BafxInstallerSingleLine -Value $Exception.Message
        exceptionType = $Exception.GetType().FullName
        hresult = Get-BafxInstallerHResult -Exception $Exception
    }
}

function New-BafxInstallerErrorDetail
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [string]$Step = ''
    )

    $exception = $ErrorRecord.Exception
    $invocation = $ErrorRecord.InvocationInfo
    $innerExceptions = New-Object Collections.Generic.List[object]
    $inner = $exception.InnerException
    for ($depth = 0; $null -ne $inner -and $depth -lt 4; ++$depth)
    {
        $innerExceptions.Add((New-BafxInstallerExceptionDetail -Exception $inner))
        $inner = $inner.InnerException
    }

    $errorDetailsMessage = if ($null -eq $ErrorRecord.ErrorDetails)
    {
        ''
    }
    else
    {
        ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.ErrorDetails.Message `
            -MaximumLength 2400
    }

    return [ordered]@{
        step = $Step
        message = ConvertTo-BafxInstallerSingleLine -Value $exception.Message
        exceptionType = $exception.GetType().FullName
        hresult = Get-BafxInstallerHResult -Exception $exception
        category = [string]$ErrorRecord.CategoryInfo.Category
        categoryReason = ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.CategoryInfo.Reason
        categoryTargetName = ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.CategoryInfo.TargetName
        categoryTargetType = ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.CategoryInfo.TargetType
        fullyQualifiedErrorId = ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.FullyQualifiedErrorId
        errorDetailsMessage = $errorDetailsMessage
        targetObject = ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.TargetObject
        command = if ($null -eq $invocation.MyCommand)
        {
            ''
        }
        else
        {
            [string]$invocation.MyCommand.Name
        }
        scriptPath = [string]$invocation.ScriptName
        scriptLine = [int]$invocation.ScriptLineNumber
        offsetInLine = [int]$invocation.OffsetInLine
        positionMessage = ConvertTo-BafxInstallerSingleLine `
            -Value $invocation.PositionMessage `
            -MaximumLength 2400
        scriptStackTrace = ConvertTo-BafxInstallerSingleLine `
            -Value $ErrorRecord.ScriptStackTrace `
            -MaximumLength 4000
        innerExceptions = $innerExceptions.ToArray()
    }
}

function New-BafxInstallerRelatedFailure
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [Parameter(Mandatory = $true)]
        [string]$Step
    )

    return [pscustomobject]@{
        ErrorRecord = $ErrorRecord
        Step = $Step
    }
}

function New-BafxInstallerFailureDiagnostic
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [Parameter(Mandatory = $true)]
        [string]$Phase,

        [Parameter(Mandatory = $true)]
        [string]$Step,

        [string]$ProductVersion = '',

        [string]$PackageVersion = '',

        [object[]]$RelatedFailures = @()
    )

    $primary = New-BafxInstallerErrorDetail `
        -ErrorRecord $ErrorRecord `
        -Step $Step
    $relatedDetails = New-Object Collections.Generic.List[object]
    foreach ($related in $RelatedFailures)
    {
        if ($null -eq $related -or
            $null -eq $related.PSObject.Properties['ErrorRecord'] -or
            $related.ErrorRecord -isnot [Management.Automation.ErrorRecord])
        {
            continue
        }
        $relatedDetails.Add((New-BafxInstallerErrorDetail `
                -ErrorRecord $related.ErrorRecord `
                -Step ([string]$related.Step)))
    }

    return [ordered]@{
        schema = 1
        event = 'BAFX.InstallerFailure'
        timestampUtc = [DateTime]::UtcNow.ToString('o')
        powerShellVersion = $PSVersionTable.PSVersion.ToString()
        powerShellEdition = [string]$PSVersionTable.PSEdition
        windowsVersion = [Environment]::OSVersion.VersionString
        processArchitecture = if ([Environment]::Is64BitProcess) { 'x64' } else { 'x86' }
        uiCulture = [Globalization.CultureInfo]::CurrentUICulture.Name
        phase = $Phase
        step = $Step
        productVersion = $ProductVersion
        packageVersion = $PackageVersion
        message = $primary.message
        exceptionType = $primary.exceptionType
        hresult = $primary.hresult
        category = $primary.category
        categoryReason = $primary.categoryReason
        categoryTargetName = $primary.categoryTargetName
        categoryTargetType = $primary.categoryTargetType
        fullyQualifiedErrorId = $primary.fullyQualifiedErrorId
        errorDetailsMessage = $primary.errorDetailsMessage
        targetObject = $primary.targetObject
        command = $primary.command
        scriptPath = $primary.scriptPath
        scriptLine = $primary.scriptLine
        offsetInLine = $primary.offsetInLine
        positionMessage = $primary.positionMessage
        scriptStackTrace = $primary.scriptStackTrace
        innerExceptions = $primary.innerExceptions
        relatedErrors = $relatedDetails.ToArray()
    }
}

function Format-BafxInstallerFailureSummary
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Diagnostic
    )

    $location = if ([int]$Diagnostic.scriptLine -gt 0)
    {
        "line=$([int]$Diagnostic.scriptLine):$([int]$Diagnostic.offsetInLine)"
    }
    else
    {
        'line=unknown'
    }
    return @(
        "phase=$(ConvertTo-BafxInstallerSummaryValue -Value $Diagnostic.phase)",
        "step=$(ConvertTo-BafxInstallerSummaryValue -Value $Diagnostic.step)",
        "message=$(ConvertTo-BafxInstallerSummaryValue -Value $Diagnostic.message)",
        "type=$(ConvertTo-BafxInstallerSummaryValue -Value $Diagnostic.exceptionType)",
        "hresult=$(ConvertTo-BafxInstallerSummaryValue -Value $Diagnostic.hresult)",
        "error-id=$(ConvertTo-BafxInstallerSummaryValue -Value $Diagnostic.fullyQualifiedErrorId)",
        $location
    ) -join '; '
}

function Write-BafxInstallerFailure
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [Parameter(Mandatory = $true)]
        [string]$Phase,

        [Parameter(Mandatory = $true)]
        [string]$Step,

        [string]$ProductVersion = '',

        [string]$PackageVersion = '',

        [string]$DiagnosticPath = '',

        [object[]]$RelatedFailures = @(),

        [switch]$SuppressConsole
    )

    try
    {
        $diagnostic = New-BafxInstallerFailureDiagnostic `
            -ErrorRecord $ErrorRecord `
            -Phase $Phase `
            -Step $Step `
            -ProductVersion $ProductVersion `
            -PackageVersion $PackageVersion `
            -RelatedFailures $RelatedFailures
        $summary = Format-BafxInstallerFailureSummary -Diagnostic $diagnostic
        $json = $diagnostic | ConvertTo-Json -Depth 6 -Compress
        $lines = @(
            "BAFX_INSTALL_FAILURE: $summary",
            "BAFX_INSTALL_DIAGNOSTIC_JSON: $json"
        )
        if (-not $SuppressConsole)
        {
            foreach ($line in $lines)
            {
                [Console]::Error.WriteLine($line)
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($DiagnosticPath))
        {
            $fullPath = [IO.Path]::GetFullPath($DiagnosticPath)
            $directory = [IO.Path]::GetDirectoryName($fullPath)
            if (-not [string]::IsNullOrWhiteSpace($directory))
            {
                [IO.Directory]::CreateDirectory($directory) | Out-Null
            }
            $encoding = New-Object Text.UTF8Encoding -ArgumentList $false
            [IO.File]::WriteAllLines($fullPath, $lines, $encoding)
        }
    }
    catch
    {
        # Diagnostics are secondary; a formatter failure must not hide the
        # installation exception that the caller is already handling.
        $fallback = ConvertTo-BafxInstallerSingleLine -Value $ErrorRecord.Exception.Message
        [Console]::Error.WriteLine(
            "BAFX_INSTALL_FAILURE: phase=$Phase; step=$Step; message=$fallback")
    }
}
