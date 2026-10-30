package com.colson.robovidcall

import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.os.Bundle
import android.view.View
import android.view.WindowManager
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.SessionDescription
import org.webrtc.SurfaceViewRenderer

/**
 * Runs on the robot (no camera - receive only). Always joins as the
 * signaling "broadcaster": when someone opens the iPhone page and taps
 * Join, this app places the call, and the iPhone's camera is shown
 * full-screen here (with its audio through the speaker).
 *
 * EDIT THESE TWO before building. This is the only place the PC's IP
 * lives now - the iPhone page picks up the server address by itself.
 */
private const val SIGNALING_SERVER_URL = "ws://YOUR_PC_LAN_IP:8080" // your PC's LAN IP, plain ws:// port 8080
private const val ROOM_NAME = "robohon"

class MainActivity : AppCompatActivity(), SignalingClient.Listener, WebRtcClient.Callbacks {

    companion object {
        /** True only while this activity is in front. KeepFrontService watches it. */
        @Volatile
        var isResumed = false
    }

    private lateinit var eglBase: EglBase
    private lateinit var webRtcClient: WebRtcClient
    private lateinit var signalingClient: SignalingClient
    private lateinit var statusText: TextView
    private lateinit var remoteRenderer: SurfaceViewRenderer

    private var audioManager: AudioManager? = null
    private var savedAudioMode = AudioManager.MODE_NORMAL
    private var savedSpeakerphone = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // The robot sits there waiting: never let the screen sleep, and use the whole screen.
        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                WindowManager.LayoutParams.FLAG_FULLSCREEN
        )
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        remoteRenderer = findViewById(R.id.remoteView)

        hideSystemUi()
        routeAudioToSpeaker()
        startCallStack()
        startService(Intent(this, KeepFrontService::class.java))
    }

    override fun onResume() {
        super.onResume()
        isResumed = true
    }

    override fun onPause() {
        isResumed = false
        super.onPause()
    }

    /** Kiosk: hide the status/nav bars, and bring them back hidden if anything reveals them. */
    private fun hideSystemUi() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            )
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemUi()
    }

    /** This is the home screen now: a Back press must not leave the app. */
    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    override fun onBackPressed() {
        // intentionally ignored
    }

    /**
     * WebRTC plays call audio on the "voice call" stream, which Android
     * sends to the quiet earpiece by default. Force the loudspeaker.
     * (Volume keys adjust the call volume while a call is up.)
     */
    @Suppress("DEPRECATION")
    private fun routeAudioToSpeaker() {
        val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager = am
        savedAudioMode = am.mode
        savedSpeakerphone = am.isSpeakerphoneOn
        am.mode = AudioManager.MODE_IN_COMMUNICATION
        am.isSpeakerphoneOn = true
    }

    private fun startCallStack() {
        eglBase = EglBase.create()

        webRtcClient = WebRtcClient(applicationContext, eglBase, this)
        webRtcClient.init()
        webRtcClient.attachRemoteRenderer(remoteRenderer)

        signalingClient = SignalingClient(SIGNALING_SERVER_URL, ROOM_NAME, this)
        signalingClient.connect(role = "broadcaster")

        setStatus("Connecting to signaling server...")
    }

    private fun setStatus(text: String) {
        runOnUiThread { statusText.text = text }
    }

    // ---- SignalingClient.Listener (OkHttp thread) ----

    override fun onJoined(myId: Int) {
        setStatus("Waiting for a call ($ROOM_NAME, id=$myId)")
    }

    override fun onNewViewer(viewerId: Int) {
        setStatus("Incoming call (viewer #$viewerId)...")
        webRtcClient.createOfferForViewer(viewerId)
    }

    override fun onAnswer(fromId: Int, sdp: String) {
        webRtcClient.handleAnswer(fromId, sdp)
    }

    override fun onOffer(fromId: Int, sdp: String) {
        // This app never joins as a "viewer" itself, so it should never
        // receive an offer - present only to satisfy the interface.
    }

    override fun onCandidate(fromId: Int, sdpMid: String?, sdpMLineIndex: Int, candidate: String) {
        webRtcClient.addIceCandidate(fromId, sdpMid, sdpMLineIndex, candidate)
    }

    override fun onPeerLeft(peerId: Int) {
        webRtcClient.removePeer(peerId)
        setStatus("Call ended. Waiting for a call ($ROOM_NAME)")
    }

    override fun onSocketError(message: String) {
        setStatus("Signaling: $message")
    }

    // ---- WebRtcClient.Callbacks (WebRTC executor thread) ----

    override fun onLocalSdpOffer(viewerId: Int, sdp: SessionDescription) {
        signalingClient.sendOffer(viewerId, sdp.description)
    }

    override fun onIceCandidate(viewerId: Int, candidate: IceCandidate) {
        signalingClient.sendCandidate(viewerId, candidate.sdpMid, candidate.sdpMLineIndex, candidate.sdp)
    }

    override fun onPeerClosed(viewerId: Int) {
        setStatus("Call ended. Waiting for a call ($ROOM_NAME)")
    }

    override fun onStatus(text: String) {
        setStatus(text)
    }

    @Suppress("DEPRECATION")
    override fun onDestroy() {
        super.onDestroy()
        if (::signalingClient.isInitialized) signalingClient.close()
        if (::webRtcClient.isInitialized) webRtcClient.release()
        if (::remoteRenderer.isInitialized) remoteRenderer.release()
        if (::eglBase.isInitialized) eglBase.release()
        audioManager?.let {
            it.isSpeakerphoneOn = savedSpeakerphone
            it.mode = savedAudioMode
        }
    }
}
