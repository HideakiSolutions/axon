#Requires -Version 5.1
<#
.SYNOPSIS
    axon install — configures a project to use the axon context engine on Windows.

.DESCRIPTION
    What this installs:
      1. %USERPROFILE%\.claude\hooks\axon-guard.ps1       — PreToolUse hook (blocks Grep/Glob)
      2. %USERPROFILE%\.claude\hooks\axon-auto-index.ps1  — UserPromptSubmit hook (hourly sync)
      3. %USERPROFILE%\.claude\hooks\axon-post-edit.ps1   — PostToolUse hook (write-through)
      4. %USERPROFILE%\.claude\hooks\axon-build-guard.ps1 — PreToolUse hook (blocks high -j builds)
      5. <project>\.claude\CLAUDE.md                      — axon workflow guide
      6. <project>\.claude\settings.json                  — registers hooks for Claude Code

.PARAMETER ProjectPath
    Path to the project to configure. Defaults to current directory.

.EXAMPLE
    .\install.ps1 C:\projects\my-project
    .\install.ps1   # configures current directory
#>
param(
    [string]$ProjectPath = (Get-Location).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$AxonRoot   = Split-Path -Parent $ScriptDir
$AxonBin    = Join-Path $AxonRoot "bin\axon.exe"
$HooksDir   = Join-Path $env:USERPROFILE ".claude\hooks"
$ClaudeDir  = Join-Path $ProjectPath ".claude"
$Project    = Resolve-Path $ProjectPath

Write-Host "[axon] Installing for: $Project"

# ── Dependency check ─────────────────────────────────────────────────────────
function Require-Command([string]$Cmd, [string]$Hint) {
    if (-not (Get-Command $Cmd -ErrorAction SilentlyContinue)) {
        Write-Error "[axon] ERROR: '$Cmd' not found. $Hint"
        exit 1
    }
}

Require-Command "jq" "Install via: winget install jqlang.jq"

# ── 1. Install global hooks ───────────────────────────────────────────────────
New-Item -ItemType Directory -Force $HooksDir | Out-Null

# axon-guard.ps1 — blocks Grep and Glob in Claude Code
@'
# axon-guard: PreToolUse hook — deny Grep and Glob when axon index is present
$input_json = $input | ConvertFrom-Json
$tool = $input_json.tool_name
if ($tool -eq "Grep" -or $tool -eq "Glob") {
    $index = Join-Path (Get-Location) ".axon\index.duckdb"
    if (Test-Path $index) {
        @{ permissionDecision = "deny"; message = "[axon] Use get_context_capsule or get_skeleton instead of $tool" } | ConvertTo-Json
        exit 0
    }
}
'@ | Set-Content -Path (Join-Path $HooksDir "axon-guard.ps1") -Encoding UTF8
Write-Host "[axon] v Hook guard: $HooksDir\axon-guard.ps1"

# axon-auto-index.ps1 — triggers sync on UserPromptSubmit
@"
# axon-auto-index: UserPromptSubmit hook — touch sync-requested for hourly sweep
`$marker = Join-Path (Get-Location) ".axon\sync-requested"
`$index   = Join-Path (Get-Location) ".axon\index.duckdb"
if (Test-Path `$index) {
    `$now  = Get-Date
    `$last = if (Test-Path `$marker) { (Get-Item `$marker).LastWriteTime } else { [datetime]::MinValue }
    if ((`$now - `$last).TotalSeconds -ge 3600) { `$null | Set-Content `$marker }
}
"@ | Set-Content -Path (Join-Path $HooksDir "axon-auto-index.ps1") -Encoding UTF8
Write-Host "[axon] v Hook auto-index: $HooksDir\axon-auto-index.ps1"

# axon-post-edit.ps1 — queues edited files for write-through reindex
@"
# axon-post-edit: PostToolUse hook — append edited paths to pending-writes.txt
`$input_json = `$input | ConvertFrom-Json
`$tool = `$input_json.tool_name
`$index = Join-Path (Get-Location) ".axon\index.duckdb"
if (-not (Test-Path `$index)) { exit 0 }
if (`$tool -match "^(Write|Edit|MultiEdit|NotebookEdit|Bash)`$") {
    `$pending = Join-Path (Get-Location) ".axon\pending-writes.txt"
    `$path = `$input_json.tool_input.file_path
    if (`$path) { `$path | Add-Content `$pending }
}
"@ | Set-Content -Path (Join-Path $HooksDir "axon-post-edit.ps1") -Encoding UTF8
Write-Host "[axon] v Hook post-edit: $HooksDir\axon-post-edit.ps1"

# axon-build-guard.ps1 — blocks high-parallelism builds
@'
# axon-build-guard: PreToolUse hook — deny make/cmake/ninja with -j > 2
$input_json = $input | ConvertFrom-Json
$tool = $input_json.tool_name
if ($tool -eq "Bash") {
    $cmd = $input_json.tool_input.command
    if ($cmd -match '(?:make|cmake\s+--build|ninja)\s+.*-j\s*([3-9]|\d{2,})') {
        @{ permissionDecision = "deny"; message = "[axon] Use -j2 maximum to avoid locking the host during llama.cpp compilation." } | ConvertTo-Json
        exit 0
    }
}
'@ | Set-Content -Path (Join-Path $HooksDir "axon-build-guard.ps1") -Encoding UTF8
Write-Host "[axon] v Hook build-guard: $HooksDir\axon-build-guard.ps1"

# ── 2. Create .claude directory in project ────────────────────────────────────
New-Item -ItemType Directory -Force $ClaudeDir | Out-Null

# ── 3. Install CLAUDE.md ─────────────────────────────────────────────────────
$templateSrc = Join-Path $ScriptDir "templates\CLAUDE.md"
if (Test-Path $templateSrc) {
    Copy-Item $templateSrc (Join-Path $ClaudeDir "CLAUDE.md") -Force
    Write-Host "[axon] v CLAUDE.md: $ClaudeDir\CLAUDE.md"
}

# ── 4. Write settings.json with hooks ────────────────────────────────────────
$hookBase    = $HooksDir.Replace('\', '\\')
$settingsPath = Join-Path $ClaudeDir "settings.json"

$settings = @{
    hooks = @{
        PreToolUse = @(
            @{ matcher = "Grep"; hooks = @(@{ type = "command"; command = "powershell -File `"$hookBase\\axon-guard.ps1`"" }) }
            @{ matcher = "Glob"; hooks = @(@{ type = "command"; command = "powershell -File `"$hookBase\\axon-guard.ps1`"" }) }
            @{ matcher = "Bash"; hooks = @(@{ type = "command"; command = "powershell -File `"$hookBase\\axon-build-guard.ps1`""; timeout = 5 }) }
        )
        UserPromptSubmit = @(
            @{ matcher = ""; hooks = @(@{ type = "command"; command = "powershell -File `"$hookBase\\axon-auto-index.ps1`""; timeout = 5 }) }
        )
        PostToolUse = @(
            @{ matcher = "Write|Edit|MultiEdit|NotebookEdit|Bash"; hooks = @(@{ type = "command"; command = "powershell -File `"$hookBase\\axon-post-edit.ps1`""; timeout = 10 }) }
        )
    }
} | ConvertTo-Json -Depth 10

$settings | Set-Content -Path $settingsPath -Encoding UTF8
Write-Host "[axon] v Settings: $settingsPath"

# ── 5. Embedding model (optional) ────────────────────────────────────────────
$ModelDir   = if ($env:AXON_MODEL_DIR) { $env:AXON_MODEL_DIR } else { Join-Path $AxonRoot "models" }
$ModelName  = "nomic-embed-text-v1.5.Q4_K_M.gguf"
$ModelPath  = Join-Path $ModelDir $ModelName
$ModelUrl   = if ($env:AXON_EMBEDDING_MODEL_URL) { $env:AXON_EMBEDDING_MODEL_URL } `
              else { "https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/$ModelName" }

if (-not $env:AXON_EMBEDDING_MODEL -and -not (Test-Path $ModelPath)) {
    $download = $false
    if ($env:AXON_DOWNLOAD_MODEL -eq "1") {
        $download = $true
    } elseif ($env:AXON_DOWNLOAD_MODEL -ne "0") {
        $ans = Read-Host "[axon] Download embedding model (~150 MiB) to $ModelDir? [y/N]"
        if ($ans -match '^[Yy]') { $download = $true }
    }
    if ($download) {
        New-Item -ItemType Directory -Force $ModelDir | Out-Null
        Write-Host "[axon] Downloading $ModelName ..."
        Invoke-WebRequest -Uri $ModelUrl -OutFile $ModelPath -UseBasicParsing
        Write-Host "[axon] v Model: $ModelPath"
    } else {
        Write-Host "[axon] (skipped model — set AXON_EMBEDDING_MODEL=<path> or AXON_DOWNLOAD_MODEL=1 later)"
    }
}

# ── 6. Index the project ──────────────────────────────────────────────────────
if (Test-Path $AxonBin) {
    Write-Host "[axon] Indexing project (this may take a moment)..."
    & $AxonBin index $Project
    Write-Host "[axon] v Indexed"
} else {
    Write-Host "[axon] WARN: binary not found at $AxonBin — index manually with: axon index $Project"
}

Write-Host ""
Write-Host "[axon] Install complete. Restart Claude Code to activate the hooks."
