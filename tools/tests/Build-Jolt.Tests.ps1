$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '../Build-Jolt.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "Build-Jolt.ps1 has parse errors: $errors" }
$checker = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-JoltDebugProject' }, $false)
. ([scriptblock]::Create($checker.Extent.Text))
$path = Join-Path ([IO.Path]::GetTempPath()) "jolt-$([guid]::NewGuid()).vcxproj"
$valid = @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <Optimization>MaxSpeed</Optimization><InlineFunctionExpansion>AnySuitable</InlineFunctionExpansion>
      <RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary><DebugInformationFormat>ProgramDatabase</DebugInformationFormat>
      <PreprocessorDefinitions>_DEBUG;_ITERATOR_DEBUG_LEVEL=2;JPH_ENABLE_ASSERTS;JPH_OBJECT_STREAM;JPH_DEBUG_RENDERER;JPH_PROFILE_ENABLED;JPH_FLOATING_POINT_EXCEPTIONS_ENABLED;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
'@
try {
    [IO.File]::WriteAllText($path, $valid)
    Test-JoltDebugProject $path
    foreach ($broken in @(
        $valid.Replace('MaxSpeed', 'Disabled'),
        $valid.Replace('MultiThreadedDebugDLL', 'MultiThreadedDLL'),
        $valid.Replace('_ITERATOR_DEBUG_LEVEL=2', '_ITERATOR_DEBUG_LEVEL=0'),
        $valid.Replace('JPH_ENABLE_ASSERTS;', ''),
        $valid.Replace('</ClCompile>', '<BasicRuntimeChecks>EnableFastChecks</BasicRuntimeChecks></ClCompile>'))) {
        [IO.File]::WriteAllText($path, $broken)
        $rejected = $false
        try { Test-JoltDebugProject $path } catch { $rejected = $true }
        if (!$rejected) { throw 'An incompatible or unoptimized Jolt configuration was accepted' }
    }
    'Optimized Debug Jolt configuration checks passed'
}
finally { [IO.File]::Delete($path) }
