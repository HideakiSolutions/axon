#Requires -Version 5.1
<#
.SYNOPSIS
    axon install -- configures a project to use the axon context engine on Windows.

.DESCRIPTION
    What this installs:
      1. %USERPROFILE%\.claude\hooks\axon-guard.ps1       -- PreToolUse hook (blocks Grep/Glob)
      2. %USERPROFILE%\.claude\hooks\axon-auto-index.ps1  -- UserPromptSubmit hook (hourly sync)
      3. %USERPROFILE%\.claude\hooks\axon-post-edit.ps1   -- PostToolUse hook (write-through)
      4. %USERPROFILE%\.claude\hooks\axon-build-guard.ps1 -- PreToolUse hook (blocks high -j builds)
      5. <project>\.claude\CLAUDE.md                      -- axon workflow guide
      6. <project>\.claude\settings.json                  -- registers hooks for Claude Code

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

# Release-tarball layout: install.ps1 sits at the package root, beside bin\axon.exe.
# Source-tree layout: install.ps1 lives in scripts\, so the root is one level up.
if (Test-Path (Join-Path $ScriptDir "bin\axon.exe")) {
    $AxonRoot = $ScriptDir
} else {
    $AxonRoot = Split-Path -Parent $ScriptDir
}

# Resolve the axon binary: packaged bin\ first, then PATH (axon installed elsewhere).
$AxonBin = Join-Path $AxonRoot "bin\axon.exe"
if (-not (Test-Path $AxonBin)) {
    $onPath = Get-Command "axon.exe" -ErrorAction SilentlyContinue
    if ($onPath) { $AxonBin = $onPath.Source }
}

# Honor CLAUDE_CONFIG_DIR (Claude Code's relocatable config root) before %USERPROFILE%.
$ClaudeHome = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE ".claude" }
$HooksDir   = Join-Path $ClaudeHome "hooks"

$ClaudeDir  = Join-Path $ProjectPath ".claude"
$Project    = Resolve-Path $ProjectPath

Write-Host "[axon] Installing for: $Project"

# -- 1. Install global hooks ---------------------------------------------------
New-Item -ItemType Directory -Force $HooksDir | Out-Null

# axon-guard.ps1 -- blocks Grep and Glob in Claude Code
@'
# axon-guard: PreToolUse hook -- deny Grep and Glob when axon index is present
$input_json = $input | ConvertFrom-Json
$tool = $input_json.tool_name
if ($tool -eq "Grep" -or $tool -eq "Glob") {
    $index = Join-Path (Get-Location) ".axon\index.duckdb"
    if (Test-Path $index) {
        @{ hookSpecificOutput = @{ hookEventName = "PreToolUse"; permissionDecision = "deny"; permissionDecisionReason = "[axon] Use get_context_capsule or get_skeleton instead of $tool" } } | ConvertTo-Json -Depth 5
        exit 0
    }
}
'@ | Set-Content -Path (Join-Path $HooksDir "axon-guard.ps1") -Encoding UTF8
Write-Host "[axon] v Hook guard: $HooksDir\axon-guard.ps1"

