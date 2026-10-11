package io.nocturne.phone

import android.os.Bundle
import androidx.activity.ComponentActivity

/**
 * Target of the "Log a gap" launcher shortcut; has no UI.
 *
 * A static shortcut pointing straight at MainActivity launches it with
 * NEW_TASK|CLEAR_TASK, which (1) destroys a running app's task so navigation
 * resets to Home and onNewIntent is never reached, (2) makes LOG_GAP the task's
 * base intent so Back finishes the root on Android 12+, and (3) relaunches
 * LOG_GAP from Recents with no saved state, popping the sheet unasked.
 *
 * Instead this trampoline raises the request flag, then starts the plain
 * MAIN/LAUNCHER intent, so an existing task simply comes to front with its
 * navigation intact. BrowserRoot opens the sheet when it sees the flag.
 */
class LogGapActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        (application as NocturneApp).container.gapRequests.value = true
        packageManager.getLaunchIntentForPackage(packageName)?.let { startActivity(it) }
        finish()
    }
}
