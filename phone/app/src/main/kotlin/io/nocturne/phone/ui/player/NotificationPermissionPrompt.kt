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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat

/**
 * UI-SPEC Surface 4 / POST_NOTIFICATIONS prompt timing contract:
 *   - On FIRST PLAY, if POST_NOTIFICATIONS is not granted (Android 13+),
 *     show a Material3 AlertDialog with a rationale, then trigger the
 *     system permission request on "Allow".
 *   - If denied, proceed directly to playback without re-prompting -- the
 *     notification surface degrades silently (lock-screen / BT controls absent,
 *     audio still plays). (RESEARCH.md Pitfall 8)
 *   - On pre-Android-13 devices, POST_NOTIFICATIONS is implicitly granted --
 *     the callable runs onProceed immediately.
 *
 * Animation gate: AlertDialog is a Material3 component with no AnimatedVisibility
 * wrapper added by project code. The dialog enter/exit animation is library-internal.
 *
 * Returns a zero-argument callable that the caller invokes when playback is
 * first initiated. Subsequent calls after the first play are a no-op prompt-wise
 * (the permission is either granted or the caller already invoked the prompt once).
 */
@Composable
fun rememberNotificationPermissionPrompt(
    onProceed: () -> Unit,
): () -> Unit {
    val context = LocalContext.current
    var showRationale by remember { mutableStateOf(false) }

    val launcher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission(),
    ) { _ ->
        // Whether granted or denied, proceed with playback either way per
        // UI-SPEC degradation contract (Pitfall 8).
        onProceed()
    }

    fun isGranted(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return true
        return ContextCompat.checkSelfPermission(
            context, Manifest.permission.POST_NOTIFICATIONS,
        ) == PackageManager.PERMISSION_GRANTED
    }

    if (showRationale) {
        AlertDialog(
            onDismissRequest = {
                showRationale = false
                onProceed()
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
                    launcher.launch(Manifest.permission.POST_NOTIFICATIONS)
                }) { Text("Allow") }
            },
            dismissButton = {
                TextButton(onClick = {
                    showRationale = false
                    onProceed()
                }) { Text("Not now") }
            },
        )
    }

    return remember(isGranted()) {
        {
            if (isGranted()) {
                onProceed()
            } else {
                showRationale = true
            }
        }
    }
}
