# RoboVidCall signaling server

Run this on any always-on PC on the same LAN as the robot, the iPhone,
and whatever browses the tank control page (your PC is the obvious
choice, since it's already always plugged in for the edl/adb work).

## Setup

```powershell
cd signaling-server
npm install
node server.js
```

You should see:
```
RoboVidCall signaling server listening on ws://0.0.0.0:8080
```

## Find your PC's LAN IP

```powershell
ipconfig
```
Look for the IPv4 address on whichever adapter is on the same WiFi as
the robot (e.g. `192.168.x.x`). You'll need this IP in three places:

1. `RoboVidCall` Android app — `SignalingClient.SERVER_URL` in `MainActivity.kt`
2. `iphone-viewer/viewer.html` — the `SERVER_URL` constant near the top
3. `tank_firmware/webpage.h` — the `SIGNALING_URL` constant in the new script block

All three must point at the same `ws://<your-pc-ip>:8080` and use the
same room name (`"robohon"` by default — change it in all three if
you want something else, they just need to match).

## Firewall

Windows Defender Firewall will likely prompt to allow Node.js on
first run — allow it for Private networks. If viewers can't connect,
that's the first thing to check.

## Keeping it running

This is a plain foreground Node process for now — closing the
terminal kills it. If you want it to survive that, the simplest
option is `pm2` (`npm install -g pm2`, then `pm2 start server.js`),
but that's optional for a LAN dev setup.
