param([string]$ip = "192.168.0.62", [int]$port = 9115,
      [string]$remote = "/data/pkg_http.log", [string]$local = "$env:TEMP\pkg_http.log")
$c = New-Object System.Net.Sockets.TcpClient
$c.Connect($ip, $port)
$s = $c.GetStream()
$path = [Text.Encoding]::UTF8.GetBytes($remote + [char]0)
$hdr = New-Object byte[] 5
$hdr[0] = 0x13
[BitConverter]::GetBytes([uint32]$path.Length).CopyTo($hdr, 1)
$s.Write($hdr, 0, 5); $s.Write($path, 0, $path.Length); $s.Flush()
function ReadExact($n) {
  $b = New-Object byte[] $n; $off = 0
  while ($off -lt $n) { $r = $s.Read($b, $off, $n - $off); if ($r -le 0) { throw "eof" }; $off += $r }
  return ,$b
}
$h = ReadExact 5
$resp = $h[0]; $dlen = [BitConverter]::ToUInt32($h, 1)
Write-Host "resp=$resp dlen=$dlen"
if ($dlen -eq 8) {
  $sz = [BitConverter]::ToInt64((ReadExact 8), 0)
  Write-Host "fileSize=$sz"
  $fs = [IO.File]::Create($local)
  $buf = New-Object byte[] 65536; $got = 0
  while ($got -lt $sz) { $r = $s.Read($buf, 0, [Math]::Min(65536, $sz - $got)); if ($r -le 0) { break }; $fs.Write($buf, 0, $r); $got += $r }
  $fs.Close()
  Write-Host "wrote $got bytes -> $local"
} elseif ($dlen -gt 0) {
  $d = ReadExact $dlen
  Write-Host ([Text.Encoding]::UTF8.GetString($d))
}
$c.Close()
