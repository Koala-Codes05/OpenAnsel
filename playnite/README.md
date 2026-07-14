# OpenAnsel Playnite Integration Extension

This extension integrates Playnite with OpenAnsel, automatically copying the appropriate proxy DLL (`d3d9.dll` for DX9 or `dxgi.dll` for DX11/12) next to the game's executable when launched, and deleting it when the game exits to keep game folders clean.

## Setup Instructions

### 1. Symbolic Link (Recommended for Development)
Run the following PowerShell command to link the Playnite extensions folder directly to this repository's folder:

```powershell
New-Item -ItemType SymbolicLink -Path "$env:APPDATA\Playnite\Extensions\OpenAnsel" -Target "c:\Users\Anakin\OpenAnsel\playnite"
```

### 2. Manual Copy
If you prefer not to use a symbolic link:
1. Open `%APPDATA%\Playnite\Extensions\` in File Explorer.
2. Create a new folder named `OpenAnsel`.
3. Copy the contents of this folder (`extension.yaml`, `OpenAnselPlaynite.psm1`, etc.) into that folder.

### 3. Usage
* By default, the extension automatically matches known games by name (e.g. *Mirror's Edge*, *NieR:Automata*, *Detroit: Become Human*, etc.).
* Alternatively, you can add a Tag or Category named **`OpenAnsel`** to any game in Playnite to force the extension to run for it.
