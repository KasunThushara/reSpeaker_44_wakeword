param([string]$Port = "COM9", [int]$Seconds = 60, [string]$Log = "D:\wakeword_anim_demo\build\serial_log.txt")

Remove-Item $Log -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds($Seconds)

function Open-Port([string]$name) {
  $p = New-Object System.IO.Ports.SerialPort $name, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
  $p.ReadTimeout = 500
  # never assert DTR/RTS: DTR+RTS together is the esptool download-mode combo
  $p.DtrEnable = $false
  $p.RtsEnable = $false
  $p.Open()
  return $p
}

$sp = $null
for ($i = 0; $i -lt 10 -and $sp -eq $null; $i++) {
  try { $sp = Open-Port $Port } catch { Start-Sleep -Milliseconds 400 }
}
if ($sp -eq $null) { Write-Output "could not open $Port"; exit 1 }
$sp.DtrEnable = $false
$sp.RtsEnable = $false

while ((Get-Date) -lt $deadline) {
  try {
    $s = $sp.ReadExisting()
    if ($s) {
      [Console]::Out.Write($s)
      Add-Content -Path $Log -Value $s -NoNewline
    }
  } catch {
    try { $sp.Close() } catch {}
    Start-Sleep -Milliseconds 500
    try { $sp = Open-Port $Port; $sp.DtrEnable = $false; $sp.RtsEnable = $false } catch { }
  }
  Start-Sleep -Milliseconds 80
}
try { $sp.Close() } catch {}
