# Plain PowerShell 5.1 tests; no Pester, game, or project build needed.
# powershell -NoProfile -File host/tests/locks.Tests.ps1 -FixtureRoot <scratch>
param([string]$FixtureRoot = (Join-Path $PSScriptRoot ('lock-fixtures-' + [guid]::NewGuid().ToString('N'))),
      [string]$LocksScript = (Join-Path $PSScriptRoot '..\locks.ps1'),
      [string]$Child = '', [string]$Label = '',
      [string]$MutexName = ('Local\SnapRecompLocksTests_' + [guid]::NewGuid().ToString('N')))
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$FixtureRoot = [IO.Path]::GetFullPath($FixtureRoot)
New-Item -ItemType Directory -Force $FixtureRoot | Out-Null
$script:LockRoot = $FixtureRoot
$script:LockMutexName = $MutexName
. $LocksScript
# Never enumerate or inspect any real game processes during tests.
function Test-SlotGameRunning([string]$SlotDir) { return $false }
function Wait-For([scriptblock]$Condition, [string]$Message, [int]$Seconds = 10) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 40
    }
    throw "timeout: $Message"
}
if ($Child) {
    try {
        if ($Child -eq 'mutex') {
            $mx = New-Object System.Threading.Mutex($false, $script:LockMutexName)
            [void]$mx.WaitOne()
            try { Set-Content (Join-Path $FixtureRoot 'ready') 'ready'; Wait-For { Test-Path (Join-Path $FixtureRoot 'release') } 'mutex release' }
            finally { $mx.ReleaseMutex(); $mx.Dispose() }
        } elseif ($Child -eq 'race') {
            Set-Content (Join-Path $FixtureRoot "ready-$PID") 'ready'
            Wait-For { Test-Path (Join-Path $FixtureRoot 'go') } 'race start'
            $path = Join-Path $FixtureRoot 'race.lock'
            $won = Try-TakeLock $path 'race'
            Set-Content (Join-Path $FixtureRoot "result-$PID") ([int]$won)
            Wait-For { Test-Path (Join-Path $FixtureRoot 'release') } 'race release'
            if ($won) { Release-Lock $path }
        } else {
            $slots = Enter-RunSlots $Label -Exclusive:($Child -eq 'exclusive') -WaitSec 8
            if ($null -eq $slots) { throw 'slot acquisition timed out' }
            Add-Content (Join-Path $FixtureRoot 'order.txt') $Label
            Set-Content (Join-Path $FixtureRoot "result-$PID") ($slots -join ',')
            Wait-For { Test-Path (Join-Path $FixtureRoot "release-$PID") } 'slot release'
            Exit-RunSlots
        }
        exit 0
    } catch { Set-Content (Join-Path $FixtureRoot "error-$PID") $_.Exception.Message; exit 1 }
}

$script:Children = @(); $script:Checks = 0; $script:Cases = 0
function Assert([bool]$Value, [string]$Message) {
    $script:Checks++
    if (-not $Value) { throw "FAIL: $Message" }
}
function Fresh([string]$Name) {
    $script:Cases++
    $script:LockRoot = Join-Path $FixtureRoot $Name
    $script:SlotsHeld = @(); $script:ExclusiveHeld = $false
    $env:SNAP_LOCK_OWNER = 'test'
    New-Item -ItemType Directory -Force (Join-Path $script:LockRoot 'build\slots\q') | Out-Null
}
function Path([string]$Name) { Join-Path $script:LockRoot $Name }
function Spawn([string]$Mode, [string]$Name = '') {
    $args = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -FixtureRoot "{1}" -LocksScript "{2}" -Child {3} -Label "{4}" -MutexName "{5}"' -f $PSCommandPath, $script:LockRoot, $LocksScript, $Mode, $Name, $script:LockMutexName
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $args -WindowStyle Hidden -PassThru
    $script:Children += $p
    return $p
}
function Finish($Process) {
    Assert ($Process.WaitForExit(12000)) 'child exited promptly'
    Assert ($Process.ExitCode -eq 0) "child exit code $($Process.ExitCode)"
}
function Wait-Ticket($Process) {
    Wait-For { @(Get-ChildItem (Path 'build\slots\q') -Filter "*_$($Process.Id)_*.tkt").Count -gt 0 } 'child queued'
}

