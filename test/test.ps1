# MoLink E2E Test Script
param(
    [int]$AccessPort = 1080,
    [int]$AdbTimeout = 10,
    [int]$StopTimeout = 30,
    [int]$BuildTimeout = 120,
    [int]$InstallTimeout = 60,
    [int]$ServiceWait = 10,
    [int]$LogcatTimeout = 15,
    [int]$ProxyTimeout = 30
)

$ErrorActionPreference = "Continue"

$ProjectRoot = "D:\project\MoLink"
$LogsDir = "$ProjectRoot\logs"
$WorkerDir = "$ProjectRoot\molink-worker"
$AccessDir = "$ProjectRoot\molink-access"
$AccessJar = "$AccessDir\target\molink-access-1.0.0.jar"

function info($msg) { Write-Host "  $msg" -ForegroundColor Gray }
function pass($msg) { Write-Host "  $msg" -ForegroundColor Green }
function step-ok($step, $desc, $t) {
    $elapsed = [int]((Get-Date) - $t).TotalSeconds
    Write-Host "[$step] OK $desc (${elapsed}s)" -ForegroundColor Green
}
function step-fail($step, $desc) {
    Write-Host "[$step] FAIL $desc" -ForegroundColor Red
}

function Invoke-CmdWithTimeout {
    param($Name, $ScriptBlock, $TimeoutSec, $ArgumentList = @())
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $job = Start-Job -ScriptBlock $ScriptBlock -ArgumentList $ArgumentList
    $waitResult = Wait-Job -Job $job -Timeout $TimeoutSec
    $elapsed = $sw.ElapsedSeconds
    $sw.Stop()
    $didTimeout = $null -eq $waitResult
    if ($didTimeout) {
        Stop-Job $job -ErrorAction SilentlyContinue
    }
    $result = Receive-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    return [PSCustomObject]@{
        Name = $Name
        TimedOut = $didTimeout
        Elapsed = $elapsed
        Output = @($result)
    }
}

function Do-Summary {
    param($StartTime, $ExitCode, $Report, $LogsDir)
    $elapsed = [int]((Get-Date) - $StartTime).TotalSeconds
    Write-Host ""
    Write-Host "===== Test Summary =====" -ForegroundColor Magenta
    $overall = if ($ExitCode -eq 0) { "ALL PASS" } else { "SOME FAILURES" }
    $color = if ($ExitCode -eq 0) { "Green" } else { "Red" }
    Write-Host "Overall: $overall (${elapsed}s)" -ForegroundColor $color
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("===== MoLink E2E Test Report =====")
    [void]$sb.AppendLine("Start: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    [void]$sb.AppendLine("Duration: ${elapsed}s")
    [void]$sb.AppendLine("Result: $overall")
    [void]$sb.AppendLine("Logs: $LogsDir")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("Steps:")
    foreach ($ln in $Report) { [void]$sb.AppendLine($ln) }
    $sb.ToString() | Out-File "$LogsDir\test-report.log" -Encoding UTF8
    Write-Host ""
    Write-Host "Report: $LogsDir\test-report.log" -ForegroundColor Gray
    exit $ExitCode
}

$startTime = Get-Date
$script:exitCode = 0
$script:device = $null
$report = New-Object System.Collections.ArrayList
function add-report($line) { [void]$report.Add($line) }

Write-Host ""
Write-Host "===== MoLink E2E Test =====" -ForegroundColor Magenta
Write-Host "Start: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray

if (Test-Path $LogsDir) {
    Remove-Item "$LogsDir\*" -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null
info "Logs dir: $LogsDir"

# Step 1: Detect ADB
$step = "1"
$t = Get-Date
Write-Host "[$step] Detect ADB device..." -ForegroundColor Cyan
info "CMD: adb devices (timeout=${AdbTimeout}s)"

$r = Invoke-CmdWithTimeout -Name "adb_devices" -TimeoutSec $AdbTimeout -ScriptBlock {
    adb devices 2>&1
}
info "done: elapsed=$([int]$r.Elapsed.TotalSeconds)s  timedOut=$($r.TimedOut)"

# Parse device serial - find non-empty lines ending with "device"
$script:device = $null
foreach ($line in $r.Output) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -gt 0 -and $trimmed -match 'device$') {
        $parts = $trimmed -split '\s+'
        $script:device = $parts[0]
        break
    }
}
info "Device: $script:device"
add-report "Device: $script:device"

