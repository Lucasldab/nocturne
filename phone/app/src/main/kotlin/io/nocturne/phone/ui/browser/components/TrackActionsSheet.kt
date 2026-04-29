package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
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
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch

/**
 * Long-press bottom sheet for a track or album. Two actions:
 *
 *   "Unpin and unload" — clears any pin and drops the resident copy from
 *   the phone (daemon demotes via unsync_overrides). Stats preserved.
 *   Reversible by re-pinning.
 *
 *   "Delete (didn't like it)" — destructive: nukes the FLAC from archive
 *   AND the resident transcode AND the DB rows. Daemon blacklists the sha
 *   so future re-imports refuse to bring it back. Two-step (confirm).
 *
 * `isAlbum` switches between track and album semantics. Display name is
 * shown verbatim in the confirm dialog so the user knows what they're
 * about to nuke.
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

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp)) {
            Text(
                text = displayName,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            )

            ActionRow(
                title = "Unpin and unload",
                subtitle = "Removes from phone. Reversible.",
                onClick = {
                    scope.launch { sheetState.hide() }
                    onUnsync()
                    onDismiss()
                },
            )

            ActionRow(
                title = "Delete (didn't like it)",
                subtitle = "Removes from desktop AND phone. Permanent.",
                destructive = true,
                onClick = { showDeleteConfirm = true },
            )

            Spacer(Modifier.height(12.dp))
        }
    }

    if (showDeleteConfirm) {
        AlertDialog(
            onDismissRequest = { showDeleteConfirm = false },
            title = { Text(if (isAlbum) "Delete album?" else "Delete track?") },
            text = {
                Text(
                    "\"$displayName\" will be removed from the desktop archive, the " +
                        "phone, and your library forever. Future re-imports of the same " +
                        "content will be refused.\n\nThis can't be undone.",
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
                    Text("Delete", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteConfirm = false }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun ActionRow(
    title: String,
    subtitle: String,
    destructive: Boolean = false,
    onClick: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.bodyLarge,
            color = if (destructive) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurface,
        )
        Text(
            text = subtitle,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
