param(
  [Parameter(Mandatory = $true)]
  [string]$Variant
)

$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$qairt = 'G:\Program Files\qairt\2.45.0.260326'
$ndk = Join-Path $root 'third_party\android-ndk-r27c'
$convertDir = Join-Path $root "build\qnn-convert\$Variant"
$build = Join-Path $root "build\qnn-model-lib-android\$Variant"
$jni = Join-Path $build 'jni'
$objRoot = Join-Path $build 'manual-obj'
$rawDir = Join-Path $build 'obj\binary'
$outDir = Join-Path $build 'libs\arm64-v8a'
$localQnnInclude = Join-Path $build 'qnn-include'
$toolbin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$qnnInclude = Join-Path $qairt 'include\QNN'
$qnnJni = Join-Path $qairt 'share\QNN\converter\jni'

foreach ($path in @(
  (Join-Path $convertDir 'model.cpp'),
  (Join-Path $convertDir 'model.bin'),
  (Join-Path $qnnJni 'QnnModel.cpp'),
  (Join-Path $qnnJni 'QnnWrapperUtils.cpp'),
  (Join-Path $qnnJni 'linux\QnnModelPal.cpp'),
  (Join-Path $toolbin 'aarch64-linux-android21-clang++.cmd'),
  (Join-Path $toolbin 'llvm-objcopy.exe')
)) {
  if (-not (Test-Path $path)) {
    throw "Required file missing: $path"
  }
}

Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $jni, $objRoot, $rawDir, $outDir, $localQnnInclude | Out-Null

Copy-Item (Join-Path $qnnJni '*') $jni -Recurse -Force
Copy-Item (Join-Path $convertDir 'model.cpp') $jni -Force
Copy-Item (Join-Path $convertDir 'model.bin') $jni -Force
Copy-Item (Join-Path $qnnInclude '*') $localQnnInclude -Recurse -Force

Push-Location $rawDir
try {
  tar -xf (Join-Path $jni 'model.bin')
} finally {
  Pop-Location
}

$raws = Get-ChildItem $rawDir -Filter '*.raw'
Write-Host "raw files: $($raws.Count)"
foreach ($raw in $raws) {
  $obj = Join-Path $objRoot ($raw.BaseName + '.o')
  $relRaw = "obj/binary/$($raw.Name)"
  Push-Location $build
  try {
    & (Join-Path $toolbin 'llvm-objcopy.exe') -I binary -O elf64-littleaarch64 -B aarch64 $relRaw $obj
  } finally {
    Pop-Location
  }
  if ($LASTEXITCODE -ne 0) {
    throw "objcopy failed: $($raw.Name)"
  }
}

$clang = Join-Path $toolbin 'aarch64-linux-android21-clang++.cmd'
$cppObjs = @()
foreach ($src in @('QnnModel.cpp', 'QnnWrapperUtils.cpp', 'linux\QnnModelPal.cpp', 'model.cpp')) {
  $obj = Join-Path $objRoot (($src -replace '[\\/]', '_') + '.o')
  & $clang -c (Join-Path $jni $src) -o $obj -std=c++11 -O3 -fPIC -fvisibility=hidden '-DQNN_API=__attribute__((visibility(\"default\")))' "-I$jni" "-I$localQnnInclude" -Wno-write-strings
  if ($LASTEXITCODE -ne 0) {
    throw "compile failed: $src"
  }
  $cppObjs += $obj
}

$out = Join-Path $outDir 'libmodel.so'
$allObjs = @($cppObjs) + (Get-ChildItem $objRoot -Filter '*.o' | Where-Object { $cppObjs -notcontains $_.FullName } | ForEach-Object FullName)
$rsp = Join-Path $objRoot 'objects.rsp'
($allObjs | ForEach-Object { '"' + ($_.Replace('\', '/')) + '"' }) | Set-Content -Path $rsp -Encoding ASCII

& $clang -shared -o $out "@$rsp" '-Wl,-z,max-page-size=16384' -lc -lm -ldl
if ($LASTEXITCODE -ne 0) {
  throw 'link failed'
}

Get-Item $out | Select-Object FullName, Length, LastWriteTime | Format-List