if (-not $script:device) {
    step-fail $step "No Android device detected"
    add-report "[$step] FAIL : No device"
    Do-Summary -StartTime $startTime -ExitCode 1 -Report $report -LogsDir $LogsDir
}
step-ok $step "ADB device detected" $t

# Step 2: Stop old services
$step = "2"
$t = Get-Date
Write-Host "[$step] Stop access service..." -ForegroundColor Cyan

$r = Invoke-CmdWithTimeout -Name "jps" -TimeoutSec $StopTimeout -ScriptBlock {
    jps -l 2>&1
}
$jpsOut = $r.Output -join "`n"
$jpsOut | Out-File "$LogsDir\jps.log" -Encoding UTF8

if ($jpsOut -match 'com\.molink\.access\.AccessApplication') {
    $apid = ($jpsOut -split '\s+')[0].Trim()
    info "Stopping access PID=$apid"
    Stop-Process -Id $apid -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    pass "Access stopped"
} else {
    info "No running access process found"
}

# Also kill any java processes that may hold the jar file
Get-Process java -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*molink*' } | Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "[$step] Stop worker service..." -ForegroundColor Cyan
$r = Invoke-CmdWithTimeout -Name "stop_worker" -TimeoutSec $StopTimeout -ScriptBlock {
    param($d) adb -s $d shell am force-stop com.molink.worker 2>&1
} -ArgumentList @($script:device)
"$($r.Output)" | Out-File "$LogsDir\stop_worker.log" -Encoding UTF8
info "Worker service stopped"
step-ok $step "Old services stopped" $t

# Step 3: Build worker
$step = "3"
$t = Get-Date
Write-Host "[$step] Build worker (Android)..." -ForegroundColor Cyan
info "CMD: gradle (timeout=${BuildTimeout}s)"

$r = Invoke-CmdWithTimeout -Name "gradle" -TimeoutSec $BuildTimeout -ScriptBlock {
    param($dir)
    Push-Location $dir
    $result = & .\gradlew.bat assembleDebug 2>&1
    Pop-Location
    return $result
} -ArgumentList @($WorkerDir)
info "done: elapsed=$([int]$r.Elapsed.TotalSeconds)s  timedOut=$($r.TimedOut)"

$buildOut = $r.Output -join "`n"
$buildOut | Out-File "$LogsDir\worker-build.log" -Encoding UTF8

if ($r.TimedOut) {
    step-fail $step "worker build timeout"
    add-report "[$step] FAIL : worker build timeout"
    Do-Summary -StartTime $startTime -ExitCode 1 -Report $report -LogsDir $LogsDir
}
if ($buildOut -match 'BUILD SUCCESSFUL') {
    info "worker build OK ($([int]$r.Elapsed.TotalSeconds)s)"
    step-ok $step "worker built" $t
} else {
    step-fail $step "worker build failed"
    add-report "[$step] FAIL : worker build failed"
    Do-Summary -StartTime $startTime -ExitCode 1 -Report $report -LogsDir $LogsDir
}

# Step 4: Build access
$step = "4"
$t = Get-Date
Write-Host "[$step] Build access (Windows)..." -ForegroundColor Cyan
info "CMD: mvn (timeout=${BuildTimeout}s)"

$r = Invoke-CmdWithTimeout -Name "maven" -TimeoutSec $BuildTimeout -ScriptBlock {
    param($dir)
    Push-Location $dir
    $result = & mvn package -DskipTests 2>&1
    Pop-Location
    return $result
} -ArgumentList @($AccessDir)
info "done: elapsed=$([int]$r.Elapsed.TotalSeconds)s  timedOut=$($r.TimedOut)"

