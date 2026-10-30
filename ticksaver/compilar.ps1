# TickSaver - compila el mod y lo instala en Geometry Dash (Windows)
# Lo ejecuta compilar.bat. No hace falta abrirlo a mano.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$GeodeVersion = '5.11.2'
$CliVersion   = '3.9.0'
$Proj  = $PSScriptRoot
$Root  = 'C:\TickSaver-build'
$Tools = Join-Path $Root 'tools'
$Sdk   = Join-Path $Root 'geode-sdk'
$ModDir = Join-Path $Root 'mod'

New-Item -ItemType Directory -Force -Path $Root, $Tools | Out-Null
try { Start-Transcript -Path (Join-Path $Root 'registro.txt') -Force | Out-Null } catch {}

function Paso($n, $texto) {
    Write-Host ''
    Write-Host "[$n/8] $texto" -ForegroundColor Cyan
}
function Falla($texto) {
    Write-Host ''
    Write-Host "ERROR: $texto" -ForegroundColor Red
    Write-Host 'Sacale una captura a esta ventana y mandasela a Claude.' -ForegroundColor Yellow
    Write-Host "(Tambien hay un registro completo en $Root\registro.txt)"
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}
function Refrescar-Path {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $extra = ($script:ExtraPaths | Where-Object { Test-Path $_ }) -join ';'
    $env:Path = "$extra;$Tools;$machine;$user"
}
function Tiene($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }
# las carpetas agregadas se recuerdan para que no se pierdan al refrescar el PATH
$script:ExtraPaths = New-Object System.Collections.Generic.List[string]
function Agregar-Path($dir) {
    if (Test-Path $dir) {
        if (-not $script:ExtraPaths.Contains($dir)) { $script:ExtraPaths.Add($dir) }
        $env:Path = "$dir;$env:Path"
    }
}
function Instalar-Winget($id, [string[]]$extra = @()) {
    Write-Host "  Instalando $id (puede tardar)..."
    $wargs = @('install', '--id', $id, '-e', '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements') + $extra
    & winget @wargs
    Write-Host "  winget termino con codigo $LASTEXITCODE"
    Refrescar-Path
}
function Bajar($url, $destino) {
    Write-Host "  Bajando $url"
    Invoke-WebRequest -Uri $url -OutFile $destino -UseBasicParsing
}

Write-Host '==============================================' -ForegroundColor Green
Write-Host '   TickSaver - compilador automatico' -ForegroundColor Green
Write-Host '==============================================' -ForegroundColor Green
Write-Host 'Esto tarda entre 30 y 60 minutos la primera vez.'
Write-Host 'Si Windows pregunta si permitis cambios, apreta SI.'

Refrescar-Path

# ---------------------------------------------------------------- 1
Paso 1 'Revisando winget (el instalador de Windows)'
if (-not (Tiene 'winget')) {
    Falla "No encontre 'winget'. Abri Microsoft Store, busca 'Instalador de aplicacion' (App Installer), actualizalo y volve a ejecutar compilar.bat."
}
Write-Host '  OK'

# ---------------------------------------------------------------- 2
Paso 2 'Git'
if (-not (Tiene 'git')) { Instalar-Winget 'Git.Git' @('--silent') }
Agregar-Path 'C:\Program Files\Git\cmd'
if (-not (Tiene 'git')) { Falla 'No se pudo instalar Git.' }
Write-Host '  OK'

# ---------------------------------------------------------------- 3
Paso 3 'CMake y Ninja'
if (-not (Tiene 'cmake')) { Instalar-Winget 'Kitware.CMake' @('--silent') }
Agregar-Path 'C:\Program Files\CMake\bin'
if (-not (Tiene 'cmake')) { Falla 'No se pudo instalar CMake.' }
if (-not (Test-Path (Join-Path $Tools 'ninja.exe'))) {
    $zip = Join-Path $Root 'ninja.zip'
    Bajar 'https://github.com/ninja-build/ninja/releases/latest/download/ninja-win.zip' $zip
    Expand-Archive -Path $zip -DestinationPath $Tools -Force
}
Write-Host '  OK'

