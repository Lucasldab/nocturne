package io.nocturne.phone.ui.theme

import androidx.compose.ui.graphics.Color

// Fixed terminal-feel palette. NO Material You / dynamic color (CAT-06).
internal val NocturneBackground = Color(0xFF0A0A0A)
internal val NocturneSurface    = Color(0xFF121212)
internal val NocturneSurfaceVariant = Color(0xFF1A1A1A)
internal val NocturneOnBackground = Color(0xFFE0E0E0)
internal val NocturneOnSurface  = Color(0xFFC8C8C8)
internal val NocturneOnSurfaceMuted = Color(0xFF8A8A8A)
internal val NocturnePrimary    = Color(0xFF7DA3A1)  // muted teal accent — terminal-friendly
internal val NocturneOnPrimary  = Color(0xFF000000)
internal val NocturneError      = Color(0xFFCF6679)

// Visual conventions for resident vs catalog distinction (CAT-05).
const val NON_RESIDENT_ALPHA: Float = 0.5f
