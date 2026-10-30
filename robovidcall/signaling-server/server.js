// RoboVidCall signaling server
// -----------------------------------------------------------------
// Pure relay: it only forwards JSON messages between clients in the
// same "room". No video/audio ever touches this process - that flows
// peer-to-peer once WebRTC connects.
//
// It listens on TWO ports that share the same rooms:
//
//   ws://  0.0.0.0:8080   plain WebSocket  -> the robot (Android app)
//   https  0.0.0.0:8443   HTTPS + wss://   -> the iPhone
//                         also serves ../iphone-viewer/viewer.html at /
//
// Why HTTPS for the iPhone: Safari only exposes the camera
// (navigator.mediaDevices) on secure pages. Plain http://192.168.x.x
// is NOT secure, so getUserMedia does not exist there at all. The
// server makes a self-signed certificate on first run (certs/) that
// includes this PC's LAN IPs; you accept the warning once on the
// iPhone. The robot can't validate a self-signed cert, so it keeps
// using the plain port.
//
// Run:   npm install
//        node server.js
//
// Wire protocol (all messages are JSON text frames):
//
//   Client -> Server
//     { type: "join",      room, role: "broadcaster" | "viewer" }
//     { type: "offer",     room, to, sdp }
//     { type: "answer",    room, to, sdp }
//     { type: "candidate", room, to, candidate }
//     { type: "leave",     room }
//
//   Server -> Client
//     { type: "joined",              id }
//     { type: "new-viewer",          id }      // to broadcaster (also sent
//                                              // for viewers already waiting
//                                              // when the broadcaster joins)
//     { type: "broadcaster-available" }        // to a viewer
//     { type: "no-broadcaster" }               // to a viewer
//     { type: "offer" | "answer" | "candidate", from, sdp/candidate }
//     { type: "peer-left",           id }
// -----------------------------------------------------------------

const fs = require("fs");
const os = require("os");
const path = require("path");
const http = require("http");
const https = require("https");
const WebSocket = require("ws");

const WS_PORT = parseInt(process.env.PORT || "8080", 10);
const HTTPS_PORT = parseInt(process.env.HTTPS_PORT || "8443", 10);
const VIEWER_FILE = path.join(__dirname, "..", "iphone-viewer", "viewer.html");
const CERT_DIR = path.join(__dirname, "certs");

let nextId = 1;

// room name -> { broadcaster: {id, ws} | null, viewers: Map<id, ws> }
const rooms = new Map();

function getRoom(name) {
    if (!rooms.has(name)) {
        rooms.set(name, { broadcaster: null, viewers: new Map() });
    }
    return rooms.get(name);
}

function send(ws, obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(obj));
    }
}

function handleConnection(ws, label) {
    const id = nextId++;
    let joinedRoom = null;
    let joinedRole = null;

    // Without this, a socket error (e.g. phone drops off Wi-Fi mid-frame)
    // is an unhandled 'error' event and kills the whole server.
    ws.on("error", (err) => console.log(`[${label}] #${id} socket error: ${err.message}`));

    function leaveRoom() {
        if (!joinedRoom) return;
        const room = rooms.get(joinedRoom);
        const roomName = joinedRoom;
        const role = joinedRole;
        joinedRoom = null;
        joinedRole = null;
        if (!room) return;

        if (role === "broadcaster") {
            // Only act if this socket is still THE broadcaster. If a newer
            // robot connection already replaced it, this is a stale socket.
            if (room.broadcaster && room.broadcaster.id === id) {
                room.broadcaster = null;
                for (const [, viewerWs] of room.viewers) {
                    send(viewerWs, { type: "peer-left", id });
                }
            }
        } else if (room.viewers.get(id) === ws) {
            room.viewers.delete(id);
            if (room.broadcaster) {
                send(room.broadcaster.ws, { type: "peer-left", id });
            }
        }
        console.log(`[room ${roomName}] ${role} #${id} left`);
    }

    ws.on("message", (raw) => {
        let msg;
        try {
            msg = JSON.parse(raw);
        } catch (e) {
            return; // ignore garbage
        }

        if (msg.type === "join") {
            leaveRoom(); // re-join on the same socket = leave the old room first
            joinedRoom = msg.room;
            joinedRole = msg.role === "broadcaster" ? "broadcaster" : "viewer";
            const room = getRoom(joinedRoom);

            if (joinedRole === "broadcaster") {
                // A robot that reconnects replaces the old (dead) one.
                room.broadcaster = { id, ws };
                send(ws, { type: "joined", id });
                for (const [viewerId, viewerWs] of room.viewers) {
                    send(viewerWs, { type: "broadcaster-available" });
                    // Viewers that were already waiting never get a
                    // "new-viewer" otherwise - the robot would never call them.
                    send(ws, { type: "new-viewer", id: viewerId });
                }
            } else {
                room.viewers.set(id, ws);
                send(ws, { type: "joined", id });
                if (room.broadcaster) {
                    send(ws, { type: "broadcaster-available" });
                    send(room.broadcaster.ws, { type: "new-viewer", id });
                } else {
                    send(ws, { type: "no-broadcaster" });
                }
            }
            console.log(`[room ${joinedRoom}] ${joinedRole} joined as #${id} (${label})`);
            return;
        }

        if (msg.type === "leave") {
            leaveRoom();
            return;
        }

        // offer / answer / candidate: relay to msg.to within the same room
        if (["offer", "answer", "candidate"].includes(msg.type)) {
            if (!joinedRoom) return;
            const room = getRoom(joinedRoom);
            let targetWs = null;

            if (room.broadcaster && room.broadcaster.id === msg.to) {
                targetWs = room.broadcaster.ws;
            } else if (room.viewers.has(msg.to)) {
                targetWs = room.viewers.get(msg.to);
            }

            if (targetWs) {
                send(targetWs, { ...msg, from: id });
            }
        }
    });

    ws.on("close", leaveRoom);
}

