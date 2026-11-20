# Dot-source: . host\locks.ps1 (caller may set $script:LockRoot for isolated tests).
# Format: "<owner> pid=<pid> <MM/dd/yyyy HH:mm:ss> <what>"; CreateNew is atomic.
# Stale = dead PID or older than 20 minutes; unfinished files get 12 seconds.
. (Join-Path $PSScriptRoot 'safe_rm.ps1')   # Remove-TreeSafe: the delete that never follows a junction
if (-not $script:LockRoot) { $script:LockRoot = if ($root) { "$root" } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path } }
$script:LockMax = 4
$script:LockPhys = 8   # physical run slots run0..run7; build\slots\max.txt caps how many are open (default 4)
if (-not $script:LockMutexWaitMs) { $script:LockMutexWaitMs = 10000 }
if (-not $script:LockMutexName) { $script:LockMutexName = 'Global\SnapRecompLocks' }

# Readers share everything, Delete included. Get-Content does not: while it had a lock file open, the owner's Remove-Item failed (silently),
# the lock stayed behind under a live PID, and every waiter sat until its timeout. Waiters read these files on every poll, so more waiters
# meant more collisions. A rewrite of max.txt failed the same way.
function Open-LockRead([string]$Path) {
    [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, ([System.IO.FileShare]'ReadWrite, Delete'))
}
function Get-LockText([string]$Path) {
    $fs = $null
    try {
        $fs = Open-LockRead $Path
        $sr = New-Object System.IO.StreamReader($fs)
        return (($sr.ReadToEnd().TrimEnd([char]13, [char]10)) -replace '\r?\n', ' ')
    } catch { return $null } finally { if ($fs) { $fs.Dispose() } }
}
# Remove a lock file and make sure it is gone. A leftover handle without Delete sharing (an editor, a scanner) gets a short retry.
function Remove-LockFile([string]$Path) {
    foreach ($try in 1..40) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $Path)) { return $true }
        Start-Sleep -Milliseconds 25
    }
    return $false
}
# Writes build\slots\max.txt (1..8; empty text = back to the default). A new file is moved over the old one, which works while waiters read it.
function Set-RunSlotCap([string]$Text) {
    $dir = Join-Path $script:LockRoot 'build\slots'; New-Item -ItemType Directory -Force $dir | Out-Null
    $tmp = Join-Path $dir ("max.$PID.tmp")
    [System.IO.File]::WriteAllText($tmp, $Text)
    foreach ($try in 1..40) {
        try { Move-Item -LiteralPath $tmp -Destination (Join-Path $dir 'max.txt') -Force -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 25 }
    }
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    throw 'max.txt could not be replaced'
}
function Get-RunSlotCap {
    $cap = 0
    $text = Get-LockText (Join-Path $script:LockRoot 'build\slots\max.txt')
    $script:LockMax = if ($null -ne $text -and [int]::TryParse($text.Trim(), [ref]$cap) -and $cap -ge 1 -and $cap -le $script:LockPhys) { $cap } else { 4 }
    return $script:LockMax
}

# Production process guard unchanged, extracted so tests can replace it.
function Test-SlotGameRunning([string]$SlotDir) {
    return [bool]@(Get-Process snap_boot -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($SlotDir, [StringComparison]::OrdinalIgnoreCase) }).Count
}
function Test-LockStale([string]$Path) {
    $t = Get-LockText $Path
    if ($null -eq $t) { return $false }
    if ((Split-Path $Path -Leaf) -match '^run(\d+)\.lock$') {
        $slotDir = Join-Path $script:LockRoot "build\runs\slot$($Matches[1])\"
        if (Test-SlotGameRunning $slotDir) { return $false }
    }
    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $false }
    $age = ([DateTime]::UtcNow - $item.LastWriteTimeUtc).TotalMinutes
    if ($age -gt 20) { return $true }
    if ($t -match 'pid=(\d+)') {
        $ownerPid = 0
        if ([int]::TryParse($Matches[1], [ref]$ownerPid) -and $ownerPid -gt 0) { return -not [bool](Get-Process -Id $ownerPid -ErrorAction SilentlyContinue) }
    }
    return ($age -gt 0.2)
}

