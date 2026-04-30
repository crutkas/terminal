# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# One-shot generator for EPresCorpus.inc.
# Reads UCD emoji-data.txt (Unicode 16.0.0) and emits a sorted, de-duplicated
# list of every codepoint with Emoji_Presentation=Yes.
#
# Usage:
#   pwsh src/types/ut_types/gen_epres_corpus.ps1 \
#        -InputPath emoji-data.txt \
#        -OutputPath src/types/ut_types/EPresCorpus.inc

param(
    [Parameter(Mandatory = $true)] [string] $InputPath,
    [Parameter(Mandatory = $true)] [string] $OutputPath
)

$cps = New-Object System.Collections.Generic.SortedSet[uint32]
foreach ($line in Get-Content -LiteralPath $InputPath) {
    $stripped = ($line -split '#', 2)[0].Trim()
    if (-not $stripped) { continue }
    $parts = $stripped -split ';'
    if ($parts.Count -lt 2) { continue }
    $prop = $parts[1].Trim()
    if ($prop -ne 'Emoji_Presentation') { continue }
    $range = $parts[0].Trim()
    if ($range -match '^([0-9A-Fa-f]+)\.\.([0-9A-Fa-f]+)$') {
        $lo = [System.Convert]::ToUInt32($Matches[1], 16)
        $hi = [System.Convert]::ToUInt32($Matches[2], 16)
        for ($c = $lo; $c -le $hi; $c++) { [void]$cps.Add([uint32]$c) }
    }
    elseif ($range -match '^([0-9A-Fa-f]+)$') {
        [void]$cps.Add([uint32]([System.Convert]::ToUInt32($Matches[1], 16)))
    }
}

$lines = @(
    '// Copyright (c) Microsoft Corporation.'
    '// Licensed under the MIT license.'
    '//'
    '// AUTO-GENERATED -- do not edit by hand.'
    '// Source: UCD emoji-data.txt, Unicode 16.0.0 (Emoji Version 16.0).'
    '// Generator: src/types/ut_types/gen_epres_corpus.ps1'
    "// Total Emoji_Presentation=Yes codepoints: $($cps.Count)"
    ''
    'static constexpr char32_t s_epresCorpus[] = {'
)
$row = New-Object System.Text.StringBuilder
$col = 0
foreach ($cp in $cps) {
    if ($col -eq 0) { [void]$row.Append('    ') }
    [void]$row.Append(('0x{0:X5},' -f $cp))
    $col++
    if ($col -eq 8) {
        $lines += $row.ToString().TrimEnd()
        $row.Clear() | Out-Null
        $col = 0
    } else {
        [void]$row.Append(' ')
    }
}
if ($col -ne 0) { $lines += $row.ToString().TrimEnd() }
$lines += '};'
$lines += ''

Set-Content -LiteralPath $OutputPath -Value $lines -Encoding ASCII
Write-Host "Wrote $($cps.Count) codepoints to $OutputPath"
