# Builds and runs the user-mode test of the driver's loopback mixer with AddressSanitizer.
# It compiles the real mixer functions, copied out of minwavert.cpp, against kernel API stubs.

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..\..")
$source = Join-Path $root "Windows-driver-samples\audio\sysvad\EndpointsCommon\minwavert.cpp"
$work = Join-Path $root "build\mixer_test"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$text = [IO.File]::ReadAllText($source) -replace "`r`n", "`n"
$start = $text.LastIndexOf("//=====", $text.IndexOf("CMiniportWaveRT::EnsureLoopbackRing()"))
$end = $text.IndexOf("`n}`n", $text.IndexOf("CMiniportWaveRT::ReadLoopbackMix")) + 3
[IO.File]::WriteAllText((Join-Path $work "mixer_extracted.inc"), $text.Substring($start, $end - $start))
Copy-Item (Join-Path $here "harness.cpp") $work -Force

$vcvars = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat", "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $vcvars) {
    throw "Visual Studio 2022 C++ x64 tools not found."
}
# vcvars64.bat may change the directory, so cd back after it.
cmd /c "`"$($vcvars.FullName)`" >nul 2>nul && cd /d `"$work`" && cl /nologo /EHsc /Zi /O1 /fsanitize=address /W3 /wd4005 harness.cpp /Fe:harness.exe && .\harness.exe"
exit $LASTEXITCODE
