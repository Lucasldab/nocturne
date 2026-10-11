package io.nocturne.phone.ui.gap

import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.stats.GapsWriter
import io.nocturne.phone.ui.theme.JetBrainsMono
import kotlinx.coroutines.launch

/**
 * One-line friction log. `send` is GapsWriter.emit with screen + track bound
 * by the host. Back / swipe-down dismisses without writing.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GapSheet(send: suspend (String) -> Boolean, onDismiss: () -> Unit) {
    val form = remember { GapForm(send) }
    var text by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val ctx = LocalContext.current
    val focus = remember { FocusRequester() }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 24.dp)) {
            Text(
                text = "~/log-gap",
                style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedTextField(
                value = text,
                onValueChange = { form.edit(it); text = form.text },
                placeholder = { Text("What's missing or annoying?") },
                supportingText = { Text("${text.length}/${GapsWriter.MAX_CHARS}") },
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp).focusRequester(focus),
            )
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(
                    enabled = form.canSend && !sending,
                    onClick = {
                        sending = true
                        scope.launch {
                            val ok = form.submit()
                            sending = false
                            if (ok) {
                                Toast.makeText(ctx, "Gap logged", Toast.LENGTH_SHORT).show()
                                onDismiss()
                            } else {
                                Toast.makeText(ctx, "Gap not saved — sync folder unavailable", Toast.LENGTH_LONG).show()
                            }
                        }
                    },
                ) { Text("Send") }
            }
        }
    }
    LaunchedEffect(Unit) { focus.requestFocus() }
}
