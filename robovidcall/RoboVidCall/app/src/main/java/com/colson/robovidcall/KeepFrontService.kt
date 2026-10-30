package com.colson.robovidcall

import android.app.Service
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper

/**
 * Kiosk keeper. Every couple of seconds it checks whether MainActivity is
 * in front and, if not, brings it back. That covers:
 *   - launching the app at boot (nobody has to tap anything),
 *   - Sharp's "sign up for COCORO Plan" popup (or any other screen) appearing
 *     on top of it,
 *   - the app being killed (START_STICKY restarts this service, which relaunches it).
 *
 * To stop it while debugging over adb:
 *   adb shell am force-stop com.colson.robovidcall
 */
class KeepFrontService : Service() {

    private val handler = Handler(Looper.getMainLooper())

    private val check = object : Runnable {
        override fun run() {
            if (!MainActivity.isResumed) {
                val intent = Intent(this@KeepFrontService, MainActivity::class.java).addFlags(
                    Intent.FLAG_ACTIVITY_NEW_TASK or
                        Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or
                        Intent.FLAG_ACTIVITY_SINGLE_TOP
                )
                startActivity(intent)
            }
            handler.postDelayed(this, CHECK_INTERVAL_MS)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        handler.removeCallbacks(check)
        handler.postDelayed(check, FIRST_CHECK_DELAY_MS)
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        handler.removeCallbacks(check)
        super.onDestroy()
    }

    companion object {
        private const val FIRST_CHECK_DELAY_MS = 1000L
        private const val CHECK_INTERVAL_MS = 2000L
    }
}
