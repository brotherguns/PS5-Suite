param([string]$ip = "192.168.0.62", [int]$port = 9113)
try {
  $c = New-Object System.Net.Sockets.TcpClient
  $c.Connect($ip, $port)
  $c.ReceiveTimeout = 5000
  $s = $c.GetStream()
  $hdr = New-Object byte[] 5
  $hdr[0] = 0x32
  $s.Write($hdr, 0, 5); $s.Flush()
  $b = New-Object byte[] 5; $off = 0
  while ($off -lt 5) { $r = $s.Read($b, $off, 5 - $off); if ($r -le 0) { throw "eof" }; $off += $r }
  $dlen = [BitConverter]::ToUInt32($b, 1)
  $d = New-Object byte[] $dlen; $off = 0
  while ($off -lt $dlen) { $r = $s.Read($d, $off, $dlen - $off); if ($r -le 0) { break }; $off += $r }
  $txt = [Text.Encoding]::UTF8.GetString($d)
  $ver = ($txt -split "`n" | Where-Object { $_ -match "server_version" })
  Write-Host "$port => resp=$($b[0]) $ver"
  $c.Close()
} catch { Write-Host "$port => ERROR $($_.Exception.Message)" }
