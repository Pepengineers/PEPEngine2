param([string]$VisualStudioDirectory, [string]$SourceDirectory)
$ErrorActionPreference = 'Stop'

function Test-JoltDebugProject([string]$Path) {
    [xml]$project = [IO.File]::ReadAllText($Path)
    $debug = @($project.Project.ItemDefinitionGroup | Where-Object { $_.Condition -match "'Debug\|x64'" })
    if ($debug.Count -ne 1) { throw "Jolt project has no unique Debug|x64 configuration: $Path" }
    $compile = $debug[0].ClCompile
    if ($compile.Optimization -ne 'MaxSpeed' -or $compile.InlineFunctionExpansion -ne 'AnySuitable' -or
        $compile.RuntimeLibrary -ne 'MultiThreadedDebugDLL' -or $compile.DebugInformationFormat -ne 'ProgramDatabase' -or
        $compile.BasicRuntimeChecks -notin @($null, '', 'Default') -or $compile.AdditionalOptions -match '/RTC') {
        throw "Jolt must retain symbols and Debug DLL CRT while using /O2 /Ob2 without /RTC: $Path"
    }
    $definitions = $compile.PreprocessorDefinitions -split ';'
    foreach ($required in @('_DEBUG', '_ITERATOR_DEBUG_LEVEL=2', 'JPH_ENABLE_ASSERTS', 'JPH_OBJECT_STREAM',
        'JPH_DEBUG_RENDERER', 'JPH_PROFILE_ENABLED', 'JPH_FLOATING_POINT_EXCEPTIONS_ENABLED')) {
        if ($required -notin $definitions) { throw "Jolt Debug is missing $required in $Path" }
    }
}

$repo = Split-Path $PSScriptRoot -Parent
if (!$SourceDirectory) { $SourceDirectory = Join-Path $repo 'External/External.Libraries/jolt/JoltPhysics' }
if (!(Test-Path "$SourceDirectory/Build/CMakeLists.txt")) { throw "Jolt source is missing: $SourceDirectory" }
if (!$VisualStudioDirectory) {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $VisualStudioDirectory = & $vswhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -or !$VisualStudioDirectory) { throw 'Visual Studio 2026 with the v145 C++ toolset is required to build Jolt' }
}
$VisualStudioDirectory = [IO.Path]::GetFullPath($VisualStudioDirectory).TrimEnd('\', '/')
$SourceDirectory = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
$cmake = Join-Path $VisualStudioDirectory 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path $cmake)) { throw "Visual Studio CMake is missing: $cmake" }
$build = Join-Path $repo 'Build/JoltOptimized'

# Several application projects share this archive; only one may configure or build it at a time.
$hash = [Security.Cryptography.SHA256]::Create()
try { $key = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes([IO.Path]::GetFullPath($build).ToUpperInvariant()))).Replace('-', '') }
finally { $hash.Dispose() }
$mutex = New-Object Threading.Mutex($false, "Local\PEPEngine2-Jolt-$key")
$locked = $false
try {
    try { $locked = $mutex.WaitOne() } catch [Threading.AbandonedMutexException] { $locked = $true }
    $configure = @('-S', "$SourceDirectory/Build", '-B', $build, '-G', 'Visual Studio 18 2026', '-A', 'x64', '-T', 'v145,host=x64',
        "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioDirectory", '-DOVERRIDE_CXX_FLAGS=OFF',
        '-DCMAKE_CXX_FLAGS_DEBUG=/GS /O2 /Ob2 /D_DEBUG /D_ITERATOR_DEBUG_LEVEL=2',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL', '-DUSE_STATIC_MSVC_RUNTIME_LIBRARY=OFF',
        '-DJPH_BUILD_SHARED_LIBS=OFF', '-DINTERPROCEDURAL_OPTIMIZATION=OFF', '-DGENERATE_DEBUG_SYMBOLS=ON',
        '-DUSE_ASSERTS=ON', '-DENABLE_OBJECT_STREAM=ON', '-DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=ON',
        '-DPROFILER_IN_DEBUG_AND_RELEASE=ON', '-DFLOATING_POINT_EXCEPTIONS_ENABLED=ON',
        '-DTARGET_UNIT_TESTS=OFF', '-DTARGET_HELLO_WORLD=OFF', '-DTARGET_PERFORMANCE_TEST=OFF', '-DTARGET_SAMPLES=OFF', '-DTARGET_VIEWER=OFF',
        '-DJPH_USE_DX12=OFF', '-DJPH_USE_VK=OFF', '-DJPH_USE_MTL=OFF', '-DJPH_USE_CPU_COMPUTE=OFF',
        '-DUSE_SSE4_1=OFF', '-DUSE_SSE4_2=OFF', '-DUSE_AVX=OFF', '-DUSE_AVX2=OFF', '-DUSE_AVX512=OFF',
        '-DUSE_LZCNT=OFF', '-DUSE_TZCNT=OFF', '-DUSE_F16C=OFF', '-DUSE_FMADD=OFF')
    # Always let CMake/MSBuild check their input dependencies, including changed source and compiler flags.
    & $cmake @configure
    if ($LASTEXITCODE) { throw "Jolt CMake configuration failed in $build" }
    Test-JoltDebugProject (Join-Path $build 'Jolt.vcxproj')
    & $cmake --build $build --config Debug --target Jolt -- /m:1 /nr:false /p:PreferredToolArchitecture=x64
    if ($LASTEXITCODE) { throw "Optimized Debug Jolt build failed in $build" }
    if (!(Test-Path "$build/Debug/Jolt.lib")) { throw "Jolt build produced no archive: $build/Debug/Jolt.lib" }
}
finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
