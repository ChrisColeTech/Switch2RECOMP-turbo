# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
param(
    [string]$QtPrefix = $env:SWR_QT_PREFIX,
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [int]$Jobs = 8,
    [string]$DeployDir
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
try {
    if (-not $QtPrefix) {
        $candidates = @('C:\Qt\6.8.3\msvc2022_64', 'C:\Qt\6.9.3\msvc2022_64')
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath (Join-Path $candidate 'lib\cmake\Qt6\Qt6Config.cmake')) { $QtPrefix = $candidate; break }
        }
    }
    if (-not $QtPrefix -or -not (Test-Path -LiteralPath (Join-Path $QtPrefix 'bin\windeployqt.exe'))) {
        throw 'Qt 6 MSVC x64 is required. Pass -QtPrefix C:\Qt\<version>\msvc2022_64 or set SWR_QT_PREFIX.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $QtPrefix 'plugins\imageformats\qwebp.dll'))) {
        throw 'Install the Qt Image Formats module for this Qt version (qtimageformats); WebP cover/background support is required.'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'The MSVC x64 toolset is not installed.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $python = & cmd.exe /d /c "call `"$PSScriptRoot\_python.bat`" && call echo %SWR_PYTHON%"
    if ($LASTEXITCODE) { throw 'Python selection failed.' }
    $python = $python.Trim()
    $scripts = & $python -c "import sysconfig; print(sysconfig.get_path('scripts'))"
    if ($LASTEXITCODE) { throw 'Cannot locate Python build tools.' }
    $cmake = Join-Path $scripts 'cmake.exe'
    $ninja = Join-Path $scripts 'ninja.exe'
    $ctest = Join-Path $scripts 'ctest.exe'
    foreach ($tool in @($cmake, $ninja, $ctest)) {
        if (-not (Test-Path -LiteralPath $tool)) { throw "Missing $tool. Install cmake==3.31.10 ninja==1.13.2 in $python explicitly." }
    }
    $build = Join-Path $root "build\launcher-$($Config.ToLower())"
    $logs = Join-Path $root 'logs'
    $deploy = if ($DeployDir) { [IO.Path]::GetFullPath($DeployDir) } else { Join-Path $root 'bin\launcher' }
    [IO.Directory]::CreateDirectory($logs) | Out-Null
    [IO.Directory]::CreateDirectory($deploy) | Out-Null
    $oldPath = $env:PATH
    $env:PATH = "$QtPrefix\bin;$env:PATH"
    $env:VSCMD_SKIP_SENDTELEMETRY = '1'
    $log = Join-Path $logs "launcher_$($Config.ToLower()).log"
    $commands = @(
        "call `"$vcvars`" >nul",
        "`"$cmake`" -S `"$root\launcher`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_PREFIX_PATH=`"$QtPrefix`" -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=`"$build\bin`"",
        "`"$cmake`" --build `"$build`" --parallel $Jobs",
        "`"$ctest`" --test-dir `"$build`" --output-on-failure"
    )
    & cmd.exe /d /c ("(" + ($commands -join ' && ') + ") > `"$log`" 2>&1")
    if ($LASTEXITCODE) { throw "Build/tests failed. See $log" }
    Copy-Item -LiteralPath "$build\bin\SwitchRecompLauncher.exe" -Destination $deploy -Force
    $deployCommand = "call `"$vcvars`" >nul && `"$QtPrefix\bin\windeployqt.exe`" --$($Config.ToLower()) --no-translations --no-system-d3d-compiler --no-opengl-sw `"$deploy\SwitchRecompLauncher.exe`""
    & cmd.exe /d /c ($deployCommand + " >> `"$log`" 2>&1")
    if ($LASTEXITCODE) { throw "Qt deployment failed. See $log" }
    $redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64\Microsoft.VC143.CRT') } | Select-Object -First 1
    if (-not $redist) { throw 'MSVC app-local runtime DLLs are missing.' }
    Copy-Item -Path (Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT\*.dll') -Destination $deploy -Force
    Copy-Item -LiteralPath (Join-Path $QtPrefix 'plugins\platforms\qoffscreen.dll') -Destination (Join-Path $deploy 'platforms') -Force
    if (-not (Test-Path -LiteralPath (Join-Path $deploy 'imageformats\qwebp.dll'))) { throw 'Qt WebP plugin was not deployed.' }
    "[LAUNCHER] SUCCESS - $deploy\SwitchRecompLauncher.exe"
    "[LOG] $log"
} catch {
    Write-Error $_
    exit 1
} finally {
    if ($oldPath) { $env:PATH = $oldPath }
}
