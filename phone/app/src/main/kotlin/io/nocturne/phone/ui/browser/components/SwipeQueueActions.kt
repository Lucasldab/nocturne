package io.nocturne.phone.ui.browser.components

import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp

@Composable
fun SwipeQueueActions(
    onPlayNext: () -> Unit,
    onAddToQueue: () -> Unit,
    enabled: Boolean = true,
    content: @Composable () -> Unit,
) {
    val density = LocalDensity.current
    val swipeThresholdPx = with(density) { 80.dp.toPx() }
    var dragOffsetPx by remember { mutableFloatStateOf(0f) }
    var menuExpanded by remember { mutableStateOf(false) }
    val animatedOffset by animateDpAsState(
        targetValue = with(density) { dragOffsetPx.toDp() },
        animationSpec = tween(durationMillis = 120),
        label = "swipeQueueActionsOffset",
    )

    Box(modifier = Modifier.fillMaxWidth()) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .offset { IntOffset(with(density) { animatedOffset.roundToPx() }, 0) }
                .background(MaterialTheme.colorScheme.background)
                .pointerInput(enabled) {
                    if (!enabled) return@pointerInput
                    detectHorizontalDragGestures(
                        onDragEnd = {
                            if (dragOffsetPx >= swipeThresholdPx) menuExpanded = true
                            dragOffsetPx = 0f
                        },
                        onDragCancel = { dragOffsetPx = 0f },
                        onHorizontalDrag = { change, dragAmount ->
                            dragOffsetPx = (dragOffsetPx + dragAmount).coerceAtLeast(0f)
                            if (dragAmount > 0f) change.consume()
                        },
                    )
                },
        ) {
            content()
        }
        DropdownMenu(
            expanded = menuExpanded,
            onDismissRequest = { menuExpanded = false },
        ) {
            DropdownMenuItem(
                text = { Text("Play next") },
                onClick = {
                    menuExpanded = false
                    onPlayNext()
                },
            )
            DropdownMenuItem(
                text = { Text("Add to queue") },
                onClick = {
                    menuExpanded = false
                    onAddToQueue()
                },
            )
        }
    }
}
