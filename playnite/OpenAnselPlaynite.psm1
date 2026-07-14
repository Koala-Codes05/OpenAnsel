# OpenAnselPlaynite.psm1
# Playnite Script Extension for automatic OpenAnsel injection and cleanup.

function Log-Message($msg) {
    $logPath = [System.IO.Path]::Combine($PSScriptRoot, "OpenAnselPlaynite.log")
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    "[$timestamp] $msg" | Out-File -FilePath $logPath -Append -Encoding utf8
}

function Get-StateFilePath {
    return [System.IO.Path]::Combine($PSScriptRoot, "active_injections.json")
}

function Load-State {
    $path = Get-StateFilePath
    if (Test-Path $path) {
        try {
            $content = Get-Content -Path $path -Raw
            return ConvertFrom-Json $content -ErrorAction Stop
        } catch {
            return @{}
        }
    }
    return @{}
}

function Save-State($state) {
    $path = Get-StateFilePath
    $content = ConvertTo-Json $state -Depth 5
    $content | Out-File -FilePath $path -Encoding utf8
}

function Add-Injection($gameId, $proxyDllPath) {
    $state = Load-State
    $state.$gameId = @{
        ProxyDllPath = $proxyDllPath
    }
    Save-State $state
}

function Remove-Injection($gameId) {
    $state = Load-State
    $strGameId = $gameId.ToString()
    if ($state.PSObject.Properties[$strGameId]) {
        $info = $state.$strGameId
        # Remove from state
        $newState = @{}
        foreach ($prop in $state.PSObject.Properties) {
            if ($prop.Name -ne $strGameId) {
                $newState[$prop.Name] = $prop.Value
            }
        }
        Save-State $newState
        return $info.ProxyDllPath
    }
    return $null
}

function Resolve-GameExePath($game) {
    if (-not $game.GameActions) { return $null }
    foreach ($action in $game.GameActions) {
        if ($action.IsPlayAction -and $action.Type -eq "File") {
            $path = $action.Path
            $path = $PlayniteApi.ExpandGameVariables($game, $path)
            if ([System.IO.Path]::IsPathRooted($path)) {
                return $path
            }
            return [System.IO.Path]::Combine($game.InstallDirectory, $path)
        }
    }
    return $null
}

function Find-GameExe($game) {
    $path = Resolve-GameExePath($game)
    if ($path -and (Test-Path $path)) {
        return $path
    }
    
    if ($game.InstallDirectory -and (Test-Path $game.InstallDirectory)) {
        $knownExes = @("bg3.exe", "bg3_dx11.exe", "Cyberpunk2077.exe", "DetroitBecomeHuman.exe", "eldenring.exe", "MirrorsEdge.exe", "NeedForSpeedHeat.exe", "NieR Replicant ver.1.22474487139.exe", "ROTTR.exe", "SB-Win64-Shipping.exe", "tlou-i.exe", "Dispatch-Win64-Shipping.exe", "Haste.exe", "LushfoilSim-Win64-Shipping.exe", "NieRAutomata.exe")
        foreach ($exe in $knownExes) {
            $found = Get-ChildItem -Path $game.InstallDirectory -Filter $exe -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($found) {
                return $found.FullName
            }
        }
    }
    return $null
}

function Get-ProxyDllName($exeName) {
    if ($exeName -like "*MirrorsEdge*" -or $exeName -like "*Mirror's Edge*") {
        return "d3d9.dll"
    }
    # Default to dxgi.dll for other modern games (DX11/12)
    return "dxgi.dll"
}

