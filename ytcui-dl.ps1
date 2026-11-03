$ErrorActionPreference = 'Stop'
$linuxProject = (& wsl.exe -- wslpath -a $PSScriptRoot).Trim()
if ($LASTEXITCODE) { throw 'WSL is required. Build ytcui-dl inside your WSL distribution first.' }
& wsl.exe --cd $linuxProject -- ./build/ytcui-dl @args
exit $LASTEXITCODE
