# Recording backup

The recorder stores WAV files on the SD card while offline and uploads pending
files when Wi-Fi is connected.

## SD card config

Network config is shared by recorder upload and future MQTT features:

```json
// /config/wifi.json
{
  "ssid": "your-wifi",
  "password": "your-password",
  "device_id": "clawdmeter-001"
}
```

Recorder upload config is separate:

```json
// /config/recorder.json
{
  "upload_url": "http://raspberrypi.local:8080/upload",
  "auth_token": "optional-token"
}
```

For compatibility, `ssid`, `password`, and `device_id` may still live in
`/config/recorder.json`, but new features should read Wi-Fi settings through
`wifi_manager`.

## Storage layout

- Pending files: `/recordings/pending/*.wav` and matching `*.json`
- Uploaded files: `/recordings/sent/*.wav` and matching `*.json`

## Controls

- `Key3 / IO18` double-click: start or stop recording
- `Key1 / PWR`: unchanged; short-press cycles screens/animations, long-press is power
- `Key2 / BOOT`: unused

## Serial commands

- `rec`: start or stop recording
- `upload`: request upload scan
- `wifi`: print Wi-Fi and recorder upload status without printing the password

