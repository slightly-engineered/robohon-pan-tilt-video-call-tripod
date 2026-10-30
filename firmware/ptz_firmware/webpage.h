#pragma once

// ============================================================
// Joystick web page with integrated ESP32‑CAM video feed.
// Camera stream URL: http://<cam-ip>:81/stream (camera IP is entered on the page)
// ============================================================

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Plen View</title>
<style>
    * {
        box-sizing: border-box;
    }
    body {
        background: #000;
        color: #e8e8e8;
        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
        text-align: center;
        margin: 0;
        padding: 0;
    }
    h2 {
        font-weight: 600;
        letter-spacing: 1px;
        color: #f2f2f2;
        margin-top: 36px;
        margin-bottom: 8px;
    }
    #status {
        margin-top: 8px;
        font-size: 13px;
        font-weight: 600;
        letter-spacing: 1.5px;
        text-transform: uppercase;
        color: #888;
    }

    /* Camera Stream */
    #camContainer {
        margin-top: 20px;
        margin-bottom: 10px;
    }
    #camstream {
        width: 90%;
        max-width: 420px;
        border: 2px solid #333;
        border-radius: 10px;
        box-shadow: 0 0 12px rgba(0,0,0,0.6);
    }

    /* Joystick */
    #joyArea {
        width: 300px;
        height: 300px;
        background: radial-gradient(circle at 38% 32%, #5a5a5a 0%, #3a3a3a 55%, #2a2a2a 100%);
        border: 1px solid #1a1a1a;
        border-radius: 50%;
        margin: 40px auto;
        touch-action: none;
        position: relative;
        box-shadow:
            inset 0 6px 14px rgba(0, 0, 0, 0.55),
            inset 0 -2px 6px rgba(255, 255, 255, 0.04),
            0 4px 12px rgba(0, 0, 0, 0.5);
    }
    #stick {
        width: 92px;
        height: 92px;
        border-radius: 50%;
        position: absolute;
        left: 104px;
        top: 104px;
        pointer-events: none;

        background-image:
            radial-gradient(circle, #0a0a0a 2.15px, transparent 2.35px),
            radial-gradient(circle, #0a0a0a 2.15px, transparent 2.35px),
            radial-gradient(circle at 34% 28%, #ff7a63 0%, #e5291b 45%, #a3140b 80%, #7a0f08 100%);
        background-size: 11px 11px, 11px 11px, 100% 100%;
        background-position: 0 0, 5.5px 5.5px, 0 0;

        border: 2px solid #5c0c06;
        box-shadow:
            0 5px 10px rgba(0, 0, 0, 0.6),
            inset 0 2px 3px rgba(255, 255, 255, 0.25),
            inset 0 -6px 10px rgba(0, 0, 0, 0.45);
        opacity: 0.97;
    }
</style>
</head>
<body>

<h2>Plen View</h2>
<div id="status">Connecting...</div>

<!-- Camera Stream -->
<div id="camContainer">
    <img id="camstream" alt="ESP32-CAM stream (click to set camera IP)" title="Click to change camera IP">
</div>

<!-- Joystick -->
<div id="joyArea">
    <div id="stick"></div>
</div>

<script>
// ESP32-CAM address: taken from ?cam=<ip>, else remembered in this browser, else asked once.
(function () {
    const img = document.getElementById("camstream");
    function setCam(ask) {
        let host = new URLSearchParams(location.search).get("cam") || localStorage.getItem("camHost");
        if (ask || !host) host = prompt("ESP32-CAM IP address (shown in its Serial Monitor at boot):", host || "");
        if (!host) return;
        host = host.trim();
        localStorage.setItem("camHost", host);
        img.src = "http://" + host + ":81/stream";
    }
    img.addEventListener("click", function () { setCam(true); });
    setCam(false);
})();
</script><script>
    // WebSocket URL derived from the host serving this page
    const wsProto = (location.protocol === "https:") ? "wss://" : "ws://";
    const WS_URL = wsProto + location.host + "/ws";

    let ws;
    let connected = false;

    function connectWS() {
        ws = new WebSocket(WS_URL);

        ws.onopen = () => {
            connected = true;
            document.getElementById("status").innerText = "Connected";
        };

        ws.onclose = () => {
            connected = false;
            document.getElementById("status").innerText = "Disconnected";
            setTimeout(connectWS, 2000);
        };

        ws.onerror = () => {
            connected = false;
            document.getElementById("status").innerText = "Error";
        };
    }

    connectWS();

    // Joystick logic
    const joyArea = document.getElementById("joyArea");
    const stick = document.getElementById("stick");

    let joyX = 0;
    let joyY = 0;

    function sendJoy() {
        if (connected) {
            ws.send(`JOY ${joyX.toFixed(2)} ${joyY.toFixed(2)}`);
        }
    }

    setInterval(sendJoy, 50); // 20 FPS control updates

    function updateStick(x, y) {
        stick.style.left = (150 + x * 100 - 46) + "px";
        stick.style.top  = (150 + y * 100 - 46) + "px";
    }

    function handleMove(evt) {
        const rect = joyArea.getBoundingClientRect();
        const cx = rect.left + rect.width / 2;
        const cy = rect.top + rect.height / 2;

        let x, y;

        if (evt.touches) {
            x = evt.touches[0].clientX;
            y = evt.touches[0].clientY;
        } else {
            x = evt.clientX;
            y = evt.clientY;
        }

        let dx = (x - cx) / 100;
        let dy = (y - cy) / 100;

        dx = Math.max(-1, Math.min(1, dx));
        dy = Math.max(-1, Math.min(1, dy));

        joyX = -dx;
        joyY = dy;

        updateStick(dx, dy);
    }

    function resetStick() {
        joyX = 0;
        joyY = 0;
        updateStick(0, 0);
    }

    joyArea.addEventListener("touchstart", handleMove);
    joyArea.addEventListener("touchmove", handleMove);
    joyArea.addEventListener("touchend", resetStick);

    joyArea.addEventListener("mousedown", handleMove);
    joyArea.addEventListener("mousemove", (e) => {
        if (e.buttons === 1) handleMove(e);
    });
    joyArea.addEventListener("mouseup", resetStick);
</script>

</body>
</html>
)HTMLPAGE";