# ---------------------------------------------------------------- 4
Paso 4 'LLVM / Clang (el compilador)'
Agregar-Path 'C:\Program Files\LLVM\bin'
$clangOk = $false
if (Tiene 'clang') {
    $v = (& clang --version | Select-Object -First 1)
    if ($v -match 'version (\d+)\.' -and [int]$Matches[1] -ge 19) { $clangOk = $true }
}
if (-not $clangOk) {
    Instalar-Winget 'LLVM.LLVM' @('--silent')
    Agregar-Path 'C:\Program Files\LLVM\bin'
    if (Tiene 'clang') {
        $v = (& clang --version | Select-Object -First 1)
        if ($v -match 'version (\d+)\.' -and [int]$Matches[1] -ge 19) { $clangOk = $true }
    }
}
$clangDir = $null
if ($clangOk) {
    $clangDir = Split-Path -Parent (Get-Command clang).Source
    Agregar-Path $clangDir
}
if ($clangOk) { Write-Host "  OK ($clangDir)" } else { Write-Host '  No hay Clang 19+, voy a usar el compilador de Microsoft.' -ForegroundColor Yellow }

# ---------------------------------------------------------------- 5
Paso 5 'Herramientas de C++ de Microsoft (lo mas pesado, 6-8 GB)'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Buscar-VS {
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}
$vs = Buscar-VS
if (-not $vs) {
    Write-Host '  Se va a abrir el instalador de Visual Studio. Dejalo terminar solo, no lo cierres.'
    Instalar-Winget 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    $vs = Buscar-VS
}
if (-not $vs) {
    Falla 'No se encontraron las herramientas de C++ (Visual Studio Build Tools). Si el instalador pidio reiniciar, reinicia la PC y volve a ejecutar compilar.bat.'
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Falla "No encontre $vcvars" }
Write-Host "  OK ($vs)"

# ---------------------------------------------------------------- 6
Paso 6 'Geode CLI y Geode SDK'
if (-not (Test-Path (Join-Path $Tools 'geode.exe'))) {
    $zip = Join-Path $Root 'geode-cli.zip'
    Bajar "https://github.com/geode-sdk/cli/releases/download/v$CliVersion/geode-cli-v$CliVersion-win.zip" $zip
    Expand-Archive -Path $zip -DestinationPath $Tools -Force
}
if (-not (Test-Path (Join-Path $Tools 'geode.exe'))) { Falla 'No se pudo bajar el Geode CLI.' }

if (-not (Test-Path (Join-Path $Sdk 'VERSION'))) {
    if (Test-Path $Sdk) { Remove-Item -Recurse -Force $Sdk }
    Write-Host '  Bajando Geode SDK...'
    & git clone --depth 1 --branch "v$GeodeVersion" https://github.com/geode-sdk/geode.git $Sdk
    if ($LASTEXITCODE -ne 0) { Falla 'No se pudo bajar el Geode SDK.' }
}
$binDir = Join-Path $Sdk "bin\$GeodeVersion"
if (-not (Test-Path (Join-Path $binDir 'Geode.lib'))) {
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    $zip = Join-Path $Root 'geode-bin.zip'
    Bajar "https://github.com/geode-sdk/geode/releases/download/v$GeodeVersion/geode-v$GeodeVersion-win.zip" $zip
    Expand-Archive -Path $zip -DestinationPath $binDir -Force
}
if (-not (Test-Path (Join-Path $binDir 'Geode.lib'))) { Falla 'No se pudieron bajar los binarios de Geode.' }
$env:GEODE_SDK = $Sdk
Write-Host '  OK'

# ---------------------------------------------------------------- 7
Paso 7 'Compilando TickSaver'
if (Test-Path $ModDir) { Remove-Item -Recurse -Force $ModDir }
New-Item -ItemType Directory -Force -Path $ModDir | Out-Null
foreach ($item in @('src', 'mod.json', 'CMakeLists.txt', 'about.md', 'logo.png', 'LICENSE')) {
    $origen = Join-Path $Proj $item
    if (Test-Path $origen) { Copy-Item -Recurse -Force $origen $ModDir }
}
if (-not (Test-Path (Join-Path $ModDir 'src\main.cpp'))) { Falla 'No encontre src\main.cpp. Descomprimi todo el zip antes de ejecutar compilar.bat.' }

