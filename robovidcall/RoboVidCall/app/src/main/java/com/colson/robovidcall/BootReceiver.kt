package com.colson.robovidcall

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Runs at power-on (and after each app update) and starts the service that
 * launches the app and keeps it in front.
 *
 * Android only delivers BOOT_COMPLETED to an app that has been launched at
 * least once since it was installed - after `adb install`, start it once with
 * `adb shell am start -n com.colson.robovidcall/.MainActivity`.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action
        if (action == Intent.ACTION_BOOT_COMPLETED || action == Intent.ACTION_MY_PACKAGE_REPLACED) {
            context.startService(Intent(context, KeepFrontService::class.java))
        }
    }
}
