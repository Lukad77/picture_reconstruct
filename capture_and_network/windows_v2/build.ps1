$cmakePath = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$sourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $sourceDir 'build'
& $cmakePath -S $sourceDir -B $buildDir -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmakePath --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmakePath --build $buildDir --config Release --target test_v2_end_to_end
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
& $ctestPath --test-dir $buildDir -C Release --output-on-failure
exit $LASTEXITCODE
