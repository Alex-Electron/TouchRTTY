param([string]$cmd = "STATUS", [string]$port = "COM27", [int]$baud = 115200, [int]$wait = 2000)
$sp = New-Object System.IO.Ports.SerialPort $port, $baud
$sp.ReadTimeout = 3000
$sp.DtrEnable = $true
$sp.RtsEnable = $true
try {
    $sp.Open()
    Start-Sleep -Milliseconds 300
    if ($sp.BytesToRead -gt 0) { $sp.ReadExisting() | Out-Null }
    $sp.Write("$cmd`r`n")
    Start-Sleep -Milliseconds $wait
    $output = ""
    while ($sp.BytesToRead -gt 0) {
        $output += $sp.ReadExisting()
        Start-Sleep -Milliseconds 100
    }
    Write-Output $output
} finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