# A timed-out WaitOne does not grant ownership; an abandoned mutex does.
function Invoke-LockMutex([scriptblock]$Action) {
    $mx = New-Object System.Threading.Mutex($false, $script:LockMutexName)
    $owns = $false
    try {
        try { $owns = $mx.WaitOne($script:LockMutexWaitMs) } catch [System.Threading.AbandonedMutexException] { $owns = $true }
        if (-not $owns) { throw 'lock mutex timeout' }
        & $Action
    } finally { if ($owns) { $mx.ReleaseMutex() }; $mx.Dispose() }
}
function Write-LockRecord($Stream, [byte[]]$Bytes) { $Stream.Write($Bytes, 0, $Bytes.Length) }
function Try-TakeLock([string]$Path, [string]$What) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    try {
        Invoke-LockMutex {
            if ((Test-Path -LiteralPath $Path) -and (Test-LockStale $Path)) { [void](Remove-LockFile $Path) }
            $fs = $null
            try {
                $owner = if ($env:SNAP_LOCK_OWNER) { $env:SNAP_LOCK_OWNER } else { 'run_boot' }
                $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
                $b = [System.Text.Encoding]::UTF8.GetBytes("$owner pid=$PID $(Get-Date -Format 'MM/dd/yyyy HH:mm:ss') $What")
                Write-LockRecord $fs $b
                return $true
            } catch {
                # This attempt created the file and still holds the mutex. Do not
                # leave an empty/partial lock when writing its record fails.
                if ($fs) { $fs.Dispose(); $fs = $null; Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }
                return $false
            } finally { if ($fs) { $fs.Dispose() } }
        }
    } catch { return $false }
}
function Release-Lock([string]$Path) {
    try {
        Invoke-LockMutex {
            if ((Get-LockText $Path) -match "(?:^| )pid=$PID(?: |$)") { [void](Remove-LockFile $Path) }
        }
    } catch { } # Never delete an unverified lock after a mutex timeout.
}
function Take-BuildLock([string]$What = 'build', [int]$WaitSec = 1800) {
    $p = Join-Path $script:LockRoot 'build\heavy.lock'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $WaitSec) { if (Try-TakeLock $p $What) { return $true }; Start-Sleep -Seconds 1 }
    return $false
}
function Get-SlotOwners {
    (0..($script:LockPhys - 1) | ForEach-Object { $f = Join-Path $script:LockRoot "build\slots\run$_.lock"; "run$_=" + $(if (Test-Path $f) { Get-LockText $f } else { 'free' }) }) -join ' | '
}

function Get-LiveQueueTickets([string]$QueueDir) {
    foreach ($f in @(Get-ChildItem -LiteralPath $QueueDir -Filter '*.tkt' -ErrorAction SilentlyContinue | Sort-Object Name)) {
        $ownerPid = 0; $stamp = 0L
        $valid = $f.BaseName -match '^(\d{19})_(\d+)_(x|n)$'
        if ($valid) { $valid = [long]::TryParse($Matches[1], [ref]$stamp) -and $stamp -lt [long]::MaxValue -and [int]::TryParse($Matches[2], [ref]$ownerPid) -and $ownerPid -gt 0 }
        if (-not $valid -or -not (Get-Process -Id $ownerPid -ErrorAction SilentlyContinue) -or ([DateTime]::UtcNow - $f.LastWriteTimeUtc).TotalHours -gt 3) {
            Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
        } else { $f }
    }
}
# Same ticks_PID_kind format; sequence under the mutex survives wall-clock rollback.
function New-QueueTicket([string]$Dir, [bool]$Excl) {
    $q = Join-Path $Dir 'q'; New-Item -ItemType Directory -Force $q | Out-Null
    Invoke-LockMutex {
        $stamp = [DateTime]::UtcNow.Ticks
        foreach ($f in @(Get-LiveQueueTickets $q)) {
            $previous = [long](($f.BaseName -split '_')[0])
            if ($previous -ge $stamp) { $stamp = $previous + 1 }
        }
        $t = Join-Path $q ('{0:D19}_{1}_{2}.tkt' -f $stamp, $PID, $(if ($Excl) { 'x' } else { 'n' }))
        Set-Content -LiteralPath $t -Value $PID -Encoding ascii
        return $t
    }
}
function Test-QueueAhead([string]$Ticket, [bool]$ExclOnly) {
    $me = Split-Path $Ticket -Leaf
    foreach ($f in @(Get-LiveQueueTickets (Split-Path $Ticket))) {
        if ([StringComparer]::Ordinal.Compare($f.Name, $me) -lt 0 -and (-not $ExclOnly -or $f.BaseName.EndsWith('_x'))) { return $true }
    }
    return $false
}

