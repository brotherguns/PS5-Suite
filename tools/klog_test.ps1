param([string]$ip = "192.168.0.62", [int]$port = 9115)
$c = New-Object System.Net.Sockets.TcpClient
$c.Connect($ip, $port)
$s = $c.GetStream()
$hdr = New-Object byte[] 5
$hdr[0] = 0x6D
[BitConverter]::GetBytes([uint32]5).CopyTo($hdr, 1)
$s.Write($hdr, 0, 5); $s.Write([Text.Encoding]::ASCII.GetBytes("4096`0"), 0, 5); $s.Flush()
function ReadExact($n) {
  $b = New-Object byte[] $n; $off = 0
  while ($off -lt $n) { $r = $s.Read($b, $off, $n - $off); if ($r -le 0) { throw "eof" }; $off += $r }
  return ,$b
}
$h = ReadExact 5
$resp = $h[0]; $dlen = [BitConverter]::ToUInt32($h, 1)
Write-Host "resp=$resp dlen=$dlen"
if ($dlen -gt 0) {
  $d = ReadExact ([Math]::Min($dlen, 300))
  Write-Host ([Text.Encoding]::UTF8.GetString($d))
}
$c.Close()
