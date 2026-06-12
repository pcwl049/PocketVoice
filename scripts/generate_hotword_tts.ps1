param(
  [string]$InputTsv = "test_audio\hotwords.tsv",
  [string]$OutDir = "test_audio\generated\hotwords",
  [string]$Manifest = "test_audio\generated\hotwords_expected.tsv",
  [string]$VoiceName = "Microsoft Huihui Desktop"
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$inputPath = Join-Path $root $InputTsv
$outPath = Join-Path $root $OutDir
$manifestPath = Join-Path $root $Manifest

if (-not (Test-Path $inputPath)) {
  throw "Input TSV not found: $inputPath"
}

Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voice = $synth.GetInstalledVoices() |
  Where-Object { $_.VoiceInfo.Name -eq $VoiceName } |
  Select-Object -First 1

if (-not $voice) {
  $available = ($synth.GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }) -join ", "
  throw "Voice '$VoiceName' not found. Available voices: $available"
}

$synth.SelectVoice($VoiceName)
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 16000, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), ([System.Speech.AudioFormat.AudioChannel]::Mono)

New-Item -ItemType Directory -Force $outPath | Out-Null
New-Item -ItemType Directory -Force (Split-Path $manifestPath) | Out-Null

$rows = Import-Csv -LiteralPath $inputPath -Delimiter "`t"
$manifestRows = New-Object System.Collections.Generic.List[string]
$manifestRows.Add("file`tmode`texpected_contains`texpected_text`thotwords")

$index = 0
foreach ($row in $rows) {
  $index += 1
  $safeId = "{0:D2}-{1}" -f $index, ($row.category -replace "[^A-Za-z0-9_-]", "")
  $wav = Join-Path $outPath "$safeId.wav"
  $synth.SetOutputToWaveFile($wav, $format)
  $synth.Speak($row.phrase) | Out-Null
  $synth.SetOutputToNull()

  $rootText = $root.Path.TrimEnd("\") + "\"
  $wavText = (Resolve-Path -LiteralPath $wav).Path
  if ($wavText.StartsWith($rootText, [System.StringComparison]::OrdinalIgnoreCase)) {
    $relative = $wavText.Substring($rootText.Length)
  } else {
    $relative = $wavText
  }
  $expectedContains = (($row.hotwords -split "[,，;；|]") | Select-Object -First 1).Trim()
  $line = "$relative`t" + "wav`t$expectedContains`t$($row.phrase)`t$($row.hotwords)"
  $manifestRows.Add($line)
}

Set-Content -LiteralPath $manifestPath -Value $manifestRows -Encoding UTF8
Write-Host "Generated wavs: $index"
Write-Host "Output dir: $outPath"
Write-Host "Manifest: $manifestPath"