# axon-auto-index.ps1 -- triggers sync on UserPromptSubmit
@"
# axon-auto-index: UserPromptSubmit hook -- touch sync-requested for hourly sweep
`$marker = Join-Path (Get-Location) ".axon\sync-requested"
`$index   = Join-Path (Get-Location) ".axon\index.duckdb"
if (Test-Path `$index) {
    `$now  = Get-Date
    `$last = if (Test-Path `$marker) { (Get-Item `$marker).LastWriteTime } else { [datetime]::MinValue }
    if ((`$now - `$last).TotalSeconds -ge 3600) { `$null | Set-Content `$marker }
}
"@ | Set-Content -Path (Join-Path $HooksDir "axon-auto-index.ps1") -Encoding UTF8
Write-Host "[axon] v Hook auto-index: $HooksDir\axon-auto-index.ps1"

# axon-post-edit.ps1 -- queues edited files for write-through reindex
@"
# axon-post-edit: PostToolUse hook -- append edited paths to pending-writes.txt
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

# axon-build-guard.ps1 -- blocks high-parallelism builds
@'
# axon-build-guard: PreToolUse hook -- deny make/cmake/ninja with -j > 2
$input_json = $input | ConvertFrom-Json
$tool = $input_json.tool_name
if ($tool -eq "Bash") {
    $cmd = $input_json.tool_input.command
    if ($cmd -match '(?:make|cmake\s+--build|ninja)\s+.*-j\s*([3-9]|\d{2,})') {
        @{ hookSpecificOutput = @{ hookEventName = "PreToolUse"; permissionDecision = "deny"; permissionDecisionReason = "[axon] Use -j2 maximum to avoid locking the host during llama.cpp compilation." } } | ConvertTo-Json -Depth 5
        exit 0
    }
}
'@ | Set-Content -Path (Join-Path $HooksDir "axon-build-guard.ps1") -Encoding UTF8
Write-Host "[axon] v Hook build-guard: $HooksDir\axon-build-guard.ps1"

# -- 2. Create .claude directory in project ------------------------------------
New-Item -ItemType Directory -Force $ClaudeDir | Out-Null

# -- 3. Install CLAUDE.md -----------------------------------------------------
$templateSrc = Join-Path $ScriptDir "templates\CLAUDE.md"
if (Test-Path $templateSrc) {
    Copy-Item $templateSrc (Join-Path $ClaudeDir "CLAUDE.md") -Force
    Write-Host "[axon] v CLAUDE.md: $ClaudeDir\CLAUDE.md"
}

# -- 4. Write settings.json with hooks ----------------------------------------
$settingsPath = Join-Path $ClaudeDir "settings.json"

$settings = @{
    hooks = @{
        PreToolUse = @(
            @{ matcher = "Grep"; hooks = @(@{ type = "command"; command = "powershell -NoProfile -File `"$HooksDir\axon-guard.ps1`"" }) }
            @{ matcher = "Glob"; hooks = @(@{ type = "command"; command = "powershell -NoProfile -File `"$HooksDir\axon-guard.ps1`"" }) }
            @{ matcher = "Bash"; hooks = @(@{ type = "command"; command = "powershell -NoProfile -File `"$HooksDir\axon-build-guard.ps1`""; timeout = 5 }) }
        )
        UserPromptSubmit = @(
            @{ matcher = ""; hooks = @(@{ type = "command"; command = "powershell -NoProfile -File `"$HooksDir\axon-auto-index.ps1`""; timeout = 5 }) }
        )
        PostToolUse = @(
            @{ matcher = "Write|Edit|MultiEdit|NotebookEdit|Bash"; hooks = @(@{ type = "command"; command = "powershell -NoProfile -File `"$HooksDir\axon-post-edit.ps1`""; timeout = 10 }) }
        )
    }
} | ConvertTo-Json -Depth 10

$settings | Set-Content -Path $settingsPath -Encoding UTF8
Write-Host "[axon] v Settings: $settingsPath"

# -- 5. Embedding model (optional) --------------------------------------------
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
        Write-Host "[axon] (skipped model -- set AXON_EMBEDDING_MODEL=<path> or AXON_DOWNLOAD_MODEL=1 later)"
    }
}

# -- 6. Index the project ------------------------------------------------------
if (Test-Path $AxonBin) {
    # Proactive, non-blocking: the bundled axon.exe links the VC++ 2015-2022
    # runtime (vcruntime140.dll). Warn early if the redistributable is absent,
    # but still attempt indexing -- the reactive $LASTEXITCODE check below is
    # the source of truth (registry detection can miss valid installs).
    $vcInstalled = $false
    try {
        $vcKey = "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
        $vcInstalled = ((Get-ItemProperty -Path $vcKey -ErrorAction Stop).Installed -eq 1)
    } catch { }
    if (-not $vcInstalled) {
        Write-Host "[axon] WARN: Visual C++ 2015-2022 Redistributable (x64) not detected."
        Write-Host "[axon]   axon.exe may fail to start. Install: https://aka.ms/vs/17/release/vc_redist.x64.exe"
    }

    Write-Host "[axon] Indexing project (this may take a moment)..."
    & $AxonBin index $Project
    $indexExit = $LASTEXITCODE
    if ($indexExit -ne 0) {
        Write-Host "[axon] ERROR: axon.exe index failed (exit $indexExit)."
        if ($indexExit -eq -1073741515 -or $indexExit -eq -1073741511 -or $indexExit -eq 53) {
            Write-Host "[axon]   STATUS_DLL_NOT_FOUND -- missing Visual C++ 2015-2022 Redistributable (x64):"
            Write-Host "[axon]   https://aka.ms/vs/17/release/vc_redist.x64.exe"
        }
        exit $indexExit
    }
    Write-Host "[axon] v Indexed"
} else {
    Write-Host "[axon] WARN: binary not found at $AxonBin -- index manually with: axon index $Project"
}

Write-Host ""
Write-Host "[axon] Install complete. Restart Claude Code to activate the hooks."
