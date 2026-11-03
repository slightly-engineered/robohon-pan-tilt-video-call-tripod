# RoBoHoN Pan/Tilt PTZ Video-Call App Tripod

A pan/tilt tripod head driven by two salvaged Sharp RoBoHoN servos, steered from
a browser joystick, plus RoboVidCall: an Android WebRTC video-call app for
RoBoHoN's Android board with a signaling server and a browser viewer.

| Folder | What it is |
|---|---|
| `firmware/ptz_firmware/` | ESP32-S3 firmware: Wi-Fi joystick web page and pan/tilt over two RoBoHoN servo buses (30-byte `77 80` frames, 115200 baud) |
| `robovidcall/RoboVidCall/` | Android app (runs on RoBoHoN's Android 5 board) that joins a WebRTC call |
| `robovidcall/signaling-server/` | Node.js WebSocket signaling server |
| `robovidcall/iphone-viewer/` | Browser viewer page for the other end of the call |

## Pan/tilt firmware

1. In `firmware/ptz_firmware/`, copy `secrets.example.h` to `secrets.h` and
   enter your Wi-Fi network. `secrets.h` is git-ignored.
2. Check pan/tilt pins, servo IDs and UART assignments in `config.h`, then
   upload with the ESP32 Arduino core.
3. Open the board's IP address in a browser. The page asks once for your
   ESP32-CAM's IP address (or pass `?cam=<ip>` in the URL); click the video
   to change it.

## RoboVidCall

See `robovidcall/README.md` and `robovidcall/signaling-server/README.md`.
Before building the Android app, set `SIGNALING_SERVER_URL` in
`MainActivity.kt` to your PC's LAN address. The signaling server generates its
own local TLS certificate in `signaling-server/certs/`, which is git-ignored.

RoBoHoN is a trademark of Sharp Corporation. This project is not affiliated
with Sharp.

## License

MIT. See [LICENSE](LICENSE).
