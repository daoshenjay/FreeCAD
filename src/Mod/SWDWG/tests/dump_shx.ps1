$path = 'C:\Program Files\Autodesk\AutoCAD 2016\UserDataCache\Support\ltypeshp.shx'
$data = [System.IO.File]::ReadAllBytes($path)
Write-Host 'header:' ([System.Text.Encoding]::ASCII.GetString($data, 0, 24))
$first = $data[24] + ($data[25] * 256)
$last = $data[26] + ($data[27] * 256)
$count = $data[28] + ($data[29] * 256)
Write-Host "first=$first last=$last count=$count"
$off = 30 + $count * 4
for ($i = 0; $i -lt $count; $i++) {
    $num = $data[30 + $i * 4] + ($data[30 + $i * 4 + 1] * 256)
    $len = $data[30 + $i * 4 + 2] + ($data[30 + $i * 4 + 3] * 256)
    $ne = [Array]::IndexOf($data, [byte]0, $off)
    $name = [System.Text.Encoding]::ASCII.GetString($data, $off, $ne - $off)
    Write-Host "`n$name num=$num len=$len"
    if ($name -in @('ZIG','BAT','TRACK1','ZAG','CIRC1','CIRC2','TRACK2','GAP','AMZIGZAG','BOX')) {
        $codes = $data[($ne + 1)..($off + $len - 1)]
        $hex = ($codes | ForEach-Object { $_.ToString('X2') }) -join ' '
        Write-Host "  raw=$hex"
    }
    $off += $len
}
