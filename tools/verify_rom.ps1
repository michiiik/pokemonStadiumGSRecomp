param(
    [string]$Path = (Join-Path $PSScriptRoot "..\baserom.z64")
)

$expectedSize = 67108864
$expectedMd5 = "1561C75D11CEDF356A8DDB1A4A5F9D5D"
$resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
$file = Get-Item -LiteralPath $resolvedPath

if ($file.Length -ne $expectedSize) {
    throw "Unexpected ROM size: $($file.Length) bytes (expected $expectedSize)"
}

$stream = [System.IO.File]::OpenRead($resolvedPath)
try {
    $header = New-Object byte[] 4
    if ($stream.Read($header, 0, 4) -ne 4) {
        throw "Could not read the ROM header"
    }
} finally {
    $stream.Dispose()
}

$headerText = ($header | ForEach-Object { $_.ToString("X2") }) -join " "
if ($headerText -ne "80 37 12 40") {
    throw "Unexpected ROM byte order/header: $headerText (expected 80 37 12 40)"
}

$actualMd5 = (Get-FileHash -Algorithm MD5 -LiteralPath $resolvedPath).Hash
if ($actualMd5 -ne $expectedMd5) {
    throw "Unexpected ROM MD5: $actualMd5 (expected $expectedMd5)"
}

Write-Host "Pokemon Stadium 2 US ROM verified: $resolvedPath"
Write-Host "Size: $expectedSize bytes"
Write-Host "MD5:  $expectedMd5"
