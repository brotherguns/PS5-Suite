param([string]$ip = "192.168.0.62", [int]$port = 9021,
      [string]$file = "C:\Users\HACKMAN\Desktop\ps5 test\my_projects\ps5_upload_suite\payload\ps5_suite_server.elf")
$bytes = [IO.File]::ReadAllBytes($file)
$c = New-Object System.Net.Sockets.TcpClient
$c.Connect($ip, $port)
$s = $c.GetStream()
$s.Write($bytes, 0, $bytes.Length)
$s.Flush()
$c.Close()
Write-Host "SENT: $($bytes.Length) bytes"
