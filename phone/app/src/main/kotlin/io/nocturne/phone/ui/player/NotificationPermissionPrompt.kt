package io.nocturne.phone.ui.player

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.launch

/**
 * Single AppRoot-hosted gate. Shows the rationale at most once per install
 * lifetime. After ANY terminal state (Allow + system-dialog returned, "Not now",
 * outside-tap dismiss) the gate sets notifPromptShown=true via SyncPrefs and
 * never shows again.
 *
 * Decision branches when a non-null pendingAction arrives:
 *   - SDK<33 OR isGranted() OR notifPromptShown == true → run pendingAction +
 *     onConsumed immediately, no dialog.
 *   - else → show the rationale AlertDialog and KEEP pendingAction queued
 *     until a terminal state resolves.
 *
 * Terminal states ALL do the same three things, in order:
 *   (1) hide the dialog
 *   (2) coroutine-launch syncPrefs.setNotifPromptShown(true)
 *   (3) run pendingAction; call onConsumed
 *
 * For the Allow path, step (3) runs in the launcher's result callback (after
 * the system dialog dismisses), NOT in the Allow tap onClick — otherwise
 * playback would start while the OS dialog is still on screen.
 *
 * Animation gate: AlertDialog is a Material3 component with no AnimatedVisibility
 * wrapper added by project code. The dialog enter/exit animation is library-internal.
 */
@Composable
fun FirstPlayNotifGate(
    syncPrefs: SyncPrefs,
    pendingAction: (() -> Unit)?,
    onConsumed: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val notifPromptShown by syncPrefs.notifPromptShown.collectAsStateWithLifecycle(initialValue = false)
    var showRationale by remember { mutableStateOf(false) }
    // Holds the action that the (asynchronous) system permission dialog must
    // consume after it returns. This is set in the Allow onClick and cleared
    // in the launcher result callback — DO NOT clear it on the Allow tap.
    var queuedForLauncher by remember { mutableStateOf<(() -> Unit)?>(null) }

    fun isGranted(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return true
        return ContextCompat.checkSelfPermission(
            context, Manifest.permission.POST_NOTIFICATIONS,
        ) == PackageManager.PERMISSION_GRANTED
    }

    val launcher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission(),
    ) { _ ->
        // Whether granted or denied, persist the shown flag and run the queued
        // action exactly once.
        scope.launch { syncPrefs.setNotifPromptShown(true) }
        val action = queuedForLauncher
        queuedForLauncher = null
        action?.invoke()
        onConsumed()
    }

    // Drive the run-immediately vs show-rationale decision from a side effect
    // keyed on pendingAction identity so re-entries pick up new actions.
    LaunchedEffect(pendingAction) {
        val action = pendingAction ?: return@LaunchedEffect
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU || isGranted() || notifPromptShown) {
            action()
            onConsumed()
        } else {
            showRationale = true
        }
    }

    if (showRationale) {
        AlertDialog(
            onDismissRequest = {
                showRationale = false
                scope.launch { syncPrefs.setNotifPromptShown(true) }
                pendingAction?.invoke()
                onConsumed()
            },
            title = { Text("Notifications", style = MaterialTheme.typography.titleMedium) },
            text = {
                Text(
                    // UI-SPEC Copywriting Contract
                    "Notifications let you control playback from the lock screen. Tap outside to skip.",
                    style = MaterialTheme.typography.bodyMedium,
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    showRationale = false
                    // Defer onConsumed until the launcher's callback so playback
                    // does not start while the system dialog is on screen.
                    queuedForLauncher = pendingAction
                    launcher.launch(Manifest.permission.POST_NOTIFICATIONS)
                }) { Text("Allow") }
            },
            dismissButton = {
                TextButton(onClick = {
                    showRationale = false
                    scope.launch { syncPrefs.setNotifPromptShown(true) }
                    pendingAction?.invoke()
                    onConsumed()
                }) { Text("Not now") }
            },
        )
    }
}

