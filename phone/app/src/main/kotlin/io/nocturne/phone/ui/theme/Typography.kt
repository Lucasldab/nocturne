package io.nocturne.phone.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

private val Mono = FontFamily.Monospace
private val Sans = FontFamily.SansSerif

internal val NocturneTypography = Typography(
    headlineLarge  = TextStyle(fontFamily = Sans, fontWeight = FontWeight.Bold,   fontSize = 24.sp),
    headlineMedium = TextStyle(fontFamily = Sans, fontWeight = FontWeight.Bold,   fontSize = 20.sp),
    titleLarge     = TextStyle(fontFamily = Sans, fontWeight = FontWeight.Medium, fontSize = 18.sp),
    titleMedium    = TextStyle(fontFamily = Sans, fontWeight = FontWeight.Medium, fontSize = 16.sp),
    bodyLarge      = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Normal, fontSize = 15.sp),
    bodyMedium     = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Normal, fontSize = 14.sp),
    bodySmall      = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Normal, fontSize = 12.sp),
    labelLarge     = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Medium, fontSize = 13.sp),
    labelMedium    = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Medium, fontSize = 12.sp),
    labelSmall     = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Medium, fontSize = 11.sp),
)
