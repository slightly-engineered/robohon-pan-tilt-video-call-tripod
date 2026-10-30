package com.colson.robovidcall

import android.content.Context
import android.util.Log
import org.webrtc.*
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit

/**
 * RECEIVE-ONLY WebRTC wrapper. This device has no camera, so nothing is
 * captured or sent: for each remote peer (the iPhone page) we create one
 * PeerConnection with a recvonly video + recvonly audio transceiver, send
 * an offer, and render whatever video comes back into [attachRemoteRenderer]'s
 * view. Remote audio plays automatically through WebRTC's audio device.
 *
 * Threading: every peer/factory operation runs on ONE private executor
 * thread, so state is never touched concurrently and PeerConnections are
 * never disposed from inside their own callbacks (which can deadlock).
 * Public methods are safe to call from any thread.
 */
class WebRtcClient(
    private val context: Context,
    private val eglBase: EglBase,
    private val callbacks: Callbacks
) {
    interface Callbacks {
        fun onLocalSdpOffer(viewerId: Int, sdp: SessionDescription)
        fun onIceCandidate(viewerId: Int, candidate: IceCandidate)
        fun onPeerClosed(viewerId: Int)
        fun onStatus(text: String)
    }

    companion object {
        private const val TAG = "WebRtcClient"

        private val ICE_SERVERS = listOf(
            PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer()
        )

        /**
         * Decode with the SoC's hardware decoder (MediaCodec). Fast and cool
         * if the MSM8926 decoder cooperates. If video is green, garbled,
         * frozen, or the app dies in MediaCodec, set this to false to use
         * WebRTC's built-in software decoders (heavier on CPU, but always works).
         */
        private const val USE_HARDWARE_DECODER = true
    }

    /** All fields are only touched on [executor]. */
    private class Peer(val pc: PeerConnection) {
        var remoteSet = false          // remote description applied yet?
        var offerSent = false          // offer handed to signaling yet?
        val pendingRemote = ArrayList<IceCandidate>()   // arrived before remoteSet
        val pendingLocal = ArrayList<IceCandidate>()    // gathered before offerSent
        var videoTrack: VideoTrack? = null
    }

    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private val peers = HashMap<Int, Peer>()

    private lateinit var factory: PeerConnectionFactory
    @Volatile private var remoteRenderer: SurfaceViewRenderer? = null

    private fun post(block: () -> Unit) {
        try {
            executor.execute {
                try {
                    block()
                } catch (e: Exception) {
                    Log.e(TAG, "task failed", e)
                }
            }
        } catch (e: RejectedExecutionException) {
            // already released
        }
    }

    fun init() {
        val options = PeerConnectionFactory.InitializationOptions
            .builder(context)
            .createInitializationOptions()
        PeerConnectionFactory.initialize(options)

        // We never send video, so the software encoder factory is enough and
        // keeps WebRTC from probing this SoC's hardware encoders at all.
        val encoderFactory: VideoEncoderFactory = SoftwareVideoEncoderFactory()
        val decoderFactory: VideoDecoderFactory =
            if (USE_HARDWARE_DECODER) DefaultVideoDecoderFactory(eglBase.eglBaseContext)
            else SoftwareVideoDecoderFactory()

        factory = PeerConnectionFactory.builder()
            .setVideoEncoderFactory(encoderFactory)
            .setVideoDecoderFactory(decoderFactory)
            .createPeerConnectionFactory()
    }

    /** Call once, from the UI thread, before any call comes in. */
    fun attachRemoteRenderer(renderer: SurfaceViewRenderer) {
        remoteRenderer = renderer
        renderer.init(eglBase.eglBaseContext, object : RendererCommon.RendererEvents {
            override fun onFirstFrameRendered() {
                callbacks.onStatus("On call - video playing")
            }

            override fun onFrameResolutionChanged(videoWidth: Int, videoHeight: Int, rotation: Int) {
                Log.i(TAG, "remote video ${videoWidth}x$videoHeight rotation=$rotation")
            }
        })
        renderer.setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
        renderer.setEnableHardwareScaler(true)
    }

    /** A viewer joined: build a receive-only connection and send it an offer. */
    fun createOfferForViewer(viewerId: Int) = post {
        // One call at a time. A leftover peer (phone slept, tab reloaded)
        // must not keep drawing into the same screen.
        for (id in peers.keys.toList()) closePeer(id, notify = false)

        val rtcConfig = PeerConnection.RTCConfiguration(ICE_SERVERS).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }

        val holder = arrayOfNulls<Peer>(1)   // lets the observer see its own Peer

        val observer = object : SimplePeerConnectionObserver(TAG) {
            override fun onIceCandidate(candidate: IceCandidate) {
                post {
                    val p = holder[0]
                    if (p == null || peers[viewerId] !== p) return@post
                    // Candidates must not overtake the offer they belong to.
                    if (p.offerSent) callbacks.onIceCandidate(viewerId, candidate)
                    else p.pendingLocal.add(candidate)
                }
            }

            override fun onTrack(transceiver: RtpTransceiver) {
                post { attachTrack(viewerId, holder[0], transceiver.receiver.track()) }
            }

            override fun onAddTrack(receiver: RtpReceiver, streams: Array<out MediaStream>) {
                post { attachTrack(viewerId, holder[0], receiver.track()) }
            }

            override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {
                Log.i(TAG, "viewer $viewerId ice: $state")
                post {
                    val p = holder[0]
                    if (p == null || peers[viewerId] !== p) return@post
                    when (state) {
                        PeerConnection.IceConnectionState.CHECKING ->
                            callbacks.onStatus("Connecting...")
                        PeerConnection.IceConnectionState.CONNECTED,
                        PeerConnection.IceConnectionState.COMPLETED ->
                            callbacks.onStatus("Connected - waiting for video")
                        PeerConnection.IceConnectionState.DISCONNECTED ->
                            callbacks.onStatus("Connection interrupted...")
                        PeerConnection.IceConnectionState.FAILED,
                        PeerConnection.IceConnectionState.CLOSED -> {
                            callbacks.onStatus("Connection $state")
                            closePeer(viewerId, notify = true)
                        }
                        else -> {}
                    }
                }
            }
        }

        val pc = factory.createPeerConnection(rtcConfig, observer)
        if (pc == null) {
            callbacks.onStatus("createPeerConnection failed")
            return@post
        }
        val peer = Peer(pc)
        holder[0] = peer
        peers[viewerId] = peer

        // Receive-only: no tracks added, just two recvonly m-lines.
        val recvOnly = RtpTransceiver.RtpTransceiverInit(
            RtpTransceiver.RtpTransceiverDirection.RECV_ONLY
        )
        pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO, recvOnly)
        pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO, recvOnly)

        pc.createOffer(object : SimpleSdpObserver() {
            override fun onCreateSuccess(desc: SessionDescription?) {
                if (desc == null) return
                post {
                    if (peers[viewerId] !== peer) return@post
                    pc.setLocalDescription(object : SimpleSdpObserver() {
                        override fun onSetSuccess() {
                            post {
                                if (peers[viewerId] !== peer) return@post
                                peer.offerSent = true
                                callbacks.onLocalSdpOffer(viewerId, desc)
                                for (c in peer.pendingLocal) callbacks.onIceCandidate(viewerId, c)
                                peer.pendingLocal.clear()
                            }
                        }
                    }, desc)
                }
            }

            override fun onCreateFailure(p0: String?) {
                super.onCreateFailure(p0)
                callbacks.onStatus("createOffer failed: $p0")
            }
        }, MediaConstraints())
    }

    private fun attachTrack(viewerId: Int, peer: Peer?, track: MediaStreamTrack?) {
        if (peer == null || peers[viewerId] !== peer) return
        if (track is VideoTrack) {
            peer.videoTrack = track
            track.setEnabled(true)
            remoteRenderer?.let { track.addSink(it) }   // adding the same sink twice is a no-op
        }
    }

    fun handleAnswer(viewerId: Int, sdp: String) = post {
        val peer = peers[viewerId] ?: return@post
        peer.pc.setRemoteDescription(object : SimpleSdpObserver() {
            override fun onSetSuccess() {
                post {
                    if (peers[viewerId] !== peer) return@post
                    peer.remoteSet = true
                    for (c in peer.pendingRemote) peer.pc.addIceCandidate(c)
                    peer.pendingRemote.clear()
                }
            }

            override fun onSetFailure(p0: String?) {
                super.onSetFailure(p0)
                callbacks.onStatus("setRemoteDescription failed: $p0")
            }
        }, SessionDescription(SessionDescription.Type.ANSWER, sdp))
    }

    fun addIceCandidate(viewerId: Int, sdpMid: String?, sdpMLineIndex: Int, candidate: String) = post {
        val peer = peers[viewerId] ?: return@post
        val ice = IceCandidate(sdpMid, sdpMLineIndex, candidate)
        if (peer.remoteSet) peer.pc.addIceCandidate(ice) else peer.pendingRemote.add(ice)
    }

    fun removePeer(viewerId: Int) = post { closePeer(viewerId, notify = false) }

    /** Executor thread only. */
    private fun closePeer(viewerId: Int, notify: Boolean) {
        val peer = peers.remove(viewerId) ?: return
        val renderer = remoteRenderer
        val track = peer.videoTrack
        if (track != null && renderer != null) {
            try { track.removeSink(renderer) } catch (e: Exception) { Log.w(TAG, "removeSink", e) }
        }
        try {
            peer.pc.close()
            peer.pc.dispose()
        } catch (e: Exception) {
            Log.w(TAG, "closing peer", e)
        }
        if (peers.isEmpty()) renderer?.clearImage()   // don't leave the last frame frozen on screen
        if (notify) callbacks.onPeerClosed(viewerId)
    }

    fun release() {
        val done = try {
            executor.submit {
                for (id in peers.keys.toList()) closePeer(id, notify = false)
                if (::factory.isInitialized) factory.dispose()
            }
        } catch (e: RejectedExecutionException) {
            null
        }
        try {
            done?.get(3, TimeUnit.SECONDS)
        } catch (e: Exception) {
            Log.w(TAG, "release timed out", e)
        }
        executor.shutdown()
    }
}