$mvnOut = $r.Output -join "`n"
$mvnOut | Out-File "$LogsDir\access-build.log" -Encoding UTF8

if ($r.TimedOut) {
    step-fail $step "access build timeout"
    add-report "[$step] FAIL : access build timeout"
    Do-Summary -StartTime $startTime -ExitCode 1 -Report $report -LogsDir $LogsDir
}
if (($mvnOut -match 'BUILD SUCCESS') -and (Test-Path $AccessJar)) {
    info "access build OK ($([int]$r.Elapsed.TotalSeconds)s)"
    step-ok $step "access built" $t
} else {
    step-fail $step "access build failed"
    add-report "[$step] FAIL : access build failed"
    Do-Summary -StartTime $startTime -ExitCode 1 -Report $report -LogsDir $LogsDir
}

# Step 5: Start services
$step = "5"
$t = Get-Date
Write-Host "[$step] Start services..." -ForegroundColor Cyan

$apkPath = "$WorkerDir\app\build\outputs\apk\debug\app-debug.apk"
if (Test-Path $apkPath) {
    info "Installing APK..."
    $r = Invoke-CmdWithTimeout -Name "install_apk" -TimeoutSec $InstallTimeout -ScriptBlock {
        param($d, $a) adb -s $d install -r $a 2>&1
    } -ArgumentList @($script:device, $apkPath)
    $r.Output | Out-File "$LogsDir\worker-install.log" -Encoding UTF8
    $installResult = ($r.Output -join "`n").Trim()
    info "APK install: $installResult"
} else {
    info "APK not found, skip install: $apkPath"
}

info "Starting worker service..."
$r = Invoke-CmdWithTimeout -Name "start_worker" -TimeoutSec 15 -ScriptBlock {
    param($d) adb -s $d shell am startservice -n com.molink.worker/.Socks5ProxyService 2>&1
} -ArgumentList @($script:device)
$workerStartOut = ($r.Output -join "`n").Trim()
info "Worker start: $workerStartOut"

info "Waiting ${ServiceWait}s..."
Start-Sleep -Seconds $ServiceWait

info "Starting access..."
$accessLogFile = "$LogsDir\access.log"
$javaCmd = "java -jar `"$AccessJar`" 2>&1 | Out-File -FilePath `"$accessLogFile`" -Encoding UTF8"
$accessProc = Start-Process powershell.exe -ArgumentList "-NoExit","-Command",$javaCmd -PassThru -WindowStyle Normal
Start-Sleep -Seconds 5

if ($accessProc.HasExited) {
    step-fail $step "access process exited immediately"
    add-report "[$step] FAIL : access process exited"
    $script:exitCode = 1
} else {
    info "access started (PID: $($accessProc.Id))"
    step-ok $step "services started" $t
}

# Step 6: Collect logcat
$step = "6"
$t = Get-Date
Write-Host "[$step] Collect worker logcat..." -ForegroundColor Cyan

$r = Invoke-CmdWithTimeout -Name "pidof" -TimeoutSec 10 -ScriptBlock {
    param($d) adb -s $d shell pidof com.molink.worker 2>&1
} -ArgumentList @($script:device)
$rawPid = (($r.Output -join "`n") -replace '\s+','').Trim()
info "Worker PID: $rawPid"

