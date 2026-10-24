param(
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [ValidateSet('x64','arm64')][string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
# src/vi_tokens.h comes from design/vi/tokens.json; a token error fails the build here.
& (Join-Path $PSScriptRoot 'gen-vi-tokens.ps1')
$localRoot = Join-Path $PSScriptRoot 'local'
$compilerRoot = Join-Path $localRoot 'msvc\VC\Tools\MSVC\14.44.35207'
$sdkRoot = Join-Path $localRoot 'sdk\c'
$sdkLibRoot = Join-Path $localRoot "sdk-$Architecture\c"
$sdkVersion = '10.0.26100.0'
# Hostx64\<target>\cl.exe is the x64-hosted cross compiler. rc.exe stays the x64 host tool; resources are not architecture-specific.
$compiler = Join-Path $compilerRoot "bin\Hostx64\$Architecture\cl.exe"
$resourceCompiler = Join-Path $sdkRoot "bin\$sdkVersion\x64\rc.exe"
if (-not (Test-Path -LiteralPath $compiler) -or -not (Test-Path -LiteralPath $resourceCompiler)) {
    throw 'Local compiler/SDK not found. Run tools/bootstrap-toolchain.ps1, or build CMakeLists.txt from a Visual Studio C++ developer prompt.'
}
# x64 keeps build\Release so the installed-package checks stay pointed at the runnable edition.
$buildRoot = if ($Architecture -eq 'arm64') { Join-Path $projectRoot "build\arm64\$Configuration" } else { Join-Path $projectRoot "build\$Configuration" }
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$expectedMachine = if ($Architecture -eq 'arm64') { [uint16]0xAA64 } else { [uint16]0x8664 }
# Keep this an array. Assigning a one-element array unwraps it, and splatting that string would pass each character to the linker.
$machineLink = @()
if ($Architecture -eq 'arm64') { $machineLink += '/MACHINE:ARM64' }
# ARM64 images cannot be executed on this x64 development host.
$executeTests = $Architecture -eq 'x64'
$savedInclude=$env:INCLUDE; $savedLib=$env:LIB; $savedPath=$env:PATH

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 64
        if ($stream.Read($header, 0, 64) -ne 64) { throw "Executable header is truncated: $Path" }
        $peOffset = [BitConverter]::ToInt32($header, 60)
        if ($peOffset -lt 64) { throw "Executable PE offset is invalid: $Path" }
        [void]$stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin)
        $pe = New-Object byte[] 6
        if ($stream.Read($pe, 0, 6) -ne 6) { throw "Executable PE header is truncated: $Path" }
        if ([System.Text.Encoding]::ASCII.GetString($pe, 0, 2) -ne 'PE') { throw "Executable is not a PE image: $Path" }
        return [BitConverter]::ToUInt16($pe, 4)
    } finally { $stream.Dispose() }
}

function Assert-PeMachine([string]$Path) {
    $machine = Get-PeMachine $Path
    if ($machine -ne $expectedMachine) {
        throw ("{0} is machine 0x{1:X4}; the {2} build requires 0x{3:X4}." -f $Path, $machine, $Architecture, $expectedMachine)
    }
}

function Invoke-CompiledTest([string]$Name) {
    $exe = Join-Path $buildRoot $Name
    Assert-PeMachine $exe
    if (-not $executeTests) {
        Write-Output "Compiled $Architecture test (not executed on this host): $exe"
        return
    }
    & ".\$Name"
    if ($LASTEXITCODE -ne 0) { throw "$Name failed." }
}