try {
    Fresh 'atomic'
    $p = Path 'simple.lock'
    Assert (Try-TakeLock $p 'first') 'first atomic take'
    Assert (-not (Try-TakeLock $p 'second')) 'second take denied'
    Release-Lock $p
    Assert (-not (Test-Path $p)) 'release removes owned lock'
    Set-Content $p 'other pid=2147483647 test'
    Release-Lock $p
    Assert (Test-Path $p) 'release preserves replacement owned by another PID'
    Assert (Try-TakeLock $p 'dead') 'dead PID reclaimed'
    (Get-Item $p).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(-21)
    Assert (Test-LockStale $p) 'live PID expires after 20 minutes'
    Assert (Try-TakeLock $p 'old') 'expired live-PID lock reclaimed'
    Release-Lock $p
    Set-Content $p ''
    Assert (-not (Try-TakeLock $p 'young')) 'young half-written lock preserved'
    (Get-Item $p).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(-13)
    Assert (Try-TakeLock $p 'partial') 'old half-written lock reclaimed'
    Release-Lock $p
    Set-Content $p 'bad pid=999999999999999999999'
    (Get-Item $p).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(-13)
    Assert (Try-TakeLock $p 'malformed') 'overflow PID treated as malformed'

    # A waiter reading a lock file must not stop its owner from releasing it, and must not stop a cap rewrite (the flake under slot load:
    # Get-Content readers made Remove-Item fail silently, so the lock stayed under a live PID and the waiters timed out).
    Fresh 'reader-open'
    $p = Path 'build\slots\run0.lock'
    Assert (Try-TakeLock $p 'held') 'take lock for reader test'
    $reader = Open-LockRead $p
    try { Release-Lock $p } finally { $reader.Dispose() }
    Assert (-not (Test-Path $p)) 'release succeeds while a waiter reads the lock'
    Assert (Try-TakeLock $p 'again') 'lock can be taken again after that release'
    Release-Lock $p
    Set-RunSlotCap '2'
    $reader = Open-LockRead (Path 'build\slots\max.txt')
    try { Set-RunSlotCap '3' } finally { $reader.Dispose() }
    Assert ((Get-RunSlotCap) -eq 3) 'cap rewrite succeeds while a waiter reads it'
    Set-Content (Path 'two-lines.txt') "a`r`nb"
    Assert ((Get-LockText (Path 'two-lines.txt')) -eq 'a b') 'lock text joins lines with a space'
    Set-Content (Path 'empty.txt') ''
    Assert ((Get-LockText (Path 'empty.txt')) -eq '') 'empty file reads as empty text'
    Assert ($null -eq (Get-LockText (Path 'missing.txt'))) 'missing file reads as null'

    Fresh 'write-failure'
    function Write-LockRecord($Stream, [byte[]]$Bytes) { throw 'synthetic write failure' }
    Assert (-not (Try-TakeLock (Path 'partial.lock') 'fail')) 'write failure denies acquisition'
    Assert (-not (Test-Path (Path 'partial.lock'))) 'write failure removes its partial file'
    function Write-LockRecord($Stream, [byte[]]$Bytes) { $Stream.Write($Bytes, 0, $Bytes.Length) }

    Fresh 'game-guard'
    $p = Path 'build\slots\run0.lock'
    Set-Content $p 'dead pid=2147483647 test'
    (Get-Item $p).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(-21)
    function Test-SlotGameRunning([string]$SlotDir) { return $true }
    Assert (-not (Test-LockStale $p)) 'game guard preserves stale watcher slot'
    function Test-SlotGameRunning([string]$SlotDir) { return $false }

    Fresh 'mutex-timeout'
    $m = Spawn 'mutex'
    Wait-For { Test-Path (Path 'ready') } 'mutex holder ready'
    $script:LockMutexWaitMs = 80
    Assert (-not (Try-TakeLock (Path 'mutex.lock') 'blocked')) 'mutex timeout cannot create lock'
    Assert (-not (Test-Path (Path 'mutex.lock'))) 'no file after mutex timeout'
    $script:LockMutexWaitMs = 10000
    Set-Content (Path 'release') 'go'; Finish $m

    Fresh 'cap'
    Set-RunSlotCap '1'
    Assert ((Enter-RunSlots 'cap' -WaitSec 1) -eq 0) 'cap one uses slot zero'
    Exit-RunSlots
    Assert (Try-TakeLock (Path 'build\slots\run0.lock') 'occupy') 'occupy only capped slot'
    $w = Spawn 'normal' 'cap-raised'; Wait-Ticket $w
    Set-RunSlotCap '2'
    Wait-For { Test-Path (Path "result-$($w.Id)") } 'waiting process rereads raised cap' 5
    Assert ((Get-Content (Path "result-$($w.Id)")) -eq '1') 'waiter acquires newly allowed slot'
    Set-Content (Path "release-$($w.Id)") 'go'; Finish $w
    Release-Lock (Path 'build\slots\run0.lock')
    Set-RunSlotCap ''
    Assert ((Get-RunSlotCap) -eq 4) 'empty cap defaults to four'
    Set-RunSlotCap 'invalid'
    Assert ((Get-RunSlotCap) -eq 4) 'invalid cap defaults to four'
    Remove-Item (Path 'build\slots\max.txt')
    Assert ((Get-RunSlotCap) -eq 4) 'removed cap defaults to four'

    Fresh 'cap-reduced'
    foreach ($n in 0..3) { Assert (Try-TakeLock (Path "build\slots\run$n.lock") 'hold') 'occupy slot before cap reduction' }
    $w = Spawn 'normal' 'cap-reduced'; Wait-Ticket $w
    Set-RunSlotCap '1'
    Release-Lock (Path 'build\slots\run1.lock')
    Start-Sleep -Milliseconds 1100
    Assert (-not (Test-Path (Path "result-$($w.Id)"))) 'waiter cannot use slot above reduced cap'
    Release-Lock (Path 'build\slots\run0.lock')
    Wait-For { Test-Path (Path "result-$($w.Id)") } 'reduced-cap waiter gets slot zero'
    Assert ((Get-Content (Path "result-$($w.Id)")) -eq '0') 'reduced-cap waiter uses allowed slot'
    Set-Content (Path "release-$($w.Id)") 'go'; Finish $w
    foreach ($n in 2..3) { Release-Lock (Path "build\slots\run$n.lock") }

    Fresh 'exclusive'
    Set-RunSlotCap '1'
    $held = @(Enter-RunSlots 'exclusive' -Exclusive -WaitSec 1)
    Assert (($held -join ',') -eq '0,1,2,3,4,5,6,7') 'exclusive reserves all physical slots despite cap'
    Exit-RunSlots
    Assert (-not (Test-Path (Path 'build\slots\exclusive.lock'))) 'exclusive gate released'
    Assert (@(Get-ChildItem (Path 'build\slots') -Filter '*.lock').Count -eq 0) 'all exclusive slots released'
    Assert (Try-TakeLock (Path 'build\slots\run3.lock') 'block') 'block one slot'
    Assert ($null -eq (Enter-RunSlots 'timeout' -Exclusive -WaitSec 1)) 'partial exclusive times out'
    Assert (@(Get-ChildItem (Path 'build\slots') -Filter '*.lock').Count -eq 1) 'partial slots and gate cleaned'
    Release-Lock (Path 'build\slots\run3.lock')
    Set-Content (Path 'build\slots\exclusive.lock') 'foreign pid=2147483647 test'
    Exit-RunSlots
    Assert (Test-Path (Path 'build\slots\exclusive.lock')) 'exit does not release unacquired gate'

    Fresh 'daytime-exclusive'
    Set-RunSlotCap '1'
    $env:SNAP_LOCK_OWNER = 'pc-in-use'
    foreach ($n in 1..7) { Assert (Try-TakeLock (Path "build\slots\run$n.lock") 'closed') 'create daytime reservation' }
    $env:SNAP_LOCK_OWNER = 'test'
    $held = @(Enter-RunSlots 'daytime bench' -Exclusive -WaitSec 1)
    Assert (($held -join ',') -eq '0') 'daytime exclusive owns only slot zero'
    Assert (Test-Path (Path 'build\slots\exclusive.lock')) 'one-slot exclusive holds its gate'
    Exit-RunSlots
    Assert (-not (Test-Path (Path 'build\slots\exclusive.lock'))) 'one-slot exclusive releases its gate'
    Assert (-not (Test-Path (Path 'build\slots\run0.lock'))) 'one-slot exclusive releases slot zero'
    foreach ($n in 1..7) { Assert ((Get-LockText (Path "build\slots\run$n.lock")) -like 'pc-in-use pid=*') 'daytime reservations remain untouched' }
    $daytime = Spawn 'exclusive' 'daytime-child'
    Wait-For { Test-Path (Path "result-$($daytime.Id)") } 'daytime exclusive child acquired'
    Assert ((Get-Content (Path "result-$($daytime.Id)")) -eq '0') 'exclusive child accepts above-cap daytime holds'
    Set-Content (Path "release-$($daytime.Id)") 'go'; Finish $daytime
    Assert (-not (Test-Path (Path 'build\slots\exclusive.lock'))) 'exclusive child releases its gate'
    Set-RunSlotCap '2'
    Assert ($null -eq (Enter-RunSlots 'below-cap reservation' -Exclusive -WaitSec 1)) 'pc-in-use below cap blocks exclusive'
    Assert (-not (Test-Path (Path 'build\slots\exclusive.lock'))) 'blocked daytime bench cleans its gate'

    Fresh 'queue'
    $a = New-QueueTicket (Path 'build\slots') $false
    $b = New-QueueTicket (Path 'build\slots') $true
    $c = New-QueueTicket (Path 'build\slots') $false
    Assert (Test-QueueAhead $b $false) 'exclusive waits for older normal'
    Assert (-not (Test-QueueAhead $b $true)) 'no older exclusive before first exclusive'
    Assert (Test-QueueAhead $c $true) 'normal waits for older exclusive'
    $future = Join-Path (Path 'build\slots\q') ('{0:D19}_{1}_x.tkt' -f [DateTime]::UtcNow.AddHours(1).Ticks, $PID)
    Set-Content $future $PID
    $later = New-QueueTicket (Path 'build\slots') $false
    Assert ([StringComparer]::Ordinal.Compare((Split-Path $later -Leaf), (Split-Path $future -Leaf)) -gt 0) 'clock rollback cannot jump queue'
    $dead = Join-Path (Path 'build\slots\q') '0000000000000000001_2147483647_x.tkt'
    $bad = Join-Path (Path 'build\slots\q') 'bad.tkt'
    Set-Content $dead 'dead'; Set-Content $bad 'bad'
    (Get-Item $a).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-4)
    [void](Test-QueueAhead $later $false)
    Assert (-not (Test-Path $dead)) 'dead ticket removed'
    Assert (-not (Test-Path $bad)) 'malformed ticket removed'
    Assert (-not (Test-Path $a)) 'ticket older than three hours removed'

    Fresh 'priority'
    Set-Content (Path 'build\heavy.priority') 'reserved'
    Assert ((Enter-RunSlots 'ordinary' -WaitSec 1) -eq 1) 'other owner skips reserved slot zero'
    Exit-RunSlots
    Assert ($null -eq (Enter-RunSlots 'bench' -Exclusive -WaitSec 1)) 'foreign priority defers exclusive'
    Assert (-not (Test-Path (Path 'build\slots\exclusive.lock'))) 'deferred exclusive does not block priority owner'
    $old = New-QueueTicket (Path 'build\slots') $true
    $env:SNAP_LOCK_OWNER = 'reserved'
    Assert ((Enter-RunSlots 'owner' -WaitSec 1) -eq 0) 'priority owner can use zero ahead of deferred exclusive'
    Exit-RunSlots

    Fresh 'race'
    $racers = @(1..4 | ForEach-Object { Spawn 'race' })
    Wait-For { @(Get-ChildItem $script:LockRoot -Filter 'ready-*').Count -eq 4 } 'four racers ready'
    Set-Content (Path 'go') 'go'
    Wait-For { @(Get-ChildItem $script:LockRoot -Filter 'result-*').Count -eq 4 } 'four race results'
    $wins = @(Get-ChildItem $script:LockRoot -Filter 'result-*' | Where-Object { (Get-Content $_.FullName) -eq '1' }).Count
    Assert ($wins -eq 1) 'exactly one winner among four processes'
    Set-Content (Path 'release') 'go'; foreach ($r in $racers) { Finish $r }
    Assert (-not (Test-Path (Path 'race.lock'))) 'race winner released lock'

    Fresh 'fair-processes'
    [void](Enter-RunSlots 'hold' -Exclusive -WaitSec 1)
    $n1 = Spawn 'normal' 'n1'; Wait-Ticket $n1
    $x = Spawn 'exclusive' 'x'; Wait-Ticket $x
    $n2 = Spawn 'normal' 'n2'; Wait-Ticket $n2
    Exit-RunSlots
    foreach ($p in @($n1, $x, $n2)) {
        Wait-For { Test-Path (Path "result-$($p.Id)") } 'fair waiter acquired'
        Set-Content (Path "release-$($p.Id)") 'go'; Finish $p
    }
    Assert (((Get-Content (Path 'order.txt')) -join ',') -eq 'n1,x,n2') 'normal-exclusive-normal acquisition order'
    Assert (@(Get-ChildItem (Path 'build\slots\q') -Filter '*.tkt').Count -eq 0) 'successful waiters leave no tickets'
    Write-Output "PASS: $script:Checks checks across $script:Cases cases (PowerShell $($PSVersionTable.PSVersion))"
} finally {
    foreach ($p in $script:Children) {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; [void]$p.WaitForExit(2000) }
        $p.Dispose()
    }
}