if (-not [string]::IsNullOrWhiteSpace($rawPid) -and $rawPid -ne "-1" -and $rawPid.Length -gt 0) {
    info "Collecting logcat by PID..."
    $r2 = Invoke-CmdWithTimeout -Name "logcat_pid" -TimeoutSec $LogcatTimeout -ScriptBlock {
        param($d, $wpid) adb -s $d logcat -d --pid=$wpid 2>&1
    } -ArgumentList @($script:device, $rawPid)
    ($r2.Output -join "`n") | Out-File "$LogsDir\worker.log" -Encoding UTF8
    info "logcat collected (PID filter)"
} else {
    info "PID not found, using tag filter..."
    $r2 = Invoke-CmdWithTimeout -Name "logcat_tag" -TimeoutSec $LogcatTimeout -ScriptBlock {
        param($d) adb -s $d logcat -d -s MolinkWorker:S Socks5ProxyService:V *:S 2>&1
    } -ArgumentList @($script:device)
    ($r2.Output -join "`n") | Out-File "$LogsDir\worker.log" -Encoding UTF8
    info "logcat collected (tag filter)"
}
step-ok $step "logcat collected" $t

# Step 7: Test SOCKS proxy
$step = "7"
$t = Get-Date
Write-Host "[$step] Test SOCKS proxy..." -ForegroundColor Cyan

$testUrls = @("http://httpbin.org/ip", "http://myip.ipip.net")
$proxyUrl = "socks5://127.0.0.1:$AccessPort"

# 7a: Direct connection test (no proxy)
Write-Host "  [7a] Direct connection test (no proxy)..." -ForegroundColor Gray
$directOk = $false
foreach ($url in $testUrls) {
    $safeName = $url.Replace('http://','').Replace('/','-').Replace('.','_')
    $directFile = "$LogsDir\direct_${safeName}.log"

    $r = Invoke-CmdWithTimeout -Name "curl_direct" -TimeoutSec 20 -ScriptBlock {
        param($u, $t) curl.exe $u --max-time $t -s -w "`nHTTP_CODE:%{http_code}`nTIME:%{time_total}" 2>&1
    } -ArgumentList @($url, 15)

    $directOut = $r.Output -join "`n"
    $directOut | Out-File $directFile -Encoding UTF8

    if ($directOut -match 'HTTP_CODE:200' -or $directOut -match '"origin"' -or $directOut -match '\d+\.\d+\.\d+\.\d+') {
        pass "Direct OK: $url"
        $directOk = $true
        break
    } else {
        info "Direct FAIL: $url"
    }
}

if ($directOk) {
    info "Direct connection: reachable"
} else {
    info "Direct connection: unreachable (network issue, not proxy)"
    add-report "[$step] WARN : direct connection unreachable, cannot verify proxy"
}

# 7b: Proxy connection test
Write-Host "  [7b] Proxy connection test..." -ForegroundColor Gray
$proxyOk = $false

foreach ($url in $testUrls) {
    info "Testing: curl.exe -x $proxyUrl $url"
    $safeName = $url.Replace('http://','').Replace('/','-').Replace('.','_')
    $curlFile = "$LogsDir\proxy_${safeName}.log"

    $r = Invoke-CmdWithTimeout -Name "curl_test" -TimeoutSec ($ProxyTimeout + 10) -ScriptBlock {
        param($u, $p, $t) curl.exe -x $p $u --max-time $t -s -w "`nHTTP_CODE:%{http_code}`nTIME:%{time_total}" 2>&1
    } -ArgumentList @($url, $proxyUrl, $ProxyTimeout)

    $curlOut = $r.Output -join "`n"
    $curlOut | Out-File $curlFile -Encoding UTF8

    if ($curlOut -match 'HTTP_CODE:200' -or $curlOut -match '"origin"' -or $curlOut -match '\d+\.\d+\.\d+\.\d+') {
        pass "Proxy OK: $url"
        $proxyOk = $true
        break
    } else {
        info "Proxy FAIL: $url"
    }
}

if ($proxyOk) {
    step-ok $step "SOCKS proxy working" $t
} else {
    step-fail $step "SOCKS proxy test failed"
    add-report "[$step] FAIL : proxy unreachable"
    $script:exitCode = 1
}

# Normal end: run summary
Do-Summary -StartTime $startTime -ExitCode $script:exitCode -Report $report -LogsDir $LogsDir
