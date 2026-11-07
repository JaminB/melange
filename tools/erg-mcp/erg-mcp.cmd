@echo off
rem erg-mcp: the Erg MCP server (stdio) on the pinned portable Node. See docs\erg-mcp.md.
rem Builds dist\erg-mcp.mjs first when it is missing (build output goes to stderr: stdout is the protocol).
setlocal
set "HERE=%~dp0"
set "NODE=%HERE%..\node\node.exe"
if not exist "%NODE%" (
  echo erg-mcp: %NODE% is missing; run scripts\web\fetch.ps1 first 1>&2
  exit /b 1
)
if not exist "%HERE%dist\erg-mcp.mjs" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%build.ps1" -SkipTypeCheck 1>&2
  if errorlevel 1 exit /b 1
)
"%NODE%" "%HERE%dist\erg-mcp.mjs" %*