$build = Join-Path $ModDir 'build'
$cpm = Join-Path $Root 'cpm-cache'
if ($clangOk) {
    # rutas completas al compilador, con barras normales (CMake las prefiere)
    $clangC = ((Join-Path $clangDir 'clang.exe') -replace '\\', '/')
    $clangCxx = ((Join-Path $clangDir 'clang++.exe') -replace '\\', '/')
    $cc = "`"-DCMAKE_C_COMPILER=$clangC`" `"-DCMAKE_CXX_COMPILER=$clangCxx`" `"-DGEODE_CODEGEN_CMAKE_ARGS=-DCMAKE_C_COMPILER=$clangC;-DCMAKE_CXX_COMPILER=$clangCxx;-G Ninja`""
} else {
    $cc = '-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DGEODE_CODEGEN_CMAKE_ARGS=-G Ninja"'
}
$configure = "cmake -S `"$ModDir`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release $cc -DGEODE_TARGET_PLATFORM=Win64 -DGEODE_DONT_INSTALL_MODS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev `"-DCPM_SOURCE_CACHE=$cpm`""
$compile = "cmake --build `"$build`" --config Release --parallel"
$bat = Join-Path $Root 'build-step.bat'
$pathFix = ((@($script:ExtraPaths) + @($Tools)) | Where-Object { $_ -and (Test-Path $_) }) -join ';'
@(
    '@echo off',
    "call `"$vcvars`" >nul",
    'if errorlevel 1 exit /b 1',
    "set `"PATH=$pathFix;%PATH%`"",
    $configure,
    'if errorlevel 1 exit /b 2',
    $compile,
    'if errorlevel 1 exit /b 3'
) | Set-Content -Path $bat -Encoding ASCII

Write-Host '  Esto puede tardar 5-15 minutos (baja dependencias y compila)...'
& cmd /c "`"$bat`""
$code = $LASTEXITCODE
if ($code -eq 1) { Falla 'No se pudo preparar el entorno de Visual Studio.' }
if ($code -eq 2) { Falla 'Fallo la configuracion de CMake (mira los mensajes de arriba).' }
if ($code -ne 0) { Falla 'Fallo la compilacion (mira los mensajes de arriba).' }

$geodeFile = Get-ChildItem -Path $build -Filter '*.geode' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $geodeFile) { Falla 'La compilacion termino pero no aparecio el archivo .geode.' }
Write-Host "  OK: $($geodeFile.FullName)"

# ---------------------------------------------------------------- 8
Paso 8 'Instalando en Geometry Dash'
$desktop = [Environment]::GetFolderPath('Desktop')
Copy-Item -Force $geodeFile.FullName (Join-Path $desktop $geodeFile.Name)
Write-Host "  Copia guardada en el Escritorio: $($geodeFile.Name)"

$libs = New-Object System.Collections.Generic.List[string]
try {
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
    if ($steam) {
        $steam = $steam -replace '/', '\'
        $libs.Add($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs.Add(($m.Groups[1].Value -replace '\\\\', '\'))
            }
        }
    }
} catch {}
$libs.Add('C:\Program Files (x86)\Steam')
foreach ($d in 'D', 'E', 'F') { $libs.Add("${d}:\SteamLibrary"); $libs.Add("${d}:\Steam") }

$instalado = $false
foreach ($lib in ($libs | Select-Object -Unique)) {
    $gd = Join-Path $lib 'steamapps\common\Geometry Dash'
    if (Test-Path (Join-Path $gd 'GeometryDash.exe')) {
        if (-not (Test-Path (Join-Path $gd 'geode'))) {
            Write-Host "  Encontre Geometry Dash en $gd pero no tiene Geode instalado." -ForegroundColor Yellow
            continue
        }
        $mods = Join-Path $gd 'geode\mods'
        New-Item -ItemType Directory -Force -Path $mods | Out-Null
        Copy-Item -Force $geodeFile.FullName (Join-Path $mods $geodeFile.Name)
        Write-Host "  Instalado en: $mods" -ForegroundColor Green
        $instalado = $true
        break
    }
}

Write-Host ''
Write-Host '==============================================' -ForegroundColor Green
if ($instalado) {
    Write-Host ' LISTO! Abri Geometry Dash: TickSaver ya esta en tus mods.' -ForegroundColor Green
} else {
    Write-Host ' LISTO! El mod esta en tu Escritorio.' -ForegroundColor Green
    Write-Host ' No encontre la carpeta de Geometry Dash: copia el archivo'
    Write-Host ' local.ticksaver.geode a la carpeta geode\mods de tu juego.'
}
Write-Host ' F6 = activar/desactivar   F7 = cambiar intensidad'
Write-Host '==============================================' -ForegroundColor Green
try { Stop-Transcript | Out-Null } catch {}
exit 0
