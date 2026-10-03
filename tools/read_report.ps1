param([string]$ip = "192.168.0.62", [int]$port = 9114)
try {
  $c = New-Object System.Net.Sockets.TcpClient
  $c.Connect($ip, $port)
  $c.ReceiveTimeout = 3000
  $s = $c.GetStream()
  $b = New-Object byte[] 16384
  Start-Sleep -Milliseconds 500
  $n = 0
  try { $n = $s.Read($b, 0, 16384) } catch {}
  if ($n -gt 0) { Write-Host ([Text.Encoding]::UTF8.GetString($b, 0, $n)) } else { Write-Host "connected, no data" }
  $c.Close()
} catch { Write-Host "connect failed" }