function Find-SourceDll($dllName) {
    $repoRoots = @(
        [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($PSScriptRoot, "..")),
        "c:\Users\Anakin\OpenAnsel"
    )
    $configs = @("Release", "Debug", "MinSizeRel", "RelWithDebInfo", "")
    
    foreach ($repoRoot in $repoRoots) {
        if ($dllName -eq "d3d9.dll") {
            # DX9 backend is x86 - built in the 'build' directory (Win32 / x86)
            foreach ($config in $configs) {
                $path = if ($config) {
                    [System.IO.Path]::Combine($repoRoot, "build", "src", "backends", "dx9", $config, "d3d9.dll")
                } else {
                    [System.IO.Path]::Combine($repoRoot, "build", "src", "backends", "dx9", "d3d9.dll")
                }
                if (Test-Path $path) { return $path }
            }
        } elseif ($dllName -eq "dxgi.dll") {
            # DXGI backend (DX11/12) is x64 - built in the 'build64' directory
            foreach ($config in $configs) {
                $path = if ($config) {
                    [System.IO.Path]::Combine($repoRoot, "build64", "src", "backends", "dxgi", $config, "dxgi.dll")
                } else {
                    [System.IO.Path]::Combine($repoRoot, "build64", "src", "backends", "dxgi", "dxgi.dll")
                }
                if (Test-Path $path) { return $path }
            }
        }
    }
    return $null
}

# --- Event Handlers ---

function OnGameStarting($args) {
    $game = $args.Game
    
    # 1. Determine if OpenAnsel is requested for this game.
    # Check if the game has the "OpenAnsel" tag or category.
    $isOpenAnselRequested = $false
    if ($game.Tags) {
        foreach ($tag in $game.Tags) {
            if ($tag.Name -eq "OpenAnsel") { $isOpenAnselRequested = $true; break }
        }
    }
    if (-not $isOpenAnselRequested -and $game.Categories) {
        foreach ($cat in $game.Categories) {
            if ($cat.Name -eq "OpenAnsel") { $isOpenAnselRequested = $true; break }
        }
    }
    
    # Also check if it's one of the known games automatically
    if (-not $isOpenAnselRequested) {
        $knownGames = @("Mirror's Edge", "Mirrors Edge", "NieR:Automata", "NieR Automata", "NieR Replicant", "Detroit: Become Human", "Detroit Become Human", "Baldur's Gate 3", "Baldurs Gate 3", "Rise of the Tomb Raider", "Clair Obscur", "Stellar Blade", "The Last of Us", "Dispatch", "Lushfoil Photography Sim")
        foreach ($g in $knownGames) {
            if ($game.Name -like "*$g*") {
                $isOpenAnselRequested = $true
                Log-Message "Auto-matched known game: $($game.Name)"
                break
            }
        }
    }
    
    if (-not $isOpenAnselRequested) {
        return
    }
    
    Log-Message "Starting integration for game: $($game.Name)"
    
    # 2. Locate executable
    $exePath = Find-GameExe($game)
    if (-not $exePath) {
        Log-Message "Error: Could not locate game executable for $($game.Name) in $($game.InstallDirectory)"
        return
    }
    
    $exeName = [System.IO.Path]::GetFileName($exePath)
    $exeDir = [System.IO.Path]::GetDirectoryName($exePath)
    $proxyName = Get-ProxyDllName($exeName)
    
    Log-Message "Resolved Exe Path: $exePath"
    Log-Message "Required Proxy DLL: $proxyName"
    
    # 3. Find source DLL
    $sourceDll = Find-SourceDll($proxyName)
    if (-not $sourceDll) {
        Log-Message "Error: Source DLL $proxyName not found in build directories. Please build the OpenAnsel project first."
        return
    }
    
    $targetPath = [System.IO.Path]::Combine($exeDir, $proxyName)
    
    # 4. Copy and register
    try {
        Log-Message "Copying $sourceDll to $targetPath"
        Copy-Item -Path $sourceDll -Destination $targetPath -Force -ErrorAction Stop
        Add-Injection $game.Id $targetPath
        Log-Message "Successfully injected $proxyName next to $exeName"
    } catch {
        Log-Message "Exception copying proxy DLL: $_"
    }
}

function OnGameStarted($args) {
    # No action needed on started
}

function OnGameStopped($args) {
    $game = $args.Game
    
    # 1. Retrieve the target path if we injected it
    $targetPath = Remove-Injection $game.Id
    if ($targetPath) {
        Log-Message "Game stopped: $($game.Name). Cleaning up injection..."
        if (Test-Path $targetPath) {
            try {
                Remove-Item -Path $targetPath -Force -ErrorAction Stop
                Log-Message "Successfully removed proxy DLL from $targetPath"
            } catch {
                Log-Message "Exception removing proxy DLL: $_"
            }
        } else {
            Log-Message "Target DLL was already gone: $targetPath"
        }
    }
}
