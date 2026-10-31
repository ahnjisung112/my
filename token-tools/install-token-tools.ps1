# Installs the token-saving tools for Claude Code on Windows.
# Run this in PowerShell (not as administrator):
#   powershell -ExecutionPolicy Bypass -File .\install-token-tools.ps1
$ErrorActionPreference = "Stop"

function Have($cmd) { [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

Write-Host "== 0. Prerequisites ==" -ForegroundColor Cyan
if (-not (Have "node")) {
    Write-Host "Installing Node.js LTS (required by claude-mem and Ponytail hooks)..."
    winget install -e --id OpenJS.NodeJS.LTS --accept-source-agreements --accept-package-agreements
}
if (-not (Have "uv")) {
    Write-Host "Installing uv (required by Serena)..."
    powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
    $env:Path = "$env:USERPROFILE\.local\bin;$env:Path"
}
if (-not (Have "claude")) {
    Write-Warning "The 'claude' CLI was not found. Install Claude Code first, then run this script again."
    exit 1
}

Write-Host "== 1. Serena (MCP server) ==" -ForegroundColor Cyan
uv tool install -p 3.13 serena-agent
$env:Path = "$env:USERPROFILE\.local\bin;$env:Path"
serena init
claude mcp add --scope user serena -- serena start-mcp-server --context claude-code --project-from-cwd

Write-Host "== 2. Headroom (desktop tray app) ==" -ForegroundColor Cyan
$rel = Invoke-RestMethod "https://api.github.com/repos/gglucass/headroom-desktop/releases/latest"
$asset = $rel.assets | Where-Object { $_.name -like "*x64-setup.exe" } | Select-Object -First 1
if ($asset) {
    $out = Join-Path $env:TEMP $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $out
    Write-Host "Launching Headroom installer: $($asset.name)"
    Start-Process $out -Wait
} else {
    Write-Warning "No Windows installer found. Download it manually: https://github.com/gglucass/headroom-desktop/releases/latest"
}

Write-Host ""
Write-Host "== Steps left to do by hand ==" -ForegroundColor Yellow
Write-Host "3. claude-mem and 4. Ponytail: open Claude Code and enter these slash commands one at a time:"
Write-Host "     /plugin marketplace add thedotmack/claude-mem"
Write-Host "     /plugin install claude-mem"
Write-Host "     /plugin marketplace add DietrichGebert/ponytail"
Write-Host "     /plugin install ponytail@ponytail"
Write-Host "   Then open /hooks, approve the hooks, and restart Claude Code."
Write-Host "5. UsageScope (paid app): buy and download the Windows version at https://www.usagescope.com/"
Write-Host "Check: run /mcp in Claude Code and confirm serena shows as connected."
