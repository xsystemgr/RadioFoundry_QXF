# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# IPv4 to a radio over its programming cable from Windows, with Windows' own dial-up client (docs/spec/ppp.md 5.1):
# the cable is a "Communications cable between two computers" modem and a dial-up entry uses it. The radio answers
# Windows' "CLIENT" with "CLIENTSERVER" and runs PPP; this script dials the entry and adds the routes to the other
# radios (12.0.0.0/8) and the talkgroups (225.0.0.0/8) through it, as the USB network adapter's DHCP does.
#
#   powershell -ExecutionPolicy Bypass -File tools\rfoundry_ppp_windows.ps1 -Setup COM5   one-time: what to set up
#   powershell -ExecutionPolicy Bypass -File tools\rfoundry_ppp_windows.ps1               connect (as administrator)
#   powershell -ExecutionPolicy Bypass -File tools\rfoundry_ppp_windows.ps1 -Stop         hang up
param(
	[string]$Setup = "",
	[switch]$Stop,
	[string]$Entry = "RadioFoundry"
)
$ErrorActionPreference = "Stop"

if ($Setup -ne "") {
	Write-Host @"
One-time setup for $Setup (the radio's programming cable):

1. The modem: Control Panel > Phone and Modem > Modems > Add... > tick "Don't detect my modem" > Next >
   Manufacturer "(Standard Modem Types)", Model "Communications cable between two computers" > Next > $Setup > Finish.
   Then Properties of that modem > Change settings > Modem > Maximum Port Speed: 115200.

2. The dial-up entry: Settings > Network & Internet > Dial-up > Set up a new connection > Set up a dial-up connection.
   Modem: the cable on $Setup. Phone number: 0. User name and password: anything (the radio asks for none).
   Connection name: $Entry. Click Create, then Close (don't let it test the Internet).

3. Its properties: Control Panel > Network and Sharing Center > Change adapter settings > $Entry > Properties >
   Networking > Internet Protocol Version 4 > Properties > Advanced > untick "Use default gateway on remote network".
   On the Security tab: Data encryption "No encryption allowed".

Then run this script without -Setup, as administrator. The radio must run RadioFoundry and be on Home (not in hotspot
or CPS mode); close the GUI and qdmr first, they hold the port.
"@
	exit 0
}

if ($Stop) {
	rasdial $Entry /disconnect
	exit $LASTEXITCODE
}

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
	[Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
	Write-Host "rfoundry_ppp: run as administrator (the routes need it)"
	exit 1
}

rasdial $Entry
if ($LASTEXITCODE -ne 0) {
	Write-Host "rfoundry_ppp: dial-up failed ($LASTEXITCODE). -Setup COMx shows the one-time setup"
	exit $LASTEXITCODE
}

$cfg = Get-NetIPConfiguration -InterfaceAlias $Entry
$hostIp = ($cfg.IPv4Address | Select-Object -First 1).IPAddress
$idx = $cfg.InterfaceIndex
# The radio is the host's address with the last bit flipped (core/dmr/ipGateway.h)
$o = $hostIp.Split(".")
$radioIp = "{0}.{1}.{2}.{3}" -f $o[0], $o[1], $o[2], ([int]$o[3] -bxor 1)

# On-link through the PPP interface, for this session only (gone when the link goes)
foreach ($p in "12.0.0.0/8", "225.0.0.0/8") {
	Remove-NetRoute -DestinationPrefix $p -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue
	New-NetRoute -DestinationPrefix $p -InterfaceIndex $idx -NextHop 0.0.0.0 -PolicyStore ActiveStore | Out-Null
}

Write-Host "rfoundry_ppp: you are $hostIp, the radio $radioIp. Try:"
Write-Host "  ping $radioIp"
Write-Host "  py tools\rfoundry_debug.py --port udp:$radioIp view"
Write-Host "  start http://$radioIp/"
Write-Host "  powershell -File tools\rfoundry_ppp_windows.ps1 -Stop"
