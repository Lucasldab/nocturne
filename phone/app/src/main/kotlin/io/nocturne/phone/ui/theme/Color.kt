package io.nocturne.phone.ui.theme

import androidx.compose.ui.graphics.Color

// Fixed terminal-feel palette. NO Material You / dynamic color (CAT-06).
// Tightened 2026-04-28: backgrounds darker per user request, primaries
// dropped one stop deeper, secondaryContainer carries alpha so the selected
// nav-pill reads as a tint rather than a solid block.
internal val NocturneBackground = Color(0xFF050505)
internal val NocturneSurface    = Color(0xFF0E0E0E)
internal val NocturneSurfaceVariant = Color(0xFF161616)
internal val NocturneOnBackground = Color(0xFFE0E0E0)
internal val NocturneOnSurface  = Color(0xFFC8C8C8)
internal val NocturneOnSurfaceMuted = Color(0xFF9E9689)
internal val NocturnePrimary    = Color(0xFF7E3AA0)  // deep purple accent — slightly darker than #8C41B4
internal val NocturneOnPrimary  = Color(0xFF000000)
// Selected nav-pill: alpha-blended deeper purple (#5B2378 ~ design `primaryDeep`).
// 0x80 alpha keeps the pill visible without dominating the icon glyph.
internal val NocturneSecondaryContainer = Color(0x805B2378)
internal val NocturneError      = Color(0xFFCF6679)

// Visual conventions for resident vs catalog distinction (CAT-05).
const val NON_RESIDENT_ALPHA: Float = 0.5f
