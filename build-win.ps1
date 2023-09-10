# Set ErrorActionPreference to "Stop" to make the script stop on the first error
$ErrorActionPreference = "Stop"

# Get the path of uetools.uproject
$root = $(Split-Path $PSCommandPath -Parent)
$proj_file = Join-Path -Path $root -ChildPath "uetools.uproject"

# Check if the proj_file exists
if(!(Test-Path $proj_file))
{
    Write-Error "The file '$proj_file' does not exist!"
    exit 1
}

# Make directory for build
if(!(Test-Path "Build"))
{
    New-Item -Path "Build" -ItemType Directory
}

Write-Output "Building PakTools..."
ue4 build-target $proj_file PakTools Win64 Development

# Copy binaries to build directory
Write-Output "Copying binaries..."
Copy-Item -Path "Binaries/Win64/PakTools.exe" -Destination "Build/PakTools.exe"
