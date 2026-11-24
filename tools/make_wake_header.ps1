param(
  [string]$Raw  = "D:\wakeword_anim_demo\assets\wake16k.raw",
  [string]$Out  = "D:\wakeword_anim_demo\firmware\audio_play\wake_sound.h"
)

# Convert raw little-endian int16 PCM into a C header. 16 values per line so the
# file stays readable and the compiler is not asked to parse a 12k-element line.
$bytes = [System.IO.File]::ReadAllBytes($Raw)
$n = [int]($bytes.Length / 2)

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <stdint.h>")
[void]$sb.AppendLine("// Generated from assets/wake16k.raw")
[void]$sb.AppendLine("// Home Assistant Voice PE 'wake_word_triggered.flac' -> 16 kHz mono int16,")
[void]$sb.AppendLine("// silence trimmed, peak normalized to 0.85. Do not edit by hand.")
[void]$sb.AppendLine("#define WAKE_SOUND_LEN $n")
[void]$sb.AppendLine("static const int16_t WAKE_SOUND[WAKE_SOUND_LEN] = {")

$line = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $n; $i++) {
  $v = [BitConverter]::ToInt16($bytes, $i * 2)
  [void]$line.Append($v)
  if ($i -lt $n - 1) { [void]$line.Append(",") }
  if (($i % 16) -eq 15 -or $i -eq $n - 1) {
    [void]$sb.AppendLine("  " + $line.ToString())
    $line.Length = 0
  }
}
[void]$sb.AppendLine("};")

[System.IO.File]::WriteAllText($Out, $sb.ToString())
Write-Output ("samples: " + $n + "  (" + [math]::Round($n / 16000.0, 3) + " s at 16 kHz)")
Write-Output ("header : " + $Out + "  " + (Get-Item $Out).Length + " bytes")
