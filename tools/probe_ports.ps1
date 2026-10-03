param([string]$ip = "192.168.0.62")
foreach ($p in 9113,9114,9115,9116,9117) {
  $t = New-Object System.Net.Sockets.TcpClient
  try {
    $r = $t.BeginConnect($ip, $p, $null, $null)
    $ok = $r.AsyncWaitHandle.WaitOne(1500)
    if ($ok -and $t.Connected) { Write-Host "$p OPEN" } else { Write-Host "$p closed" }
  } catch { Write-Host "$p closed" }
  $t.Close()
}