# One slot (0..cap-1), or -Exclusive every slot except live above-cap pc-in-use
# reservations. Return only indices actually acquired; $null on timeout.
# Above-cap free slots are taken; any other live owner still blocks exclusivity.
function Enter-RunSlots([string]$What, [switch]$Exclusive, [int]$WaitSec = 3600) {
    if ($script:SlotsHeld -and @($script:SlotsHeld).Count -gt 0) { throw 'Exit-RunSlots before acquiring another run slot' }
    $dir = Join-Path $script:LockRoot 'build\slots'; New-Item -ItemType Directory -Force $dir | Out-Null
    $xl = Join-Path $dir 'exclusive.lock'; $prio = Join-Path $script:LockRoot 'build\heavy.priority'
    $got = @(); $haveX = $false; $success = $false; $script:ExclusiveHeld = $false
    $lastMsg = [DateTime]::MinValue; $tkt = $null; $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        $tkt = New-QueueTicket $dir ([bool]$Exclusive)
        while ($timer.Elapsed.TotalSeconds -lt $WaitSec) {
            $cap = Get-RunSlotCap
            $reserved = Get-LockText $prio
            if ($reserved) { $reserved = $reserved.Trim() }
            $isOwner = $reserved -and $env:SNAP_LOCK_OWNER -and ($env:SNAP_LOCK_OWNER.Trim() -eq $reserved)
            if ($Exclusive) {
                if ($haveX -and ((Get-LockText $xl) -notmatch "(?:^| )pid=$PID(?: |$)" -or ($reserved -and -not $isOwner))) {
                    foreach ($n in $got) { Release-Lock (Join-Path $dir "run$n.lock") }
                    Release-Lock $xl; $got = @(); $haveX = $false
                }
                if (-not ($reserved -and -not $isOwner)) {
                    if (-not $haveX -and -not (Test-QueueAhead $tkt $false)) { $haveX = Try-TakeLock $xl "$What (exclusive)" }
                    if ($haveX) {
                        $got = @($got | Where-Object { (Get-LockText (Join-Path $dir "run$_.lock")) -match "(?:^| )pid=$PID(?: |$)" })
                        $parked = @()
                        foreach ($n in 0..($script:LockPhys - 1)) {
                            if ($got -contains $n) { continue }
                            $path = Join-Path $dir "run$n.lock"
                            if ($n -ge $cap -and (Get-LockText $path) -match '^pc-in-use pid=\d+(?: |$)' -and -not (Test-LockStale $path)) {
                                $parked += $n
                            } elseif (Try-TakeLock $path "$What (exclusive)") { $got += $n }
                        }
                        if ($got.Count + $parked.Count -eq $script:LockPhys -and (Get-RunSlotCap) -eq $cap) {
                            $script:SlotsHeld = $got; $script:ExclusiveHeld = $true; $success = $true; return $got
                        }
                    }
                }
            } elseif ((-not (Test-Path -LiteralPath $xl) -or (Test-LockStale $xl)) -and ($isOwner -or -not (Test-QueueAhead $tkt $true))) {
                foreach ($n in 0..($cap - 1)) {
                    if ($n -eq 0 -and $reserved -and -not $isOwner) { continue }
                    if ($n -ge (Get-RunSlotCap)) { break }
                    if (Try-TakeLock (Join-Path $dir "run$n.lock") $What) {
                        # The cap may have fallen while the mutex or stale check waited.
                        if ($n -ge (Get-RunSlotCap)) { Release-Lock (Join-Path $dir "run$n.lock"); break }
                        $script:SlotsHeld = @($n); $success = $true; return @($n)
                    }
                }
            }
            if (([DateTime]::UtcNow - $lastMsg).TotalSeconds -ge 30) { $lastMsg = [DateTime]::UtcNow; Write-Host "waiting for run slot$(if ($Exclusive) { 's (exclusive)' }): $(Get-SlotOwners)" }
            Start-Sleep -Milliseconds 500
        }
        return $null
    } finally {
        if ($tkt) { Remove-Item -LiteralPath $tkt -Force -ErrorAction SilentlyContinue }
        if (-not $success) { foreach ($n in $got) { Release-Lock (Join-Path $dir "run$n.lock") }; if ($haveX) { Release-Lock $xl } }
    }
}
function Exit-RunSlots {
    $dir = Join-Path $script:LockRoot 'build\slots'
    foreach ($n in @($script:SlotsHeld)) { Release-Lock (Join-Path $dir "run$n.lock") }
    if ($script:ExclusiveHeld) { Release-Lock (Join-Path $dir 'exclusive.lock') }
    $script:SlotsHeld = @(); $script:ExclusiveHeld = $false
}