/** SdpObserver with every method defaulted to a no-op except the ones you override. */
open class SimpleSdpObserver : SdpObserver {
    override fun onCreateSuccess(p0: SessionDescription?) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(p0: String?) { Log.w("SimpleSdpObserver", "create failure: $p0") }
    override fun onSetFailure(p0: String?) { Log.w("SimpleSdpObserver", "set failure: $p0") }
}

/** PeerConnection.Observer with every method defaulted to a no-op except the ones you override. */
open class SimplePeerConnectionObserver(private val tag: String) : PeerConnection.Observer {
    override fun onSignalingChange(p0: PeerConnection.SignalingState?) {}
    override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {
        Log.i(tag, "ice connection state: $state")
    }
    override fun onIceConnectionReceivingChange(p0: Boolean) {}
    override fun onIceGatheringChange(p0: PeerConnection.IceGatheringState?) {}
    override fun onIceCandidate(candidate: IceCandidate) {}
    override fun onIceCandidatesRemoved(p0: Array<out IceCandidate>?) {}
    override fun onAddStream(p0: MediaStream?) {}
    override fun onRemoveStream(p0: MediaStream?) {}
    override fun onDataChannel(p0: DataChannel?) {}
    override fun onRenegotiationNeeded() {}
    override fun onAddTrack(receiver: RtpReceiver, streams: Array<out MediaStream>) {}
    override fun onTrack(transceiver: RtpTransceiver) {}
}
