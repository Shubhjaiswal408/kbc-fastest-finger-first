# Rebuilds KBC_Host/web_index.h from web/index.html (gzip-compressed, so the
# page loads ~4x faster from the ESP32).
# Run this after you change the web page, then re-upload KBC_Host.
#   powershell -ExecutionPolicy Bypass -File tools\embed.ps1
$root = Split-Path -Parent $PSScriptRoot
$bytes = [System.IO.File]::ReadAllBytes((Join-Path $root 'web\index.html'))

$ms = New-Object System.IO.MemoryStream
$gz = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionLevel]::Optimal)
$gz.Write($bytes, 0, $bytes.Length)
$gz.Close()
$z = $ms.ToArray()

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// AUTO-GENERATED from web/index.html by tools/embed.ps1 - do not edit by hand.")
[void]$sb.AppendLine("// $($bytes.Length) bytes of HTML, gzip-compressed to $($z.Length) bytes.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <pgmspace.h>")
[void]$sb.AppendLine("const size_t INDEX_HTML_GZ_LEN = $($z.Length);")
[void]$sb.Append("const uint8_t INDEX_HTML_GZ[] PROGMEM = {")
for ($i = 0; $i -lt $z.Length; $i++) {
  if ($i % 24 -eq 0) { [void]$sb.Append("`n  ") }
  [void]$sb.Append(('0x{0:X2},' -f $z[$i]))
}
[void]$sb.AppendLine("`n};")
[System.IO.File]::WriteAllText((Join-Path $root 'KBC_Host\web_index.h'), $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("web_index.h written: {0:N0} bytes of HTML -> {1:N0} bytes gzip" -f $bytes.Length, $z.Length)
