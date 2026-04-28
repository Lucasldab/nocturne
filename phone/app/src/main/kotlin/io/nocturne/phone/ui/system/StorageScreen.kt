package io.nocturne.phone.ui.system

import android.os.StatFs
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import io.nocturne.phone.data.AppContainer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Quick task 260428-ja8 — Storage / budget screen, now an inline utility-mode
 * content slot (no Scaffold / TopAppBar / back-button — the BrowserRoot shell
 * owns chrome). Mirrors
 * /tmp/nocturne-design/nocturne/project/screens-system.jsx lines 199-263.
 *
 * Donut-fill summary + per-bucket bytes breakdown + 4..32 GB cap slider
 * persisted via SyncPrefs.storageBudgetGb. The slider is currently
 * advisory — the daemon's config.toml carries the authoritative cap; a
 * future plan can push slider edits back via the meta folder.
 *
 * Device free / total reads StatFs(filesDir) — app private dir, no
 * permission required.
 */
@Composable
fun StorageScreen(container: AppContainer) {
    val vm: SystemViewModel = viewModel(factory = SystemViewModel.Factory(container))
    LaunchedEffect(Unit) { vm.refreshRotation() }
    val view by vm.rotation.collectAsStateWithLifecycle()
    val budgetGb by container.syncPrefs.storageBudgetGb.collectAsStateWithLifecycle(initialValue = 12)
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()

    var deviceFreeGb by remember { mutableFloatStateOf(0f) }
    var deviceTotalGb by remember { mutableFloatStateOf(0f) }
    LaunchedEffect(Unit) {
        val (free, total) = withContext(Dispatchers.IO) {
            val s = StatFs(ctx.filesDir.path)
            (s.availableBytes / 1e9).toFloat() to (s.totalBytes / 1e9).toFloat()
        }
        deviceFreeGb = free
        deviceTotalGb = total
    }

    // Float-backed slider state. Material3 Slider's onValueChange fires with
    // continuous floats; rounding to Int per-callback froze the thumb until the
    // drag crossed a whole-GB boundary (sluggish-feeling slider). We hold the
    // float live, snap-to-int via `steps`, and only persist the int on release.
    var localBudget by remember(budgetGb) { mutableFloatStateOf(budgetGb.toFloat()) }
    val localBudgetInt = localBudget.toInt().coerceIn(4, 32)

    val usedBytes = view.totalUsedBytes
    val usedGb = (usedBytes / 1e9).toFloat()
    val capGbActual = localBudgetInt.toFloat()
    val pct = if (capGbActual > 0f) (usedGb / capGbActual).coerceAtLeast(0f) else 0f

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 16.dp)
            .padding(bottom = 80.dp)
            .verticalScroll(rememberScrollState()),
    ) {
        TerminalPrompt("~/storage", modifier = Modifier.padding(top = 16.dp))
        ScreenHero("budget")

        // Donut-ish vertical fill row.
        Row(
            verticalAlignment = Alignment.Bottom,
            modifier = Modifier.padding(top = 24.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(width = 96.dp, height = 140.dp)
                    .border(1.dp, MaterialTheme.colorScheme.surfaceVariant),
            ) {
                val fillFraction = pct.coerceAtMost(1f)
                val fillColor = if (pct > 0.92f) {
                    MaterialTheme.colorScheme.error
                } else {
                    MaterialTheme.colorScheme.primary
                }
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .fillMaxHeight(fillFraction)
                        .align(Alignment.BottomCenter)
                        .background(fillColor.copy(alpha = 0.85f)),
                )
                Text(
                    text = "${(pct * 100).toInt().coerceAtLeast(0)}%",
                    style = monoStyle(22).copy(fontWeight = FontWeight.SemiBold),
                    color = MaterialTheme.colorScheme.onBackground,
                    modifier = Modifier.align(Alignment.Center),
                )
            }
            Column(
                modifier = Modifier
                    .weight(1f)
                    .padding(start = 16.dp),
            ) {
                KvLine("used   ", "%.1f GB".format(usedGb))
                KvLine("cap    ", "%d GB".format(localBudgetInt))
                KvLine(
                    "free   ",
                    "%.1f GB".format((capGbActual - usedGb).coerceAtLeast(0f)),
                )
                KvLine("device ", "%.0f GB total".format(deviceTotalGb))
                KvLine("avail  ", "%.1f GB free".format(deviceFreeGb))
            }
        }

        // Slider design spec (locked 2026-04-28 storage-cap-slider):
        //   accent (thumb + active track) — #8C41B4 (deeper purple than primary
        //   #7E3AA0; matches the reference accent-color)
        //   labels (section header + 4/32 GB endpoints) — #8A8A8A literal
        //   (NOT NocturneOnSurfaceMuted #9E9689 which is warmer beige used for
        //   the rest of the muted-text surface)
        val sliderAccent = androidx.compose.ui.graphics.Color(0xFF8C41B4)
        val sliderMuted = androidx.compose.ui.graphics.Color(0xFF8A8A8A)
        SectionHeader("cap · drag to set", color = sliderMuted)
        Slider(
            value = localBudget,
            onValueChange = { localBudget = it },
            onValueChangeFinished = {
                scope.launch { container.syncPrefs.setStorageBudgetGb(localBudget.toInt().coerceIn(4, 32)) }
            },
            valueRange = 4f..32f,
            // steps=0 → continuous slider, no tick dots drawn at all. Snap-to-
            // int still happens via toInt().coerceIn on commit.
            colors = SliderDefaults.colors(
                thumbColor = sliderAccent,
                activeTrackColor = sliderAccent,
                inactiveTrackColor = androidx.compose.ui.graphics.Color(0xFF3A3A3A),
            ),
        )
        Row(
            horizontalArrangement = Arrangement.SpaceBetween,
            modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
        ) {
            Text(text = "4 GB", style = monoStyle(11), color = sliderMuted)
            Text(text = "32 GB", style = monoStyle(11), color = sliderMuted)
        }

        SectionHeader("by bucket")
        val nonZeroUsed = usedBytes.coerceAtLeast(1L)
        view.buckets.forEachIndexed { i, b ->
            BucketStorageRow(index = i, row = b, totalBytes = nonZeroUsed)
        }
    }
}

@Composable
private fun KvLine(key: String, value: String) {
    Row(modifier = Modifier.padding(vertical = 2.dp)) {
        Text(
            text = key,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

@Composable
private fun BucketStorageRow(index: Int, row: BucketRow, totalBytes: Long) {
    val w = (row.bytes.toFloat() / totalBytes.toFloat()).coerceIn(0f, 1f)
    Column(modifier = Modifier.padding(vertical = 8.dp)) {
        Row(modifier = Modifier.fillMaxWidth()) {
            Text(
                text = row.label,
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = "%.1f GB".format(row.bytes / 1e9),
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        // 2dp progress bar with bucket-color fill.
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(2.dp)
                .padding(top = 6.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant),
        ) {
            Box(
                modifier = Modifier
                    .fillMaxWidth(w)
                    .fillMaxHeight()
                    .background(BucketColors.forIndex(index).copy(alpha = 0.8f)),
            )
        }
    }
}

// Suppress unused — kept to surface the canonical sp import in this file alongside dp.
@Suppress("unused") private val _spAnchor = 1.sp
