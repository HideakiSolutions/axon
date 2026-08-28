param([string]$AxonBin = "axon.exe")

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$payload = [Console]::In.ReadToEnd()
if (-not $payload) { exit 0 }
$inputJson = $payload | ConvertFrom-Json
$tool = [string]$inputJson.tool_name
$root = [IO.Path]::GetFullPath((Get-Location).Path).TrimEnd('\', '/')
$axonDir = Join-Path $root ".axon"
$index = Join-Path $axonDir "index.duckdb"
if (-not (Test-Path $index)) { exit 0 }

$queue = Join-Path $axonDir "pending-writes.txt"
$syncMarker = Join-Path $axonDir "sync-requested"
$queued = $false
if ($tool -eq "Bash") {
    $null | Set-Content $syncMarker
} elseif ($tool -match "^(Write|Edit|MultiEdit|NotebookEdit)$") {
    $candidate = if ($tool -eq "NotebookEdit") { $inputJson.tool_input.notebook_path } else { $inputJson.tool_input.file_path }
    if ($candidate) {
        $full = if ([IO.Path]::IsPathRooted([string]$candidate)) {
            [IO.Path]::GetFullPath([string]$candidate)
        } else {
            [IO.Path]::GetFullPath((Join-Path $root ([string]$candidate)))
        }
        $prefix = $root + [IO.Path]::DirectorySeparatorChar
        if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $hash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root)))).Replace("-", "")
            } finally {
                $sha.Dispose()
            }
            $mutex = New-Object Threading.Mutex($false, "Local\AxonPendingWrites-$hash")
            $acquired = $false
            try {
                try { $acquired = $mutex.WaitOne(5000) } catch [Threading.AbandonedMutexException] { $acquired = $true }
                if ($acquired) {
                    if ((Test-Path $queue) -and (Get-Item $queue).Length -gt 1MB) {
                        Move-Item $queue (Join-Path $axonDir ("pending-writes.{0}.bak" -f [DateTimeOffset]::UtcNow.ToUnixTimeSeconds())) -Force
                        $null | Set-Content $syncMarker
                    }
                    [IO.File]::AppendAllText($queue, $full + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
                    $queued = $true
                } else {
                    $null | Set-Content $syncMarker
                }
            } finally {
                if ($acquired) { $mutex.ReleaseMutex() }
                $mutex.Dispose()
            }
        }
    }
}

if (($queued -or $tool -eq "Bash") -and $env:AXON_DISABLE_QUEUE_DRAIN -ne "1") {
    $drain = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "axon-queue-drain.ps1"
    if (Test-Path $drain) {
        $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$drain`"", "-ProjectRoot", "`"$root`"", "-AxonBin", "`"$AxonBin`"")
        Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -WindowStyle Hidden | Out-Null
    }
}
