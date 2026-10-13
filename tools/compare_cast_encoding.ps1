param(
    [string]$MfRuntime=(Join-Path $PSScriptRoot '../build-mf-encoding-benchmark/bin'),
    [string]$FfmpegRuntime=(Join-Path $PSScriptRoot '../build-minimal/bin'),
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '../artifacts/encoding-comparison'),
    [int]$SampleSeconds=12,
    [int]$Rounds=3,
    [string]$MfSourceRevision='',
    [string]$FfmpegSourceRevision=''
)
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
$old=[IO.Path]::GetFullPath($MfRuntime)
$new=[IO.Path]::GetFullPath($FfmpegRuntime)
$media=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../artifacts/ffmpeg-comparison/media'))
$probe='kirakara_show_cast_resource_probe.exe'
Copy-Item -LiteralPath (Join-Path $new $probe) -Destination $old -Force
New-Item -ItemType Directory (Join-Path $output 'runs') -Force | Out-Null
$results=[Collections.Generic.List[object]]::new()
foreach ($dataset in @('h264-1080p','hevc-1080p10')) {
    $extension=if ($dataset -eq 'h264-1080p') { '.mp4' } else { '.mkv' }
    for ($round=1;$round -le $Rounds;$round++) {
        $order=if ($round%2) { @('mf','ffmpeg') } else { @('ffmpeg','mf') }
        foreach ($variant in $order) {
            $runtime=if ($variant -eq 'mf') { $old } else { $new }
            $resultPath=Join-Path $output "runs/$dataset-$variant-$round.json"
            $log=Join-Path $output "runs/$dataset-$variant-$round.log"
            Write-Host "Sampling Cast $dataset / $variant / $round of $Rounds"
            & (Join-Path $runtime $probe) (Join-Path $media "$dataset$extension") (Join-Path $media 'audio.m4a') $resultPath $SampleSeconds *> $log
            if ($LASTEXITCODE -ne 0) { throw "Cast probe failed: $log" }
            $result=Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            if (!$result.playing_at_end -or !$result.gpu_counters_available -or $result.encoder_fps -lt 45 -or $result.matched_latency_frames -lt 100) {
                throw "Invalid Cast measurement: $resultPath"
            }
            $result | Add-Member dataset $dataset
            $result | Add-Member variant $variant
            $result | Add-Member round $round
            $results.Add($result)
            $results | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'results.json') -Encoding utf8
            Write-Host ("CPU {0:N2}% | GPU encode {1:N2}% | {2:N1} fps | encoder latency {3:N2} ms" -f
                $result.cpu_percent_machine,$result.gpu_video_encode_percent,$result.encoder_fps,$result.encoder_latency_mean_ms)
        }
    }
}
$provenance=[ordered]@{
    mfBaselineRevision=$MfSourceRevision
    ffmpegRevision=$FfmpegSourceRevision
    measuredAtUtc=[DateTime]::UtcNow.ToString('o')
    baselineDll=(Get-FileHash (Join-Path $old 'show_host.dll') -Algorithm SHA256).Hash
    ffmpegDll=(Get-FileHash (Join-Path $new 'show_host.dll') -Algorithm SHA256).Hash
    probe=(Get-FileHash (Join-Path $new $probe) -Algorithm SHA256).Hash
    cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfLogicalProcessors)
    gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    rounds=$Rounds
    warmupSeconds=4
    sampleSeconds=$SampleSeconds
    output='1920x1080@60 H264 target 6 Mbps + AAC 160 kbps, GPU input, local HTTP receiver'
}
$provenance | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'provenance.json') -Encoding utf8
Write-Host "Completed Cast encoding comparison: $output"
