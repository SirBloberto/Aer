# Lines of C source/headers -- the "easy to understand" axis. Run from the repo root:
#   powershell -File bench/loc_report.ps1
$files = Get-ChildItem -Recurse -Include *.c,*.h -Path source,include -File
$total = 0
foreach ($f in $files) {
    $n = (Get-Content $f.FullName | Measure-Object -Line).Lines
    $total += $n
}
Write-Host ("{0} files, {1} total lines (source/ + include/)" -f $files.Count, $total)
