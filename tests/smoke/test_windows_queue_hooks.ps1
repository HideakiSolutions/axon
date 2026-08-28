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

try {
    $env:AXON_DISABLE_QUEUE_DRAIN = "1"
    Push-Location $project
    try {
        @{ tool_name = "Write"; tool_input = @{ file_path = $owned } } |
            ConvertTo-Json -Compress | powershell.exe -NoProfile -ExecutionPolicy Bypass -File $hook -AxonBin "missing-axon.exe"
        @{ tool_name = "Write"; tool_input = @{ file_path = $outside } } |
            ConvertTo-Json -Compress | powershell.exe -NoProfile -ExecutionPolicy Bypass -File $hook -AxonBin "missing-axon.exe"
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
