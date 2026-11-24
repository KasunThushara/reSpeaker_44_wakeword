param(
  [int]$Port = 8099,
  [string]$Out = "D:\wakeword_anim_demo\assets\wake16k.raw"
)

# Minimal one-shot TCP HTTP server: accepts a single POST, writes the body to
# $Out, replies 200. Used to pull the browser-decoded PCM onto disk without
# passing tens of KB through the conversation.
$ErrorActionPreference = "Stop"
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Output "listening on 127.0.0.1:$Port"

$client = $listener.AcceptTcpClient()
$stream = $client.GetStream()

$acc = New-Object System.Collections.Generic.List[byte]
$buf = New-Object byte[] 4096
$headerEnd = -1
while ($headerEnd -lt 0) {
  $n = $stream.Read($buf, 0, $buf.Length)
  if ($n -le 0) { break }
  for ($i = 0; $i -lt $n; $i++) { $acc.Add($buf[$i]) }
  $arr = $acc.ToArray()
  for ($i = 3; $i -lt $arr.Length; $i++) {
    if ($arr[$i-3] -eq 13 -and $arr[$i-2] -eq 10 -and $arr[$i-1] -eq 13 -and $arr[$i] -eq 10) {
      $headerEnd = $i
      break
    }
  }
}

if ($headerEnd -lt 0) {
  Write-Output "no HTTP headers received"
  $client.Close(); $listener.Stop(); exit 1
}

$head = [System.Text.Encoding]::ASCII.GetString($acc.ToArray(), 0, $headerEnd + 1)
$m = [regex]::Match($head, "Content-Length:\s*(\d+)", "IgnoreCase")
if (-not $m.Success) {
  Write-Output "no Content-Length"
  $client.Close(); $listener.Stop(); exit 1
}
$len = [int]$m.Groups[1].Value

$all = $acc.ToArray()
$body = New-Object System.Collections.Generic.List[byte]
for ($i = $headerEnd + 1; $i -lt $all.Length; $i++) { $body.Add($all[$i]) }
while ($body.Count -lt $len) {
  $n = $stream.Read($buf, 0, $buf.Length)
  if ($n -le 0) { break }
  for ($i = 0; $i -lt $n; $i++) { $body.Add($buf[$i]) }
}

$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
[System.IO.File]::WriteAllBytes($Out, $body.ToArray())
Write-Output ("wrote " + $body.Count + " of " + $len + " bytes to " + $Out)

$resp = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nAccess-Control-Allow-Origin: *`r`nContent-Length: 2`r`nConnection: close`r`n`r`nok")
$stream.Write($resp, 0, $resp.Length)
$stream.Flush()
$client.Close()
$listener.Stop()
Write-Output "done"
