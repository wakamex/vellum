# Installs vellum for the current user and makes it what opens .md and .markdown files:
#   powershell -ExecutionPolicy Bypass -File tools\register-windows.ps1 [-From PATH\vellum.exe]
# It copies the program to %LOCALAPPDATA%\Programs\vellum and writes the association under
# HKCU\Software\Classes, which needs no administrator. Where another program already holds .md
# as the user's own choice (Settings, Default apps), Windows keeps that choice; pick vellum there.
param([string]$From = "$PSScriptRoot\..\zig-out\bin\vellum.exe")
$dir = "$env:LOCALAPPDATA\Programs\vellum"
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $From "$dir\vellum.exe" -Force
$exe = "$dir\vellum.exe"
$cmd = "`"$exe`" `"%1`""
$c = 'HKCU:\Software\Classes'
# The kind of file vellum opens, and how.
New-Item -Force "$c\vellum.md" | Out-Null
Set-ItemProperty "$c\vellum.md" '(default)' 'Markdown document'
New-Item -Force "$c\vellum.md\DefaultIcon" | Out-Null
Set-ItemProperty "$c\vellum.md\DefaultIcon" '(default)' "`"$exe`",0"
New-Item -Force "$c\vellum.md\shell\open\command" | Out-Null
Set-ItemProperty "$c\vellum.md\shell\open\command" '(default)' $cmd
# The program itself, for Open with.
New-Item -Force "$c\Applications\vellum.exe\shell\open\command" | Out-Null
Set-ItemProperty "$c\Applications\vellum.exe" 'FriendlyAppName' 'vellum'
Set-ItemProperty "$c\Applications\vellum.exe\shell\open\command" '(default)' $cmd
New-Item -Force "$c\Applications\vellum.exe\SupportedTypes" | Out-Null
foreach ($e in '.md', '.markdown') {
  Set-ItemProperty "$c\Applications\vellum.exe\SupportedTypes" $e ''
  New-Item -Force "$c\$e" | Out-Null
  Set-ItemProperty "$c\$e" '(default)' 'vellum.md'
  New-Item -Force "$c\$e\OpenWithProgids" | Out-Null
  Set-ItemProperty "$c\$e\OpenWithProgids" 'vellum.md' ([byte[]]@()) -Type Binary
}
# Tell Explorer that associations changed.
Add-Type -Namespace W -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, System.IntPtr a, System.IntPtr b);'
[W.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
"installed: $exe"
"open command: " + (Get-ItemProperty "$c\vellum.md\shell\open\command").'(default)'
"assoc says: " + (cmd /c assoc .md)
# What Windows itself resolves for .md, through the same lookup Explorer uses.
Add-Type -Namespace W -Name Assoc -MemberDefinition '[DllImport("shlwapi.dll", CharSet = CharSet.Unicode)] public static extern int AssocQueryString(int f, int s, string a, string v, System.Text.StringBuilder o, ref uint n);'
$sb = New-Object System.Text.StringBuilder 1024; [uint32]$n = 1024
[void][W.Assoc]::AssocQueryString(0, 1, '.md', 'open', $sb, [ref]$n)
"Windows opens .md with: " + $sb.ToString()
