# Windows version of tools/build_gen.sh + tools/redecomp.sh: regenerate src/gen/
# from original/NITE3W.EXE (Ghidra analysis + export, NE relocation scan,
# p-code recompiler, data image).
# Needs Ghidra 12.x ($env:GHIDRA_INSTALL_DIR), a JDK 21 ($env:JAVA_HOME) and Python 3.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path "$Root\original\NITE3W.EXE")) {
    throw 'original\NITE3W.EXE not found (run tools\extract_installer.py first)'
}
if (-not $env:GHIDRA_INSTALL_DIR) { throw 'set GHIDRA_INSTALL_DIR to your Ghidra folder' }
$Headless = Join-Path $env:GHIDRA_INSTALL_DIR 'support\analyzeHeadless.bat'
if (-not (Test-Path $Headless)) { throw "$Headless not found" }
$env:PYTHONUTF8 = '1' # the scripts read and write UTF-8, not the ANSI code page
# Native tools report problems by exit code. Their stderr (the recompiler's
# warnings, Ghidra's log) must not stop the script, which Windows PowerShell
# does under 'Stop' as soon as that output is redirected.
function Run-Native([string]$exe) {
    $ErrorActionPreference = 'Continue'
    & $exe @args
    if ($LASTEXITCODE) { throw "$exe $args failed (exit $LASTEXITCODE)" }
}
function Run-Python { Run-Native python -I @args }

New-Item -ItemType Directory -Force "$Root\ghidra_proj", "$Root\decomp", "$Root\recomp\export" | Out-Null
$log = "$Root\ghidra_proj\log.txt"
try {
    Run-Native $Headless "$Root\ghidra_proj" nite3w -import "$Root\original\NITE3W.EXE" -overwrite `
        -scriptPath "$Root\tools\ghidra_scripts" `
        -postScript FindMissingFuncs.java `
        -postScript ApplyNames.java "$Root\re\names.txt" `
        -postScript ExportDecomp.java "$Root\decomp" `
        -postScript ExportAsm.java "$Root\decomp" `
        -postScript ExportRecomp.java "$Root\recomp\export" *> $log
} catch { Get-Content $log -Tail 30; throw 'Ghidra failed' }
Select-String -Path $log -CaseSensitive -Pattern 'ApplyNames.java>|FindMissingFuncs.java>|ERROR' | ForEach-Object { $_.Line }

Run-Python "$Root\tools\annotate.py" "$Root\decomp\decomp.c" "$Root\re\nite3w.c"
Run-Python "$Root\tools\annotate.py" "$Root\decomp\nite3w.asm" "$Root\re\nite3w.asm"
Run-Python "$Root\tools\ne_relocs.py" | Out-Null
Run-Python "$Root\tools\recomp.py"
Run-Python "$Root\tools\gen_data.py"
