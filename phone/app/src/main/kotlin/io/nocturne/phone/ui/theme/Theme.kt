package io.nocturne.phone.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val NocturneColorScheme = darkColorScheme(
    background       = NocturneBackground,
    surface          = NocturneSurface,
    surfaceVariant   = NocturneSurfaceVariant,
    onBackground     = NocturneOnBackground,
    onSurface        = NocturneOnSurface,
    onSurfaceVariant = NocturneOnSurfaceMuted,
    primary          = NocturnePrimary,
    onPrimary        = NocturneOnPrimary,
    error            = NocturneError,
)

@Composable
fun NocturneTheme(content: @Composable () -> Unit) {
    // FORBIDDEN by CAT-06: dynamicDarkColorScheme(...). Must NOT branch
    // on Build.VERSION_CODES.S to opt into Material You.
    MaterialTheme(
        colorScheme = NocturneColorScheme,
        typography  = NocturneTypography,
        content     = content,
    )
}
