package io.nocturne.phone.ui.home

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.nocturne.phone.data.catalog.DiscoveryPick
import io.nocturne.phone.data.catalog.Recommendation

/**
 * Home / For You — the surface that makes the rotation engine visible.
 *
 * Before this existed, `nocturned discover` computed picks every Monday that
 * nothing ever displayed, and recommendations had no destination at all. The
 * browser tabs show what you HAVE; this shows what you have not heard and what
 * you do not own.
 */
@Composable
fun HomeScreen(vm: HomeViewModel) {
    val state by vm.state.collectAsStateWithLifecycle()

    LazyColumn(modifier = Modifier.fillMaxWidth()) {
        item {
            Spacer(Modifier.height(8.dp))
            SectionLabel("this week", state.picks.size)
        }
        if (state.picks.isEmpty()) {
            item { Hint(state.note ?: "no picks yet — discovery runs Mondays") }
        }
        items(state.picks, key = { it.id.ifBlank { it.query } }) { pick ->
            FeedRow(
                title = pick.title,
                subtitle = listOf(pick.artist, pick.album)
                    .filter { it.isNotBlank() }
                    .joinToString("  ·  "),
                trailing = pick.reason.replace("_", " "),
                requested = pick.query in state.requested,
                onTap = { vm.fetch(pick.query) },
            )
        }

        item {
            Spacer(Modifier.height(20.dp))
            SectionLabel("new to you", state.recommendations.size)
        }
        if (state.recommendations.isEmpty() && !state.loading) {
            item { Hint("no recommendations yet") }
        }
        items(state.recommendations, key = { it.artistMbid.ifBlank { it.name } }) { rec ->
            val q = rec.query ?: rec.name
            FeedRow(
                title = rec.topTrack ?: rec.name,
                subtitle = if (rec.topTrack != null) rec.name else rec.comment,
                // "because you listen to X" is the whole point — show the seed.
                trailing = rec.because.firstOrNull()?.let { "← $it" } ?: "",
                requested = q in state.requested,
                onTap = { vm.fetch(q) },
            )
        }
        item { Spacer(Modifier.height(24.dp)) }
    }
}

@Composable
private fun SectionLabel(text: String, count: Int) {
    Text(
        text = if (count > 0) "$text  ($count)" else text,
        fontFamily = FontFamily.Monospace,
        fontSize = 13.sp,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
    )
}

@Composable
private fun Hint(text: String) {
    Text(
        text = text,
        fontFamily = FontFamily.Monospace,
        fontSize = 12.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
    )
}

@Composable
private fun FeedRow(
    title: String,
    subtitle: String,
    trailing: String,
    requested: Boolean,
    onTap: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = !requested, onClick = onTap)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                fontFamily = FontFamily.Monospace,
                fontSize = 15.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                color = MaterialTheme.colorScheme.onSurface,
            )
            // One line, not two. Three-line rows showed ~10 items per screen and
            // repeated "never played" down the whole list, which carries no
            // information when every pick shares a reason.
            val detail = listOf(subtitle, trailing)
                .filter { it.isNotBlank() }
                .joinToString("  ·  ")
            if (detail.isNotBlank()) {
                Text(
                    text = detail,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Box(
            modifier = Modifier
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .padding(horizontal = 10.dp, vertical = 6.dp),
        ) {
            Text(
                text = if (requested) "QUEUED" else "GET",
                fontFamily = FontFamily.Monospace,
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurface,
            )
        }
    }
}
