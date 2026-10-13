param(
    [string]$Python = 'python',
    [string]$Bash = 'C:/Program Files/Git/bin/bash.exe',
    [string]$CMakeDirectory = '',
    [string]$NinjaDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$deps = Join-Path $repository '.deps'
$sources = Join-Path $deps 'minimal-sources'
$sdk = Join-Path $deps 'ffmpeg-minimal-sdk'
$lockFile = Join-Path $repository 'third_party/ffmpeg/dependency.json'
$dependency = Get-Content -LiteralPath $lockFile -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force $deps, $sources | Out-Null
function Download([string]$Url, [string]$Path, [string]$Hash) {
    if (!(Test-Path -LiteralPath $Path) -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) {
        Invoke-WebRequest $Url -OutFile $Path
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) {
        throw "SHA-256 mismatch: $Path"
    }
}
Download $dependency.toolchain.url (Join-Path $deps 'w64devkit.zip') $dependency.toolchain.sha256
Download $dependency.runtimeLicense.url (Join-Path $deps 'GCC-COPYING.RUNTIME') $dependency.runtimeLicense.sha256
if (!(Test-Path (Join-Path $deps 'toolchain/w64devkit/bin/gcc.exe'))) {
    & $Python (Join-Path $PSScriptRoot 'ffmpeg_source_materials.py') extract-toolchain $repository
    if ($LASTEXITCODE -ne 0) { throw 'Toolchain extraction failed.' }
}
foreach ($source in $dependency.sources) {
    $archive = Join-Path $sources $source.name
    # Reuse the previous full SDK's exact source archive when available.
    $previous = Join-Path $deps "ffmpeg-sdk/third_party/ffmpeg/sources/$($source.name)"
    if (!(Test-Path $archive) -and (Test-Path $previous)) { Copy-Item $previous $archive }
    Download $source.url $archive $source.sha256
}
& $Python (Join-Path $PSScriptRoot 'ffmpeg_source_materials.py') extract $repository
if ($LASTEXITCODE -ne 0) { throw 'Source extraction failed.' }
$pythonBuild = Join-Path $deps 'python-build'
& $Python -m pip install --target $pythonBuild "meson==$($dependency.mesonVersion)"
if ($LASTEXITCODE -ne 0) { throw 'Meson preparation failed.' }
if (!$CMakeDirectory -or !$NinjaDirectory) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products '*' -property installationPath
        if ($vs) {
            $bundled = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake'
            if (!$CMakeDirectory) { $CMakeDirectory = Join-Path $bundled 'CMake/bin' }
            if (!$NinjaDirectory) { $NinjaDirectory = Join-Path $bundled 'Ninja' }
        }
    }
}
$originalPath = $env:PATH
try {
    $env:PATH = "$CMakeDirectory;$NinjaDirectory;$originalPath"
    & $Bash (Join-Path $PSScriptRoot 'build_ffmpeg_minimal.sh').Replace('\', '/')
    if ($LASTEXITCODE -ne 0) { throw 'Minimal FFmpeg build failed.' }
} finally { $env:PATH = $originalPath }
& $Python (Join-Path $PSScriptRoot 'ffmpeg_source_materials.py') package $repository
if ($LASTEXITCODE -ne 0) { throw 'License/source packaging failed.' }
Write-Host "Minimal LGPL shared FFmpeg SDK ready: $sdk"
