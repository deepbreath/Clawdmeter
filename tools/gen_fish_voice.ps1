# Generates 4 fish-personality WAV clips (one per usage-rate group) using
# Windows SAPI at 16 kHz 16-bit mono — matching the existing sound system.
# Output: tools\fish_voice\fish_idle.wav .. fish_heavy.wav

param([string]$OutDir = "$PSScriptRoot\fish_voice")

Add-Type -AssemblyName System.Speech

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$synth  = New-Object System.Speech.Synthesis.SpeechSynthesizer
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono
)

# Use English voice; slightly faster rate for a lively fish personality
$synth.SelectVoice("Microsoft Zira Desktop")
$synth.Rate = 1    # -10..+10; 0 = normal, 1 = slightly brisk

$phrases = @{
    "fish_idle"   = "the water is very still today. i found a nice spot near the rock."
    "fish_norm"   = "there is a gentle current. something is happening up above."
    "fish_active" = "lots of bubbles today. the water feels exciting. i like this."
    "fish_heavy"  = "maximum bubbles. everything is vibrating. so much happening right now."
}

foreach ($name in $phrases.Keys) {
    $path = Join-Path $OutDir "$name.wav"
    $synth.SetOutputToWaveFile($path, $format)
    $synth.Speak($phrases[$name])
    Write-Host "wrote $path"
}

$synth.SetOutputToDefaultAudioDevice()
$synth.Dispose()
Write-Host "done."
