# Software OpenGL for the Windows CI runners, which have no GPU (docs/WINDOWS.md).
#   install-mesa.ps1               fetch Mesa llvmpipe and register it as the system OpenGL driver
#   install-mesa.ps1 -Deploy <dir> copy its opengl32.dll next to every program under <dir>
param([string]$Deploy = "")

$ErrorActionPreference = "Stop"
$mesa = "C:\tools\mesa"

if ($Deploy -ne "") {
    # Only beside programs: the DLL is tens of MB, and a copy in every build
    # directory filled the runner's disk.
    Get-ChildItem -Path $Deploy -Recurse -Filter *.exe | Select-Object -ExpandProperty DirectoryName -Unique |
        ForEach-Object { Copy-Item "$mesa\opengl32.dll" -Destination $_ }
    exit 0
}

$url = "https://github.com/mmozeiko/build-mesa/releases/download/26.2.5/mesa-llvmpipe-x64-26.2.5.7z"
Invoke-WebRequest -Uri $url -OutFile "mesa.7z"
7z x mesa.7z -o"$mesa"
# System-wide OpenGL ICD registration for headless Windows Server VMs.
Copy-Item "$mesa\opengl32.dll" -Destination "C:\Windows\System32\mesadrv.dll" -Force
$key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL"
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name "DLL" -Value "mesadrv.dll"
Set-ItemProperty -Path $key -Name "DriverVersion" -Type DWord -Value 1
Set-ItemProperty -Path $key -Name "Flags" -Type DWord -Value 1
Set-ItemProperty -Path $key -Name "Version" -Type DWord -Value 2
"GALLIUM_DRIVER=llvmpipe" >> $env:GITHUB_ENV
"CUERVO_PARITY_VULKAN=0" >> $env:GITHUB_ENV
"$mesa" >> $env:GITHUB_PATH
