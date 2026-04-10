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
    param($Name, $ScriptBlock, $TimeoutSec, $ArgumentList = @(), [switch]$Silent)
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

# 使用 jps + Select-String 找到 molink-access 进程并停止
$accessPid = $null
$r = Invoke-CmdWithTimeout -Name "jps_l" -TimeoutSec 5 -ScriptBlock {
    jps -l 2>&1
}
$jpsOut = $r.Output -join "`n"
$jpsOut | Write-Host

foreach ($line in $r.Output) {
    $trimmed = $line.Trim()
    if ($trimmed -match 'com\.molink\.access\.AccessApplication') {
        $parts = $trimmed -split '\s+'
        $accessPid = $parts[0]
        break
    }
}

if ($accessPid) {
    info "Stopping access PID=$accessPid"
    Stop-Process -Id $accessPid -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    pass "Access stopped"
} else {
    info "No running access process found"
}

Write-Host "[$step] Stop worker service..." -ForegroundColor Cyan
$r = Invoke-CmdWithTimeout -Name "stop_worker" -TimeoutSec $StopTimeout -ScriptBlock {
    param($d) adb -s $d shell am force-stop com.molink.worker 2>&1
} -ArgumentList @($script:device)
$swOut = ($r.Output -join "`n").Trim()
info "Worker: $swOut"
step-ok $step "Old services stopped" $t

# Step 3: Build worker
$step = "3"
$t = Get-Date
Write-Host "[$step] Build worker (Android)..." -ForegroundColor Cyan
info "CMD: gradle (timeout=${BuildTimeout}s)"

$r = Invoke-CmdWithTimeout -Name "gradle" -TimeoutSec $BuildTimeout -ScriptBlock {
    param($dir)
    Push-Location $dir
    $result = & .\gradlew.bat clean assembleDebug --no-daemon 2>&1
    Pop-Location
    return $result
} -ArgumentList @($WorkerDir)

$buildOut = $r.Output -join "`n"
$buildOut | Write-Host          # 打印到控制台
info "done: elapsed=$([int]$r.Elapsed.TotalSeconds)s  timedOut=$($r.TimedOut)"

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
    $result = & mvn clean package -DskipTests 2>&1
    Pop-Location
    return $result
} -ArgumentList @($AccessDir)

$mvnOut = $r.Output -join "`n"
$mvnOut | Write-Host          # 打印到控制台
info "done: elapsed=$([int]$r.Elapsed.TotalSeconds)s  timedOut=$($r.TimedOut)"

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
    $installResult = ($r.Output -join "`n").Trim()
    info "APK install: $installResult"   # 安装结果打印到控制台

    # 启动 MainActivity 使组件注册，再通过 ADB 启动服务
    info "Launching MainActivity to register components..."
    Invoke-CmdWithTimeout -Name "launch_activity" -TimeoutSec 10 -ScriptBlock {
        param($d) adb -s $d shell am start -n com.molink.worker/.MainActivity 2>&1
    } -ArgumentList @($script:device) | Out-Null
    Start-Sleep -Seconds 2
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

# 启动 access - Start-Process java 直接启动，新窗口显示
info "Starting access..."
$accessProc = Start-Process java -ArgumentList "-Dfile.encoding=GBK", "-jar", $AccessJar -PassThru -WindowStyle Normal
Start-Sleep -Seconds 5
Start-Sleep -Seconds 5

if ($accessProc.HasExited) {
    step-fail $step "access process exited immediately"
    add-report "[$step] FAIL : access process exited"
    $script:exitCode = 1
} else {
    info "access started (PID: $($accessProc.Id))"

    # 等待 access Spring Boot 真正就绪（端口可响应）
    info "Waiting for access API ready..."
    $apiReady = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 1
        $tcp = Test-NetConnection -ComputerName localhost -Port 8080 -WarningAction SilentlyContinue -ErrorAction SilentlyContinue
        if ($tcp.TcpTestSucceeded) {
            $apiReady = $true
            break
        }
    }
    if ($apiReady) {
        info "access API ready"
    } else {
        info "access API not ready in time, continuing anyway"
    }

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
    $workerLog = $r2.Output -join "`n"
    $workerLog | Out-File "$LogsDir\worker.log" -Encoding UTF8
    $workerLog | Write-Host    # 打印到控制台
    info "logcat collected (PID filter)"
} else {
    info "PID not found, using tag filter..."
    $r2 = Invoke-CmdWithTimeout -Name "logcat_tag" -TimeoutSec $LogcatTimeout -ScriptBlock {
        param($d) adb -s $d logcat -d -s MolinkWorker:S Socks5ProxyService:V *:S 2>&1
    } -ArgumentList @($script:device)
    $workerLog = $r2.Output -join "`n"
    $workerLog | Out-File "$LogsDir\worker.log" -Encoding UTF8
    $workerLog | Write-Host    # 打印到控制台
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
    $r = Invoke-CmdWithTimeout -Name "curl_direct" -TimeoutSec 20 -ScriptBlock {
        param($u, $t) curl.exe $u --max-time $t -s -w "`nHTTP_CODE:%{http_code}`nTIME:%{time_total}" 2>&1
    } -ArgumentList @($url, 15)

    $directOut = $r.Output -join "`n"
    $directOut | Write-Host   # 打印到控制台

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
    $r = Invoke-CmdWithTimeout -Name "curl_test" -TimeoutSec ($ProxyTimeout + 10) -ScriptBlock {
        param($u, $p, $t) curl.exe -x $p $u --max-time $t -s -w "`nHTTP_CODE:%{http_code}`nTIME:%{time_total}" 2>&1
    } -ArgumentList @($url, $proxyUrl, $ProxyTimeout)

    $curlOut = $r.Output -join "`n"
    $curlOut | Write-Host   # 打印到控制台

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
