package io.nocturne.phone.ui.firstrun

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.launch

/**
 * Step 2 of the first-run flow — pick the music-files Syncthing folder.
 *
 * Mirrors the visual style of [FirstRunScreen] (which picks the metadata
 * folder). Two pickers stacked across two screens keeps each step single-
 * purpose and matches the setup contract.
 *
 * Persists the granted SAF tree URI via `takePersistableUriPermission`
 * BEFORE storing it (Pitfall 29).
 */
@Composable
fun MusicFolderPickerScreen(
    onFolderPicked: (Uri) -> Unit,
    syncPrefs: SyncPrefs,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
            scope.launch {
                syncPrefs.setMusicTreeUri(uri.toString())
                onFolderPicked(uri)
            }
        }
    }
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.Start,
    ) {
        Text(
            text = "nocturne",
            style = MaterialTheme.typography.headlineLarge,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Spacer(Modifier.height(16.dp))
        Text(
            text = "Pick the music-files Syncthing folder. The phone reads track audio from this tree at playback time. Choose the same folder Syncthing-Fork is receiving the files into (the directory containing the synced track files).",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(8.dp))
        Text(
            text = "No internet permission. No telemetry. No accounts.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(32.dp))
        Button(onClick = { launcher.launch(null) }) {
            Text("PICK MUSIC FOLDER", style = MaterialTheme.typography.labelLarge)
        }
    }
}
