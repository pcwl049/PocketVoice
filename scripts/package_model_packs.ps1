param(
    [string]$Root = "D:\Project\STT"
)

$ErrorActionPreference = "Stop"

$modelsRoot = Join-Path $Root "models"
$outputRoot = Join-Path $Root "build\model-packs"

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$packs = @(
    @{ Name = "sensevoice"; Files = @("libmodel.so", "model.SM8550.bin", "tokens.txt") },
    @{ Name = "paraformer-qnn"; Files = @("libencoder.so", "libpredictor.so", "libdecoder.so", "tokens.txt") },
    @{ Name = "paraformer-offline"; Files = @("model.int8.onnx", "tokens.txt") }
)

foreach ($pack in $packs) {
    $sourceDir = Join-Path $modelsRoot $pack.Name
    if (-not (Test-Path $sourceDir)) {
        Write-Warning "Skip missing model dir: $sourceDir"
        continue
    }

    $staging = Join-Path $outputRoot $pack.Name
    if (Test-Path $staging) {
        Remove-Item -Recurse -Force $staging
    }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null

    $top = Join-Path $staging $pack.Name
    New-Item -ItemType Directory -Force -Path $top | Out-Null

    foreach ($file in $pack.Files) {
        $src = Join-Path $sourceDir $file
        if (-not (Test-Path $src)) {
            throw "Missing required file for $($pack.Name): $src"
        }
        Copy-Item -LiteralPath $src -Destination (Join-Path $top $file) -Force
    }

    $zipPath = Join-Path $outputRoot ($pack.Name + ".zip")
    if (Test-Path $zipPath) {
        Remove-Item -Force $zipPath
    }
    Compress-Archive -Path (Join-Path $staging $pack.Name) -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "Packed: $zipPath"
}
