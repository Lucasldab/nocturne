package io.nocturne.phone.ui.system

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import io.nocturne.phone.data.AppContainer

/**
 * Quick task 260428-ja8 — Rotation / smart-buckets dashboard, now an inline
 * utility-mode content slot (no Scaffold / TopAppBar / back-button — the
 * BrowserRoot shell owns chrome). Mirrors
 * /tmp/nocturne-design/nocturne/project/screens-system.jsx lines 11-80.
 *
 * Renders bucket-bar (proportional fill) + per-bucket rows + an honest
 * empty state for "recent rotations" — the daemon's rotation log is not
 * surfaced on-device by design (CROSS-03 forbids hitting the daemon REST
 * from the phone), so we don't fake one.
 */
@Composable
fun RotationScreen(container: AppContainer) {
    val vm: SystemViewModel = viewModel(factory = SystemViewModel.Factory(container))
    LaunchedEffect(Unit) { vm.refreshRotation() }
    val view by vm.rotation.collectAsStateWithLifecycle()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 16.dp)
            .padding(bottom = 80.dp)
            .verticalScroll(rememberScrollState()),
    ) {
        TerminalPrompt("~/rotation", modifier = Modifier.padding(top = 16.dp))
        ScreenHero("smart buckets")
        Text(
            text = view.generatedAt?.let { "last rotation $it" } ?: "no manifest yet",
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 6.dp),
        )

        // Bucket bar — proportional widths.
        BucketBar(view, modifier = Modifier.padding(top = 20.dp))

        // Cap summary line.
        val usedGb = view.totalUsedBytes / 1e9
        val capGb = view.capBytes / 1e9
        val pct = if (view.capBytes > 0) (view.totalUsedBytes.toDouble() / view.capBytes) else 0.0
        Row(
            horizontalArrangement = Arrangement.SpaceBetween,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
        ) {
            Text(
                text = "%.1f GB resident".format(usedGb),
                style = monoStyle(11),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = "${(pct * 100).toInt()}% of %.0f GB cap".format(capGb),
                style = monoStyle(11),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        SectionHeader("buckets")
        view.buckets.forEachIndexed { i, b ->
            BucketRowView(
                index = i,
                row = b,
                totalBytes = view.totalUsedBytes.coerceAtLeast(1L),
            )
        }

        SectionHeader("recent rotations")
        Text(
            text = "rotation log not surfaced on phone",
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 4.dp),
        )
    }
}

@Composable
private fun BucketBar(view: RotationView, modifier: Modifier = Modifier) {
    if (view.buckets.isEmpty()) {
        Box(
            modifier = modifier
                .fillMaxWidth()
                .height(28.dp)
                .border(1.dp, MaterialTheme.colorScheme.surfaceVariant),
        )
        return
    }
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(28.dp)
            .border(1.dp, MaterialTheme.colorScheme.surfaceVariant),
    ) {
        view.buckets.forEachIndexed { i, b ->
            // Weight = bytes (clamped so zero-byte buckets still draw a sliver).
            val weight = b.bytes.toFloat().coerceAtLeast(1f)
            Box(
                modifier = Modifier
                    .weight(weight)
                    .fillMaxHeight()
                    .background(BucketColors.forIndex(i).copy(alpha = 0.7f)),
            )
            // 1dp inter-segment hairline (bg color) so segments read distinct.
            if (i != view.buckets.lastIndex) {
                Box(
                    modifier = Modifier
                        .width(1.dp)
                        .fillMaxHeight()
                        .background(MaterialTheme.colorScheme.background),
                )
            }
        }
    }
}

@Composable
private fun BucketRowView(index: Int, row: BucketRow, totalBytes: Long) {
    val pct = if (totalBytes > 0) (row.bytes * 100 / totalBytes).toInt() else 0
    Column {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 12.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(width = 6.dp, height = 32.dp)
                    .background(BucketColors.forIndex(index)),
            )
            Column(
                modifier = Modifier
                    .weight(1f)
                    .padding(start = 12.dp),
            ) {
                Text(
                    text = row.label,
                    style = monoStyle(14),
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    text = "${row.count} tracks · %.1f GB".format(row.bytes / 1e9),
                    style = monoStyle(11),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 2.dp),
                )
            }
            Text(
                text = "$pct%",
                style = monoStyle(11),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant),
        )
    }
}

/**
 * Helper: monospaced text style at a fixed sp size, using the project's
 * mono family (bodySmall.fontFamily resolves to JetBrains Mono via Typography).
 */
@Composable
internal fun monoStyle(sizeSp: Int): TextStyle = TextStyle(
    fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
    fontSize = sizeSp.sp,
)