try {
    $env:INCLUDE = (@((Join-Path $compilerRoot 'include')) + @('ucrt','shared','um','winrt','cppwinrt') | ForEach-Object {
        if ([System.IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $sdkRoot "Include\$sdkVersion\$_" }
    }) -join ';'
    $env:LIB = @((Join-Path $compilerRoot "lib\$Architecture"),(Join-Path $sdkLibRoot "um\$Architecture"),(Join-Path $sdkLibRoot "ucrt\$Architecture")) -join ';'
    $env:PATH = "$(Split-Path $compiler);$(Split-Path $resourceCompiler);$savedPath"
    Push-Location (Join-Path $projectRoot 'packaging')
    try { & $resourceCompiler /nologo "/fo$buildRoot\app.res" app.rc; if($LASTEXITCODE -ne 0) {throw 'Resource compilation failed.'} }
    finally { Pop-Location }
    $flags=@('/nologo','/std:c++20','/utf-8','/EHsc','/MT','/W4','/DUNICODE','/D_UNICODE','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/D_WIN32_WINNT=0x0A00',"/I$projectRoot\src")
    if($Configuration -eq 'Debug') { $flags+=@('/Od','/Zi') } else { $flags+=@('/O2','/DNDEBUG') }
    $sources=@('main.cpp','appearance.cpp','renderer.cpp','settings.cpp','localization.cpp','iconography.cpp','osd.cpp','behavior.cpp','presets.cpp','official_icons.cpp','local_launch.cpp','installed_apps.cpp','artwork.cpp','acrylic.cpp','test_input.cpp','tray_menu.cpp') | ForEach-Object {Join-Path $projectRoot "src\$_"}
    $libraries=@('d3d11.lib','d2d1.lib','dwrite.lib','dxgi.lib','dxguid.lib','dwmapi.lib','windowsapp.lib','CoreMessaging.lib','user32.lib','gdi32.lib','shell32.lib','ole32.lib','oleaut32.lib','propsys.lib','shlwapi.lib','comdlg32.lib','comctl32.lib','advapi32.lib','wtsapi32.lib','windowscodecs.lib','uuid.lib','imm32.lib','uxtheme.lib','shcore.lib')
    Push-Location $buildRoot
    try {
        $buildAssets=Join-Path $buildRoot 'assets'
        New-Item -ItemType Directory -Path $buildAssets -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot 'assets\brands') -Destination $buildAssets -Recurse -Force
        & $compiler @flags @sources app.res /Fe:CopilotReel.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO @machineLink @libraries
        if($LASTEXITCODE -ne 0) {throw 'Application compilation failed.'}
        Assert-PeMachine (Join-Path $buildRoot 'CopilotReel.exe')
        & $compiler @flags "$projectRoot\tests\core_tests.cpp" "$projectRoot\src\localization.cpp" "$projectRoot\src\settings.cpp" /Fe:core_tests.exe /link @machineLink user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Core tests compilation failed.'}
        Invoke-CompiledTest 'core_tests.exe'
        & $compiler @flags "$projectRoot\tests\settings_tests.cpp" "$projectRoot\src\localization.cpp" /Fe:settings_tests.exe /link @machineLink user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Settings tests compilation failed.'}
        Invoke-CompiledTest 'settings_tests.exe'
        & $compiler @flags "$projectRoot\tests\behavior_tests.cpp" "$projectRoot\src\behavior.cpp" "$projectRoot\src\settings.cpp" "$projectRoot\src\localization.cpp" /Fe:behavior_tests.exe /link @machineLink user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Behavior tests compilation failed.'}
        Invoke-CompiledTest 'behavior_tests.exe'
        & $compiler @flags "$projectRoot\tests\iconography_tests.cpp" "$projectRoot\src\iconography.cpp" "$projectRoot\src\official_icons.cpp" "$projectRoot\src\artwork.cpp" /Fe:iconography_tests.exe /link @machineLink d3d11.lib dxgi.lib d2d1.lib dwrite.lib windowsapp.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib shlwapi.lib windowscodecs.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Iconography tests compilation failed.'}
        Invoke-CompiledTest 'iconography_tests.exe'
        & $compiler @flags "$projectRoot\tests\local_launch_tests.cpp" "$projectRoot\src\local_launch.cpp" /Fe:local_launch_tests.exe /link @machineLink user32.lib shell32.lib ole32.lib windowsapp.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Local launch tests compilation failed.'}
        Invoke-CompiledTest 'local_launch_tests.exe'
        & $compiler @flags "$projectRoot\tests\installed_apps_tests.cpp" "$projectRoot\src\installed_apps.cpp" "$projectRoot\src\local_launch.cpp" /Fe:installed_apps_tests.exe /link @machineLink user32.lib gdi32.lib shell32.lib ole32.lib propsys.lib windowsapp.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Installed apps tests compilation failed.'}
        Invoke-CompiledTest 'installed_apps_tests.exe'
        & $compiler @flags "$projectRoot\tests\display_names_tests.cpp" "$projectRoot\src\iconography.cpp" "$projectRoot\src\behavior.cpp" "$projectRoot\src\localization.cpp" "$projectRoot\src\settings.cpp" /Fe:display_names_tests.exe /link @machineLink d2d1.lib windowsapp.lib user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib
        if($LASTEXITCODE -ne 0) {throw 'Display name tests compilation failed.'}
        Invoke-CompiledTest 'display_names_tests.exe'
        & $compiler @flags "$projectRoot\tests\test_input_tests.cpp" "$projectRoot\src\test_input.cpp" /Fe:test_input_tests.exe /link @machineLink user32.lib
        if($LASTEXITCODE -ne 0) {throw 'Test input seam tests compilation failed.'}
        Invoke-CompiledTest 'test_input_tests.exe'
        & $compiler @flags "$projectRoot\tests\launch_probe.cpp" /Fe:launch_probe.exe /link /SUBSYSTEM:WINDOWS @machineLink user32.lib
        if($LASTEXITCODE -ne 0) {throw 'Launch probe compilation failed.'}
        Assert-PeMachine (Join-Path $buildRoot 'launch_probe.exe')
    } finally { Pop-Location }
    $machine = Get-PeMachine (Join-Path $buildRoot 'CopilotReel.exe')
    Write-Output ("Built {0} machine 0x{1:X4}: {2}\CopilotReel.exe" -f $Architecture, $machine, $buildRoot)
    if (-not $executeTests) {
        Write-Output 'ARM64 executables were cross-compiled from the x64 host and were not run.'
    }
} finally { $env:INCLUDE=$savedInclude; $env:LIB=$savedLib; $env:PATH=$savedPath }
