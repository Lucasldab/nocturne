package io.nocturne.phone.ui.system

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Quick task 260428-7zc — System hub. Lists the four sub-screens and offers
 * a back-arrow to the previous tab. Bottom-bar (mini-player + nav) stays
 * visible here so the user can flip back to a music tab without going
 * through Back; only the four sub-screens hide chrome.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SystemHubScreen(
    onRotation: () -> Unit,
    onSync: () -> Unit,
    onStorage: () -> Unit,
    onStats: () -> Unit,
    onBack: () -> Unit,
) {
    val rows = listOf(
        HubRow("rotation", "smart buckets · last rotation —", onRotation),
        HubRow("sync",     "syncthing · last sync —",         onSync),
        HubRow("storage",  "budget · used —",                 onStorage),
        HubRow("stats",    "last 7 days · plays —",           onStats),
    )
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("System", style = MaterialTheme.typography.titleMedium) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Back",
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                    titleContentColor = MaterialTheme.colorScheme.onSurface,
                    navigationIconContentColor = MaterialTheme.colorScheme.onSurface,
                ),
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background)
                .padding(padding)
                .padding(horizontal = 16.dp),
        ) {
            TerminalPrompt("~/system", modifier = Modifier.padding(top = 16.dp))
            ScreenHero("system")
            LazyColumn(modifier = Modifier.padding(top = 16.dp)) {
                items(rows, key = { it.title }) { row ->
                    SystemHubRow(row)
                }
            }
        }
    }
}

private data class HubRow(val title: String, val tagline: String, val onClick: () -> Unit)

@Composable
private fun SystemHubRow(row: HubRow) {
    Box(modifier = Modifier
        .fillMaxWidth()
        .clickable(onClick = row.onClick)) {
        Column {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 0.dp, vertical = 12.dp),
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = row.title,
                        style = TextStyle(
                            fontFamily = MaterialTheme.typography.bodyMedium.fontFamily,
                            fontSize = 14.sp,
                        ),
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        text = row.tagline,
                        style = TextStyle(
                            fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                            fontSize = 11.sp,
                        ),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 2.dp),
                    )
                }
                Text(
                    text = "›",
                    style = TextStyle(
                        fontFamily = MaterialTheme.typography.bodyMedium.fontFamily,
                        fontSize = 18.sp,
                        fontWeight = FontWeight.Normal,
                    ),
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
}
