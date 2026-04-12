$sp = New-Object System.IO.Ports.SerialPort 'COM27', 115200
$sp.ReadTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 300
if ($sp.BytesToRead -gt 0) { $null = $sp.ReadExisting() }

# Send STOP AUTO to trigger stop-bit detection
$sp.Write("STOP AUTO`r`n")
Write-Host ">>> Sent: STOP AUTO"

# Read output for 15 seconds (3 tests x 3s + margin)
$end = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $end) {
    if ($sp.BytesToRead -gt 0) {
        $data = $sp.ReadExisting()
        Write-Host $data -NoNewline
    }
    Start-Sleep -Milliseconds 200
}

# Get STATUS
if ($sp.BytesToRead -gt 0) { $null = $sp.ReadExisting() }
$sp.Write("STATUS`r`n")
Start-Sleep -Milliseconds 1500
if ($sp.BytesToRead -gt 0) {
    Write-Host ""
    Write-Host ">>> STATUS:"
    Write-Host $sp.ReadExisting()
}

$sp.Close()
