# Builds every plugin, puts the binaries where the catalogue points, and
# writes the catalogue itself with real sizes and real hashes.
#
# The hash is the only thing standing between "the project published this" and
# "something else answered that URL", so it is never typed by hand - it is
# computed here from the file that is actually being published.
#
#   powershell -ExecutionPolicy Bypass -File plugins\publish.ps1
#
# Then commit plugins\dist and plugins\catalog.json and push. PicoView reads
# the catalogue straight from the default branch.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $PSScriptRoot "dist\win-x64"
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# id, folder, dll, name, author, version, description, extensions
$plugins = @(
    @{
        id = "upscale"; folder = "sample"; dll = "upscale.dll"
        name = "Приклад: PCX і збільшення"
        author = "PicoView"; version = "1.0"
        description = "Збільшує вдвічі й підвищує різкість"
        extensions = ".pcx"
    },
    @{
        id = "hoverpeek"; folder = "hoverpeek"; dll = "hoverpeek.dll"
        name = "Предперегляд у Провіднику"
        author = "PicoView"; version = "1.0"
        description = "Наведіть на файл у Провіднику - зʼявиться картка з ним"
        extensions = ""
    }
)

$rows = @()
foreach ($p in $plugins) {
    $dir = Join-Path $PSScriptRoot $p.folder
    Write-Host "building $($p.id) ..."
    Push-Location $dir
    & cmd /c ".\build.bat 2>&1" | Select-Object -Last 2 | ForEach-Object { "   $_" }
    Pop-Location

    $built = Join-Path $PSScriptRoot $p.dll
    if (-not (Test-Path $built)) { throw "$($p.dll) was not produced" }
    Copy-Item $built (Join-Path $dist $p.dll) -Force

    $f = Get-Item (Join-Path $dist $p.dll)
    $hash = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    $rows += [ordered]@{
        id          = $p.id
        file        = $p.dll
        name        = $p.name
        author      = $p.author
        version     = $p.version
        description = $p.description
        extensions  = $p.extensions
        platform    = "win-x64"
        size        = [int]$f.Length
        sha256      = $hash
        url         = "https://raw.githubusercontent.com/Reiclid/PicoView/main/plugins/dist/win-x64/$($p.dll)"
    }
    "   $($p.dll)  $($f.Length) bytes  $($hash.Substring(0,16))..."
}

$catalog = [ordered]@{ version = 1; plugins = $rows }
$json = $catalog | ConvertTo-Json -Depth 5
# Written as UTF-8 without a BOM: the parser in the viewer skips one if it is
# there, but every other tool is happier without.
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot "catalog.json"), $json,
                               (New-Object System.Text.UTF8Encoding $false))
"wrote catalog.json with $($rows.Count) entries"
