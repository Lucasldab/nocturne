package io.nocturne.phone

import android.app.Application
import io.nocturne.phone.data.AppContainer

class NocturneApp : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
    }
}
