package io.nocturne.phone

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.core.view.WindowCompat
import io.nocturne.phone.ui.AppRoot
import io.nocturne.phone.ui.theme.NocturneTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val app = application as NocturneApp
        // savedInstanceState != null means a config-change recreate re-delivering
        // the same shortcut intent — the sheet was already opened once.
        if (savedInstanceState == null) handleGapIntent(intent)
        setContent { NocturneTheme { AppRoot(app) } }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleGapIntent(intent)
    }

    private fun handleGapIntent(intent: Intent?) {
        if (intent?.action == ACTION_LOG_GAP) {
            (application as NocturneApp).container.gapRequests.value = true
        }
    }

    companion object {
        const val ACTION_LOG_GAP = "io.nocturne.phone.action.LOG_GAP"
    }
}
