package io.nocturne.phone

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.core.view.WindowCompat
import io.nocturne.phone.ui.AppRoot
import io.nocturne.phone.ui.theme.NocturneTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val app = application as NocturneApp
        setContent { NocturneTheme { AppRoot(app) } }
    }
}
