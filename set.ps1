# ─────────────────────────────────────────────────────────────
#  novac — GitHub repo setup script
#  Run from the root of your novac project folder
# ─────────────────────────────────────────────────────────────

param(
    [string]$GitHubUsername = "",
    [string]$GitHubToken    = ""
)

if (-not $GitHubUsername) { $GitHubUsername = Read-Host "GitHub username" }
if (-not $GitHubToken)    { $GitHubToken    = Read-Host "GitHub personal access token (needs repo scope)" }

$RepoName = "novac"
$Headers  = @{
    Authorization = "Bearer $GitHubToken"
    Accept        = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
}

# ── 1. Create .gitignore ──────────────────────────────────────
Write-Host "`n[1/5] Writing .gitignore..." -ForegroundColor Cyan

$gitignore = @"
# Build output dirs
build/
build_*/
out/

# Compiled binaries
*.exe
*.dll
*.dll.a
*.so
*.dylib
*.a
*.o
*.obj

# CMake generated
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
Makefile
*.cmake
!CMakeLists.txt

# IDE / OS
.vscode/
.idea/
*.user
.DS_Store
Thumbs.db

# REPL history
.nova.history*
"@
[System.IO.File]::WriteAllText("$PWD\.gitignore", $gitignore)

# ── 2. Create GitHub Actions workflow ─────────────────────────
Write-Host "[2/5] Writing GitHub Actions workflow..." -ForegroundColor Cyan

New-Item -ItemType Directory -Force -Path ".github/workflows" | Out-Null

$e = '$'
$workflow = @"
name: Build

on:
  push:
    branches: [ master, main ]
  pull_request:
  workflow_dispatch:

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: Linux x86-64
            os: ubuntu-latest
            artifact: novac-linux-x86_64

          - name: Windows x86-64
            os: windows-latest
            artifact: novac-windows-x86_64

          - name: macOS x86-64
            os: macos-13
            artifact: novac-macos-x86_64

          - name: macOS ARM64
            os: macos-latest
            artifact: novac-macos-arm64

          - name: Linux ARM64 (cross)
            os: ubuntu-latest
            artifact: novac-linux-arm64
            cross: true

    name: ${e}{{ matrix.name }}
    runs-on: ${e}{{ matrix.os }}

    steps:
      - uses: actions/checkout@v4

      - name: Install deps (Linux native)
        if: runner.os == 'Linux' && !matrix.cross
        run: sudo apt-get update && sudo apt-get install -y libffi-dev libssl-dev cmake

      - name: Install deps (Linux ARM64 cross)
        if: matrix.cross
        run: |
          sudo apt-get update
          sudo dpkg --add-architecture arm64 || true
          sudo apt-get update || true
          sudo apt-get install -y cmake gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
          sudo apt-get install -y libffi-dev:arm64 libssl-dev:arm64 || true

      - name: Write ARM64 toolchain file
        if: matrix.cross
        run: |
          mkdir -p .github/toolchains
          cat > .github/toolchains/linux-arm64.cmake << 'EOF'
          set(CMAKE_SYSTEM_NAME Linux)
          set(CMAKE_SYSTEM_PROCESSOR aarch64)
          set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
          set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
          set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
          set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
          set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
          EOF

      - name: Install deps (macOS)
        if: runner.os == 'macOS'
        run: brew install libffi openssl cmake

      - name: Install deps (Windows)
        if: runner.os == 'Windows'
        run: choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y

      - name: Configure (native)
        if: '!matrix.cross'
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release

      - name: Configure (cross ARM64)
        if: matrix.cross
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=.github/toolchains/linux-arm64.cmake

      - name: Build
        run: cmake --build build --config Release -j4

      - name: Collect binaries (Unix)
        if: runner.os != 'Windows'
        run: |
          mkdir dist
          cp build/novac dist/ 2>/dev/null || cp build/Release/novac dist/ 2>/dev/null || true
          cp "build/nv+" dist/ 2>/dev/null || cp "build/Release/nv+" dist/ 2>/dev/null || true

      - name: Collect binaries (Windows)
        if: runner.os == 'Windows'
        run: |
          New-Item -ItemType Directory -Force dist
          Copy-Item build\Release\novac.exe dist\ -ErrorAction SilentlyContinue
          Copy-Item "build\Release\nv+.exe"  dist\ -ErrorAction SilentlyContinue

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: ${e}{{ matrix.artifact }}
          path: dist/
          if-no-files-found: warn
"@
[System.IO.File]::WriteAllText("$PWD\.github\workflows\build.yml", $workflow)

# ── 3. Git init & commit ──────────────────────────────────────
Write-Host "[3/5] Initialising git and committing..." -ForegroundColor Cyan

git init
git add .
git commit -m "initial commit"

# ── 4. Create GitHub repo ─────────────────────────────────────
Write-Host "[4/5] Creating GitHub repository '$RepoName'..." -ForegroundColor Cyan

$body = @{ name = $RepoName; private = $false; auto_init = $false } | ConvertTo-Json

$response = Invoke-RestMethod `
    -Uri "https://api.github.com/user/repos" `
    -Method POST `
    -Headers $Headers `
    -Body $body `
    -ContentType "application/json"

$repoUrl = $response.clone_url
Write-Host "  Repo created: $($response.html_url)" -ForegroundColor Green

# ── 5. Push ───────────────────────────────────────────────────
Write-Host "[5/5] Pushing to GitHub..." -ForegroundColor Cyan

$authUrl = $repoUrl -replace "https://", "https://${GitHubUsername}:${GitHubToken}@"

git remote add origin $authUrl
git branch -M master
git push -u origin master

Write-Host "`nDone! Repo live at: https://github.com/$GitHubUsername/$RepoName" -ForegroundColor Green
Write-Host "Actions will kick off automatically." -ForegroundColor Green