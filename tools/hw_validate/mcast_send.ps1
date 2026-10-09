# Sends one E1.31 packet (hex) -Count times to an sACN multicast group from the
# Windows interface -From, the sequence byte (offset 111) bumped each time.
# validate_sacn_multicast.py uses it from WSL, whose NAT keeps multicast in.
param([string]$From, [string]$Group, [string]$Hex, [int]$Count = 20, [int]$GapMs = 25)
$bytes = [byte[]]::new($Hex.Length / 2)
for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($Hex.Substring(2 * $i, 2), 16) }
$s = New-Object System.Net.Sockets.Socket([System.Net.Sockets.AddressFamily]::InterNetwork, [System.Net.Sockets.SocketType]::Dgram, [System.Net.Sockets.ProtocolType]::Udp)
$s.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($From), 0)))
$s.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP, [System.Net.Sockets.SocketOptionName]::MulticastInterface, [System.Net.IPAddress]::Parse($From).GetAddressBytes())
$s.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::IP, [System.Net.Sockets.SocketOptionName]::MulticastTimeToLive, 4)
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($Group), 5568)
$seq = [int]$bytes[111]
for ($k = 0; $k -lt $Count; $k++) { $bytes[111] = [byte](($seq + $k) % 256); [void]$s.SendTo($bytes, $ep); Start-Sleep -Milliseconds $GapMs }
$s.Close()
