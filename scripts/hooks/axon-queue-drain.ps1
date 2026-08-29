param(
    [string]$ProjectRoot = (Get-Location).Path,
    [string]$AxonBin = "axon.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = [IO.Path]::GetFullPath($ProjectRoot)
$axonDir = Join-Path $root ".axon"
$queue = Join-Path $axonDir "pending-writes.txt"
$index = Join-Path $axonDir "index.duckdb"
if (-not (Test-Path $index) -or -not (Test-Path $queue)) { exit 0 }

$lines = @([IO.File]::ReadAllLines($queue) | Where-Object { $_.Trim().Length -gt 0 })
if ($lines.Count -eq 0) { exit 0 }
$maxAge = if ($env:AXON_QUEUE_MAX_AGE_SECONDS) { [int]$env:AXON_QUEUE_MAX_AGE_SECONDS } else { 900 }
$maxLines = if ($env:AXON_QUEUE_MAX_LINES) { [int]$env:AXON_QUEUE_MAX_LINES } else { 100 }
$maxAttempts = if ($env:AXON_QUEUE_MAX_ATTEMPTS) { [int]$env:AXON_QUEUE_MAX_ATTEMPTS } else { 3 }
$attemptTimeoutSeconds = if ($env:AXON_QUEUE_ATTEMPT_TIMEOUT_SECONDS) { [Math]::Max(1, [int]$env:AXON_QUEUE_ATTEMPT_TIMEOUT_SECONDS) } else { 30 }
$age = [int]((Get-Date).ToUniversalTime() - (Get-Item $queue).LastWriteTimeUtc).TotalSeconds
if ($lines.Count -lt $maxLines -and $age -lt $maxAge) { exit 0 }

$sha = [Security.Cryptography.SHA256]::Create()
try {
    $hash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root)))).Replace("-", "")
} finally {
    $sha.Dispose()
}
$mutex = New-Object Threading.Mutex($false, "Local\AxonQueueDrain-$hash")
$acquired = $false
try {
    try { $acquired = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) { exit 0 }

    $request = '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_overview","arguments":{"limit":1}}}'
    for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $AxonBin
        $psi.Arguments = "serve"
        $psi.WorkingDirectory = $root
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        try {
            $process = New-Object Diagnostics.Process
            $process.StartInfo = $psi
            if (-not $process.Start()) { throw "failed to start axon serve" }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $length = [Text.Encoding]::UTF8.GetByteCount($request)
            $process.StandardInput.Write("Content-Length: $length`r`n`r`n$request")
            $process.StandardInput.Close()
            if (-not $process.WaitForExit($attemptTimeoutSeconds * 1000)) {
                $process.Kill()
                $process.WaitForExit()
            }
            $null = $stdout.Result
            $null = $stderr.Result
            $process.Dispose()
        } catch {
            # Bounded fallback: retry below; the synchronous MCP drain remains
            # the source of truth on the next client tool call.
        }
        $remaining = if (Test-Path $queue) {
            @([IO.File]::ReadAllLines($queue) | Where-Object { $_.Trim().Length -gt 0 }).Count
        } else { 0 }
        if ($remaining -eq 0) { exit 0 }
        Start-Sleep -Seconds $attempt
    }
    exit 2
} finally {
    if ($acquired) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
