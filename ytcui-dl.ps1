$ErrorActionPreference = 'Stop'
$translatedPath = & wsl.exe --exec wslpath -a ($PSScriptRoot.Replace('\', '/'))
if ($LASTEXITCODE -or !$translatedPath) { throw 'WSL is required. Build ytcui-dl inside your WSL distribution first.' }
$linuxProject = $translatedPath.Trim()
& wsl.exe --cd $linuxProject --exec ./build/ytcui-dl @args
exit $LASTEXITCODE
