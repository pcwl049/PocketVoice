$adb = 'D:\Android\Sdk\platform-tools\adb.exe'
$device = '/data/local/tmp/tiny-custom-io-probe'
$probe = 'G:\STTModels\qnn-work\tiny-custom-io-probe'
$result = 'D:\Project\STT\build\test-results\gate-b3-tiny-custom-io-probe'

# Clean and create result dir
if (Test-Path $result) { Remove-Item -Recurse -Force $result }
New-Item -ItemType Directory -Force -Path $result | Out-Null

# === uint8 variant ===
Write-Host "`n=== uint8 variant ===" -ForegroundColor Cyan
& $adb shell mkdir -p $device/uint8/input_zero $device/uint8/input_pattern $device/uint8/output_zero $device/uint8/output_pattern
& $adb push "$probe\build_uint8\libs\arm64-v8a\libmodel.so" $device/uint8/
& $adb push "$probe\inputs\cache_key_zero_uint8.raw" $device/uint8/input_zero/cache_key_0.raw
& $adb push "$probe\inputs\cache_key_pattern_uint8.raw" $device/uint8/input_pattern/cache_key_0.raw

# Create input list files
"cache_key_0:=./uint8/input_zero/cache_key_0.raw" | Set-Content -Path "$env:TEMP\gate_b3_uint8_zero.txt"
"cache_key_0:=./uint8/input_pattern/cache_key_0.raw" | Set-Content -Path "$env:TEMP\gate_b3_uint8_pattern.txt"
& $adb push "$env:TEMP\gate_b3_uint8_zero.txt" $device/uint8/input_zero.txt
& $adb push "$env:TEMP\gate_b3_uint8_pattern.txt" $device/uint8/input_pattern.txt

# Run uint8 zero
Write-Host "Running uint8 ZERO..."
$zeroLog = "$result\uint8.zero.log"
& $adb shell "cd $device && export LD_LIBRARY_PATH=. && export ADSP_LIBRARY_PATH=. && ./qnn-net-run --model ./uint8/libmodel.so --backend ./libQnnHtp.so --input_list ./uint8/input_zero.txt --output_dir ./uint8/output_zero --log_level info" 2>&1 | Set-Content -Path $zeroLog

# Run uint8 pattern
Write-Host "Running uint8 PATTERN..."
$patternLog = "$result\uint8.pattern.log"
& $adb shell "cd $device && export LD_LIBRARY_PATH=. && export ADSP_LIBRARY_PATH=. && ./qnn-net-run --model ./uint8/libmodel.so --backend ./libQnnHtp.so --input_list ./uint8/input_pattern.txt --output_dir ./uint8/output_pattern --log_level info" 2>&1 | Set-Content -Path $patternLog

# Pull uint8 results
Write-Host "Pulling uint8 results..."
New-Item -ItemType Directory -Force -Path "$result\uint8" | Out-Null
& $adb pull $device/uint8/output_zero "$result\uint8\output_zero" 2>&1 | Out-Null
& $adb pull $device/uint8/output_pattern "$result\uint8\output_pattern" 2>&1 | Out-Null

# === float32 variant ===
Write-Host "`n=== float32 variant ===" -ForegroundColor Cyan
& $adb shell mkdir -p $device/float32/input_zero $device/float32/input_pattern $device/float32/output_zero $device/float32/output_pattern
& $adb push "$probe\build_float32\libs\arm64-v8a\libmodel.so" $device/float32/
& $adb push "$probe\inputs\cache_key_zero_float32.raw" $device/float32/input_zero/cache_key_0.raw
& $adb push "$probe\inputs\cache_key_one_float32.raw" $device/float32/input_pattern/cache_key_0.raw

"cache_key_0:=./float32/input_zero/cache_key_0.raw" | Set-Content -Path "$env:TEMP\gate_b3_float32_zero.txt"
"cache_key_0:=./float32/input_pattern/cache_key_0.raw" | Set-Content -Path "$env:TEMP\gate_b3_float32_pattern.txt"
& $adb push "$env:TEMP\gate_b3_float32_zero.txt" $device/float32/input_zero.txt
& $adb push "$env:TEMP\gate_b3_float32_pattern.txt" $device/float32/input_pattern.txt

# Run float32 zero
Write-Host "Running float32 ZERO..."
$zeroLog = "$result\float32.zero.log"
& $adb shell "cd $device && export LD_LIBRARY_PATH=. && export ADSP_LIBRARY_PATH=. && ./qnn-net-run --model ./float32/libmodel.so --backend ./libQnnHtp.so --input_list ./float32/input_zero.txt --output_dir ./float32/output_zero --log_level info" 2>&1 | Set-Content -Path $zeroLog

# Run float32 pattern
Write-Host "Running float32 PATTERN..."
$patternLog = "$result\float32.pattern.log"
& $adb shell "cd $device && export LD_LIBRARY_PATH=. && export ADSP_LIBRARY_PATH=. && ./qnn-net-run --model ./float32/libmodel.so --backend ./libQnnHtp.so --input_list ./float32/input_pattern.txt --output_dir ./float32/output_pattern --log_level info" 2>&1 | Set-Content -Path $patternLog

# Pull float32 results
Write-Host "Pulling float32 results..."
New-Item -ItemType Directory -Force -Path "$result\float32" | Out-Null
& $adb pull $device/float32/output_zero "$result\float32\output_zero" 2>&1 | Out-Null
& $adb pull $device/float32/output_pattern "$result\float32\output_pattern" 2>&1 | Out-Null

Write-Host "`n=== DONE ===" -ForegroundColor Green
Write-Host "Results at: $result"
