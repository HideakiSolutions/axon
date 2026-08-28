$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$hook = Join-Path $root "scripts\hooks\axon-post-edit.ps1"
$temp = Join-Path ([IO.Path]::GetTempPath()) ("axon-queue-test-" + [Guid]::NewGuid().ToString("N"))
$project = Join-Path $temp "project with space"
$foreign = Join-Path $temp "foreign"
New-Item -ItemType Directory -Force (Join-Path $project ".axon"), $foreign | Out-Null
$null | Set-Content (Join-Path $project ".axon\index.duckdb")
$owned = Join-Path $project "owned unicode ü.md"
$outside = Join-Path $foreign "foreign.md"
$null | Set-Content $owned
$null | Set-Content $outside

function Invoke-QueueHook([hashtable]$Payload, [string]$Label) {
    $stdin = Join-Path $temp ("hook-stdin-{0}.json" -f [Guid]::NewGuid().ToString("N"))
    try {
        $json = $Payload | ConvertTo-Json -Compress
        [IO.File]::WriteAllText($stdin, $json, (New-Object Text.UTF8Encoding($false)))
        $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$hook`"", "-AxonBin", "missing-axon.exe")
        $process = Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -RedirectStandardInput $stdin -NoNewWindow -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "$Label hook failed with exit $($process.ExitCode)" }
    } finally {
        Remove-Item $stdin -Force -ErrorAction SilentlyContinue
    }
}

try {
    $env:AXON_DISABLE_QUEUE_DRAIN = "1"
    Push-Location $project
    try {
        Invoke-QueueHook @{ tool_name = "Write"; tool_input = @{ file_path = $owned } } "owned-path"
        Invoke-QueueHook @{ tool_name = "Write"; tool_input = @{ file_path = $outside } } "foreign-path"
    } finally {
        Pop-Location
    }
    $queued = @([IO.File]::ReadAllLines((Join-Path $project ".axon\pending-writes.txt")))
    if ($queued.Count -ne 1 -or $queued[0] -ne $owned) { throw "PowerShell queue boundary or Unicode path failed" }
    Write-Host "windows_queue_hooks_ok=true"
} finally {
    Remove-Item Env:AXON_DISABLE_QUEUE_DRAIN -ErrorAction SilentlyContinue
    Remove-Item $temp -Recurse -Force
}
