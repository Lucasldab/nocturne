// Root build for the nocturne phone app.
// Plugins are declared via the version catalog (gradle/libs.versions.toml)
// and applied in module-level builds (app/build.gradle.kts).
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.compose) apply false
    alias(libs.plugins.kotlin.serialization) apply false
    alias(libs.plugins.ksp) apply false
}