// ---------- plain ws:// (robot) ----------
const wss = new WebSocket.Server({ port: WS_PORT });
wss.on("connection", (ws) => handleConnection(ws, "ws"));
wss.on("error", (err) => console.error("ws server error:", err.message));

// ---------- https + wss:// (iPhone) ----------
function lanIps() {
    const ips = [];
    for (const list of Object.values(os.networkInterfaces())) {
        for (const ni of list || []) {
            if (ni.family === "IPv4" && !ni.internal) ips.push(ni.address);
        }
    }
    return ips;
}

async function loadOrCreateCert() {
    const keyPath = path.join(CERT_DIR, "key.pem");
    const certPath = path.join(CERT_DIR, "cert.pem");
    const ipsPath = path.join(CERT_DIR, "ips.txt");
    const ips = lanIps().sort();

    // Reuse the cert (so the iPhone only has to accept it once) as long as
    // it still covers every IP this PC currently has.
    if (fs.existsSync(keyPath) && fs.existsSync(certPath) && fs.existsSync(ipsPath)) {
        const covered = fs.readFileSync(ipsPath, "utf8").split("\n").filter(Boolean);
        if (ips.every((ip) => covered.includes(ip))) {
            return { key: fs.readFileSync(keyPath), cert: fs.readFileSync(certPath) };
        }
    }

    const selfsigned = require("selfsigned");
    const altNames = [
        { type: 2, value: "localhost" },
        { type: 7, ip: "127.0.0.1" },
        ...ips.map((ip) => ({ type: 7, ip })),
    ];
    const pems = await selfsigned.generate([{ name: "commonName", value: "RoboVidCall" }], {
        days: 825, // iOS rejects certs valid for longer than ~825 days
        keySize: 2048,
        algorithm: "sha256",
        extensions: [
            { name: "basicConstraints", cA: false },
            { name: "keyUsage", digitalSignature: true, keyEncipherment: true },
            { name: "extKeyUsage", serverAuth: true },
            { name: "subjectAltName", altNames },
        ],
    });
    fs.mkdirSync(CERT_DIR, { recursive: true });
    fs.writeFileSync(keyPath, pems.private);
    fs.writeFileSync(certPath, pems.cert);
    fs.writeFileSync(ipsPath, ips.join("\n") + "\n");
    console.log("Generated new self-signed certificate for: " + ips.join(", "));
    return { key: pems.private, cert: pems.cert };
}

async function startHttps() {
    const creds = await loadOrCreateCert();

    const server = https.createServer(creds, (req, res) => {
        if (req.url === "/" || req.url.startsWith("/viewer.html")) {
            fs.readFile(VIEWER_FILE, (err, data) => {
                if (err) {
                    res.writeHead(500, { "Content-Type": "text/plain" });
                    res.end("Could not read " + VIEWER_FILE);
                    return;
                }
                res.writeHead(200, {
                    "Content-Type": "text/html; charset=utf-8",
                    "Cache-Control": "no-store",
                });
                res.end(data);
            });
        } else {
            res.writeHead(404, { "Content-Type": "text/plain" });
            res.end("Not found");
        }
    });

    const wssSecure = new WebSocket.Server({ server });
    wssSecure.on("connection", (ws) => handleConnection(ws, "wss"));
    wssSecure.on("error", (err) => console.error("wss error:", err.message));
    server.on("error", (err) => console.error("https server error:", err.message));

    server.listen(HTTPS_PORT, "0.0.0.0", () => {
        console.log(`iPhone page:   https://<PC-IP>:${HTTPS_PORT}/`);
        for (const ip of lanIps()) console.log(`               https://${ip}:${HTTPS_PORT}/`);
    });
}

console.log(`Robot (Android) connects to: ws://<PC-IP>:${WS_PORT}`);
startHttps().catch((err) => {
    console.error("HTTPS listener failed to start (robot ws:// still works):", err);
});
