package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

/**
 * Long-press bottom sheet — terminal-aesthetic to match SyncScreen / StatsScreen.
 *
 * Header reads `~/<unit>/<name>` like a shell path. Each action row is a
 * `$ command` line in primary purple, with a muted-mono subtitle below.
 * Confirm dialog uses the same mono typography (no Material titleLarge/
 * bodyLarge defaults that look out of place against the rest of the app).
 *
 * Two actions:
 *   $ unpin & unload         — clear pin + drop resident copy. Reversible.
 *   $ delete (didn't like)   — confirm → archive + resident files nuked,
 *                               sha blacklisted, DB rows dropped. Permanent.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TrackActionsSheet(
    displayName: String,
    isAlbum: Boolean,
    onUnsync: () -> Unit,
    onDelete: () -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState()
    val scope = rememberCoroutineScope()
    var showDeleteConfirm by remember { mutableStateOf(false) }

    val unitWord = if (isAlbum) "album" else "track"

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.background,
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(bottom = 16.dp)) {
            // ~/track/<name> — terminal-prompt header in muted mono.
            Text(
                text = "~/$unitWord",
                style = TextStyle(
                    fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                    fontSize = 11.sp,
                    letterSpacing = 1.sp,
                ),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
            Text(
                text = displayName,
                style = MonoText(13),
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
            )

            DividerLine()

            CommandRow(
                command = "unpin & unload",
                hint = "removes from phone — reversible",
                onClick = {
                    scope.launch { sheetState.hide() }
                    onUnsync()
                    onDismiss()
                },
            )

            DividerLine()

            CommandRow(
                command = "delete (didn't like)",
                hint = "removes from desktop and phone — permanent",
                destructive = true,
                onClick = { showDeleteConfirm = true },
            )

            DividerLine()
        }
    }

    if (showDeleteConfirm) {
        AlertDialog(
            onDismissRequest = { showDeleteConfirm = false },
            containerColor = MaterialTheme.colorScheme.surface,
            title = {
                Text(
                    text = "── delete $unitWord ──",
                    style = MonoText(12).copy(letterSpacing = 1.5.sp),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            },
            text = {
                Text(
                    text = "$displayName\n\nfiles unlinked from archive + resident, " +
                        "sha blacklisted (future re-imports refused), db rows " +
                        "dropped. cannot be undone.",
                    style = MonoText(12),
                    color = MaterialTheme.colorScheme.onSurface,
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        showDeleteConfirm = false
                        scope.launch { sheetState.hide() }
                        onDelete()
                        onDismiss()
                    },
                ) {
                    Text(
                        text = "$ delete",
                        style = MonoText(13),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteConfirm = false }) {
                    Text(
                        text = "$ cancel",
                        style = MonoText(13),
                        color = MaterialTheme.colorScheme.primary,
                    )
                }
            },
        )
    }
}

@Composable
private fun CommandRow(
    command: String,
    hint: String,
    destructive: Boolean = false,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Column {
            Text(
                text = "$ $command",
                style = MonoText(14),
                color = if (destructive) MaterialTheme.colorScheme.error
                        else MaterialTheme.colorScheme.primary,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                text = hint,
                style = MonoText(11),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun DividerLine() {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant),
    )
}

@Composable
private fun MonoText(sizeSp: Int): TextStyle = TextStyle(
    fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
    fontSize = sizeSp.sp,
)
