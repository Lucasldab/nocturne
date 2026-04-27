package io.nocturne.phone.ui.firstrun

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.catalog.ImportResult
import io.nocturne.phone.data.catalog.Stage
import io.nocturne.phone.ui.theme.NocturneTheme

/**
 * Renders ImportState transitions. Caller (AppRoot) drives navigation on
 * Succeeded via a LaunchedEffect — this composable is purely view-only.
 */
@Composable
fun ImportProgressScreen(
    state: ImportState,
    onRetry: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.Start,
    ) {
        when (state) {
            is ImportState.NotStarted, ImportState.PickingFolder -> {
                Text(
                    text = "Preparing import…",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onBackground,
                )
            }
            is ImportState.Importing -> {
                Text(
                    text = stageLabel(state.stage),
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Spacer(Modifier.height(8.dp))
                if (state.total > 0) {
                    Text(
                        text = "${state.done} / ${state.total}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(8.dp))
                    LinearProgressIndicator(
                        progress = { (state.done.toFloat() / state.total.toFloat()).coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                    )
                } else {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
            }
            is ImportState.Failed -> {
                Text(
                    text = "Import failed.",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MaterialTheme.colorScheme.error,
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    text = state.message,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(Modifier.height(24.dp))
                Button(onClick = onRetry) {
                    Text("RETRY", style = MaterialTheme.typography.labelLarge)
                }
            }
            is ImportState.Succeeded -> {
                Text(
                    text = "Imported ${state.result.tracksImported} tracks " +
                        "(${state.result.albumsImported} albums) " +
                        "in ${state.result.durationMs}ms.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                )
            }
        }
    }
}

private fun stageLabel(s: Stage): String = when (s) {
    Stage.PARSING_CATALOG -> "Parsing catalog.json…"
    Stage.INSERTING_TRACKS -> "Inserting tracks…"
    Stage.DERIVING_GROUPS -> "Deriving albums / artists / genres…"
    Stage.INSERTING_GROUPS -> "Saving groups…"
    Stage.APPLYING_MANIFEST -> "Applying manifest residency…"
    Stage.DONE -> "Done."
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun ImportProgressPreview_Importing() {
    NocturneTheme {
        ImportProgressScreen(
            state = ImportState.Importing(Stage.INSERTING_TRACKS, 4500, 15000),
            onRetry = {},
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun ImportProgressPreview_Failed() {
    NocturneTheme {
        ImportProgressScreen(
            state = ImportState.Failed("FileNotFoundException: catalog.json not found"),
            onRetry = {},
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun ImportProgressPreview_Succeeded() {
    NocturneTheme {
        ImportProgressScreen(
            state = ImportState.Succeeded(ImportResult(699, 65, 42, 12, 87, 1234)),
            onRetry = {},
        )
    }
}
