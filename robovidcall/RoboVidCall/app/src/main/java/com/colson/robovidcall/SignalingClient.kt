package com.colson.robovidcall

import android.os.Handler
import android.os.Looper
import android.util.Log
import okhttp3.*
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * Thin wrapper over an OkHttp WebSocket that speaks the tiny JSON
 * protocol implemented by signaling-server/server.js.
 *
 * Reconnects automatically (2s, 4s, then every 5s) if the server goes
 * away or Wi-Fi drops, and re-joins the room each time - the robot is
 * meant to sit there "always waiting", so it must survive a server
 * restart without someone touching it.
 *
 * All Listener callbacks fire on OkHttp's internal thread - hop to the
 * main thread yourself (see MainActivity) before touching views.
 */
class SignalingClient(
    private val serverUrl: String,
    private val room: String,
    private val listener: Listener
) {
    interface Listener {
        fun onJoined(myId: Int)
        fun onBroadcasterAvailable() {}
        fun onNoBroadcaster() {}
        fun onNewViewer(viewerId: Int)
        fun onOffer(fromId: Int, sdp: String)
        fun onAnswer(fromId: Int, sdp: String)
        fun onCandidate(fromId: Int, sdpMid: String?, sdpMLineIndex: Int, candidate: String)
        fun onPeerLeft(peerId: Int)
        fun onSocketError(message: String)
    }

    private val client = OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)
        .build()

    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile private var socket: WebSocket? = null
    @Volatile private var closedByUs = false
    private var role: String = "broadcaster"
    private var attempt = 0

    fun connect(role: String) {
        this.role = role
        closedByUs = false
        openSocket()
    }

    private fun openSocket() {
        if (closedByUs) return
        val request = try {
            Request.Builder().url(serverUrl).build()
        } catch (e: IllegalArgumentException) {
            listener.onSocketError("bad server URL: $serverUrl")
            return
        }

        socket = client.newWebSocket(request, object : WebSocketListener() {

            override fun onOpen(webSocket: WebSocket, response: Response) {
                if (webSocket !== socket) return
                attempt = 0
                val join = JSONObject().apply {
                    put("type", "join")
                    put("room", room)
                    put("role", role)
                }
                webSocket.send(join.toString())
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                if (webSocket !== socket) return
                handleMessage(text)
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                if (webSocket !== socket) return
                Log.e("SignalingClient", "socket failure", t)
                listener.onSocketError("${t.message ?: "unknown socket error"} (retrying)")
                scheduleReconnect()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                if (webSocket !== socket) return
                Log.i("SignalingClient", "socket closed: $code $reason")
                if (!closedByUs) {
                    listener.onSocketError("server closed the connection (retrying)")
                    scheduleReconnect()
                }
            }
        })
    }

    private fun scheduleReconnect() {
        if (closedByUs) return
        attempt++
        val delayMs = if (attempt == 1) 2000L else if (attempt == 2) 4000L else 5000L
        mainHandler.postDelayed({ openSocket() }, delayMs)
    }

    private fun handleMessage(text: String) {
        val json = try {
            JSONObject(text)
        } catch (e: Exception) {
            Log.w("SignalingClient", "bad json: $text")
            return
        }

        when (json.optString("type")) {
            "joined" -> listener.onJoined(json.optInt("id"))
            "broadcaster-available" -> listener.onBroadcasterAvailable()
            "no-broadcaster" -> listener.onNoBroadcaster()
            "new-viewer" -> listener.onNewViewer(json.optInt("id"))
            "offer" -> listener.onOffer(json.optInt("from"), json.optString("sdp"))
            "answer" -> listener.onAnswer(json.optInt("from"), json.optString("sdp"))
            "candidate" -> {
                val cand = json.optJSONObject("candidate") ?: return
                // optString(name, null) is a Kotlin null-safety trap - use isNull instead.
                val mid = if (cand.isNull("sdpMid")) null else cand.optString("sdpMid")
                listener.onCandidate(
                    json.optInt("from"),
                    mid,
                    cand.optInt("sdpMLineIndex", 0),
                    cand.optString("candidate")
                )
            }
            "peer-left" -> listener.onPeerLeft(json.optInt("id"))
        }
    }

    fun sendOffer(toId: Int, sdp: String) {
        send(JSONObject().apply {
            put("type", "offer")
            put("room", room)
            put("to", toId)
            put("sdp", sdp)
        })
    }

    fun sendAnswer(toId: Int, sdp: String) {
        send(JSONObject().apply {
            put("type", "answer")
            put("room", room)
            put("to", toId)
            put("sdp", sdp)
        })
    }

    fun sendCandidate(toId: Int, sdpMid: String?, sdpMLineIndex: Int, candidate: String) {
        send(JSONObject().apply {
            put("type", "candidate")
            put("room", room)
            put("to", toId)
            put("candidate", JSONObject().apply {
                put("sdpMid", sdpMid ?: JSONObject.NULL)
                put("sdpMLineIndex", sdpMLineIndex)
                put("candidate", candidate)
            })
        })
    }

    private fun send(obj: JSONObject) {
        socket?.send(obj.toString())
    }

    fun close() {
        closedByUs = true
        mainHandler.removeCallbacksAndMessages(null)
        socket?.close(1000, "bye")
        socket = null
        client.dispatcher.executorService.shutdown()
    }
}
