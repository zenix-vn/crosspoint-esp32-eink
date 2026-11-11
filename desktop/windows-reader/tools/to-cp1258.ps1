# Rewrites Vietnamese text into the form code page 1258 can hold: letters with
# their circumflex, breve or horn precomposed (â ă ê ô ơ ư đ), and the tone mark
# (grave, acute, tilde, hook, dot below) as a separate combining character after
# them. "ặ" becomes "ă" + U+0323. The MSI database uses code page 1258.
param([Parameter(Mandatory)] [string]$In, [Parameter(Mandatory)] [string]$Out)
$ErrorActionPreference = 'Stop'
[System.Text.Encoding]::RegisterProvider([System.Text.CodePagesEncodingProvider]::Instance)

$modifiers = [char[]]@([char]0x0302, [char]0x0306, [char]0x031B)  # circumflex, breve, horn
$text = [System.IO.File]::ReadAllText($In).Normalize([System.Text.NormalizationForm]::FormD)
$sb = New-Object System.Text.StringBuilder
$i = 0
while ($i -lt $text.Length) {
    $base = [string]$text[$i]; $i++
    $mods = ''; $tones = ''
    while ($i -lt $text.Length -and [System.Globalization.CharUnicodeInfo]::GetUnicodeCategory($text[$i]) -eq 'NonSpacingMark') {
        if ($modifiers -contains $text[$i]) { $mods += $text[$i] } else { $tones += $text[$i] }
        $i++
    }
    [void]$sb.Append(($base + $mods).Normalize([System.Text.NormalizationForm]::FormC)).Append($tones)
}
$result = $sb.ToString()

# Fail the build instead of shipping "?" in the wizard.
$strict = [System.Text.Encoding]::GetEncoding(1258, [System.Text.EncoderFallback]::ExceptionFallback, [System.Text.DecoderFallback]::ExceptionFallback)
[void]$strict.GetBytes($result)

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[System.IO.File]::WriteAllText($Out, $result, (New-Object System.Text.UTF8Encoding $false))
