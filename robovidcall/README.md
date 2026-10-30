# RoboVidCall

The iPhone's camera (and mic) shown full-screen on the RoBoHoN Android
board. The board has **no camera, so it is receive-only**: it never
captures or sends video.

## The three pieces

1. **`signaling-server/`** - runs on your PC. Relays connection setup
   messages only; no video/audio passes through it. Two ports, same rooms:
   - `ws://PC-IP:8080` - plain WebSocket, used by the robot.
   - `https://PC-IP:8443/` - HTTPS + wss, serves the iPhone page.
2. **`RoboVidCall/`** - the Android Studio project for the robot. Always
   waits for a call and shows the caller's camera full-screen.
3. **`iphone-viewer/viewer.html`** - served by the server above. Nothing
   to edit; it connects back to whatever host it was loaded from.

## Why HTTPS for the iPhone

Safari only provides the camera on secure pages. Over plain
`http://192.168.x.x` `navigator.mediaDevices` does not exist at all, so
the page can never capture the camera. The server therefore makes a
self-signed certificate on first run (`signaling-server/certs/`, covers
this PC's LAN IPs, reused on later runs).

## Setup

### 1. Server (PC)
```powershell
cd signaling-server
npm install
node server.js
```
It prints the URLs. Find your PC's LAN IP with `ipconfig`.
Windows Firewall must allow inbound TCP **8080** and **8443** for Node.

### 2. Android app
Edit the two constants at the top of `MainActivity.kt`:
```kotlin
private const val SIGNALING_SERVER_URL = "ws://<PC-IP>:8080"
private const val ROOM_NAME = "robohon"
```
Build, then:
```powershell
./adb install -r app-debug.apk
```
Launch it; it shows "Waiting for a call". It reconnects on its own if
the server restarts or Wi-Fi drops.

### 3. iPhone
Safari -> `https://<PC-IP>:8443/`. Safari warns about the certificate
(self-signed): Show Details -> visit this website. Tap **Join Call**,
allow camera/mic. The robot calls automatically within a second or two.
Buttons: **Flip camera**, **Hang up**.

If the iPhone refuses the certificate outright or the WebSocket won't
connect after accepting it, install the certificate as a trusted
profile instead (AirDrop `signaling-server/certs/cert.pem`, install it in
Settings, then Settings -> General -> About -> Certificate Trust
Settings -> enable it).

## Debugging on the robot

`adb logcat -s WebRtcClient SignalingClient` shows ICE state, the remote
video resolution, and any WebRTC errors.

- **Green / garbled / frozen video, or a crash in MediaCodec:** set
  `USE_HARDWARE_DECODER = false` in `WebRtcClient.kt` (software decode).
- **Choppy but working:** lower `VIDEO_CONSTRAINTS` / `MAX_VIDEO_BITRATE`
  at the top of the script in `viewer.html`.
- **No sound / too quiet:** the app forces the loudspeaker; adjust the
  volume keys while a call is up. Set `SEND_AUDIO = false` in
  `viewer.html` to send video only.

## Notes
- Only one call at a time: a new viewer replaces the previous one.
- No authentication on the signaling server - home LAN only.
