# Legacy-unicast mDNS A query (RFC 6762 6.7) sent straight to the board, so
# the reply reaches us even where Windows filters inbound multicast (a
# "Public" bench NIC). Prints "<name> -> answers=N ip=a.b.c.d" or "no answer".
# validate_webops.py runs it through powershell.exe when WSL has one.
param([string]$Name, [string]$Board = "192.168.2.50")
$labels = $Name.TrimEnd('.').Split('.')
$q = New-Object System.Collections.Generic.List[byte]
$q.AddRange([byte[]](0x12,0x34, 0,0, 0,1, 0,0, 0,0, 0,0))
foreach ($l in $labels) { $b = [Text.Encoding]::ASCII.GetBytes($l); $q.Add([byte]$b.Length); $q.AddRange($b) }
$q.AddRange([byte[]](0, 0,1, 0,1))
$u = New-Object Net.Sockets.UdpClient(0)
$u.Client.ReceiveTimeout = 2000
[void]$u.Send($q.ToArray(), $q.Count, $Board, 5353)
$ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
try {
  $r = $u.Receive([ref]$ep)
  $ans = $r[6]*256 + $r[7]
  # The answer's RDATA is the last 4 bytes of a single A record.
  $ip = ($r[($r.Length-4)..($r.Length-1)]) -join '.'
  "$Name -> answers=$ans ip=$ip (from $($ep.Address))"
} catch { "$Name -> no answer" }
$u.Close()
