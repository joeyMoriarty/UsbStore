# Runs idf.py against the VS Code extension's ESP-IDF install, which export.ps1
# cannot set up (its venv lives at tools\python\v6.1\venv, not python_env\).
#
#   .\tools\idf.ps1 build
#   .\tools\idf.ps1 -p COMx flash
#
# The paths below match a default VS Code ESP-IDF extension install of v6.1
# (framework in C:\esp\v6.1, tools in C:\Espressif). Edit them for yours.
#
# Plain $args, deliberately: a param() block with [Parameter] makes this an
# advanced script, and PowerShell then swallows idf.py's own -p as one of its
# common parameters.
$IdfArgs = $args

$env:IDF_PATH                    = "C:\esp\v6.1\esp-idf"
$env:IDF_TOOLS_PATH              = "C:\Espressif"
$env:IDF_PYTHON_ENV_PATH         = "C:\Espressif\tools\python\v6.1\venv"
$env:IDF_PYTHON_CHECK_CONSTRAINTS = "no"
$env:ESP_IDF_VERSION             = "6.1"
$rom = Get-ChildItem "C:\Espressif\tools\esp-rom-elfs\*" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($rom) { $env:ESP_ROM_ELF_DIR = $rom.FullName + "\" }
$py = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"

$dirs  = @("C:\Espressif\tools\python\v6.1\venv\Scripts")
$dirs += (Get-ChildItem "C:\Espressif\tools\cmake\*\bin" -Directory -ErrorAction SilentlyContinue).FullName
$dirs += (Get-ChildItem "C:\Espressif\tools\ninja\*" -Directory -ErrorAction SilentlyContinue).FullName
$dirs += (Get-ChildItem "C:\Espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin" -Directory -ErrorAction SilentlyContinue).FullName
$dirs += (Get-ChildItem "C:\Espressif\tools\ccache" -Recurse -Filter ccache.exe -ErrorAction SilentlyContinue | Select-Object -First 1).DirectoryName
$env:PATH = (($dirs | Where-Object { $_ }) -join ';') + ';' + $env:PATH

# -C from the script's own location, so the project can be moved or cloned.
& $py "$env:IDF_PATH\tools\idf.py" -C (Split-Path $PSScriptRoot -Parent) @IdfArgs 2>&1
exit $LASTEXITCODE
