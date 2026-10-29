# Prefer this PowerShell installation's standard modules even when launched
# from a host that supplies its own PSModulePath. This affects this process only.
$env:PSModulePath = (Join-Path $PSHOME 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
