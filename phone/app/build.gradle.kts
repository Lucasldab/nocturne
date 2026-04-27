import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
}

android {
    namespace = "io.nocturne.phone"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.nocturne.phone"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0-dev"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        // Reproducibility: pin the resource configurations we ship.
        resourceConfigurations += listOf("en")
    }

    sourceSets["androidTest"].assets.srcDir("$projectDir/schemas")

    testOptions {
        unitTests.isIncludeAndroidResources = true
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    // Reproducible-build scaffold (CROSS-02). Verification gate is Phase 8;
    // scaffold lives here from day one.
    androidResources {
        // Intentionally no compression toggles — keep defaults deterministic.
    }
}

// Archive name pinning so APK filenames are deterministic across machines.
// Gradle 9.x: configure via the `base` extension's archivesName property.
base {
    archivesName.set("nocturne-phone")
}

// Reproducibility: pin the Kotlin JVM bytecode target to 11 to match
// compileOptions.{source,target}Compatibility above. We avoid jvmToolchain(11)
// here because the dev machine ships only JDK 21 (Gradle compiles with JDK 21
// but emits JVM 11-compatible bytecode).
kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_11)
    }
}

// Room/KSP schema export — schemas/ is committed for migration diff visibility.
ksp {
    arg("room.schemaLocation", "$projectDir/schemas")
    arg("room.incremental", "true")
    arg("room.generateKotlin", "true")
}

configurations.all {
    resolutionStrategy {
        // Room 2.8.0 ships `$$serializer` classes generated against
        // kotlinx-serialization-core 1.8.x. The Kotlin 2.1.20 serialization
        // compiler plugin pulls 1.7.x by default, which causes
        // AbstractMethodError on `typeParametersSerializers()` during KSP's
        // schema-bundle round-trip. Force the entire classpath to 1.8.1.
        force("org.jetbrains.kotlinx:kotlinx-serialization-core:1.8.1")
        force("org.jetbrains.kotlinx:kotlinx-serialization-core-jvm:1.8.1")
        force("org.jetbrains.kotlinx:kotlinx-serialization-json:1.8.1")
        force("org.jetbrains.kotlinx:kotlinx-serialization-json-jvm:1.8.1")
    }
}

dependencies {
    val composeBom = platform(libs.compose.bom)
    implementation(composeBom)
    androidTestImplementation(composeBom)

    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons.core)
    implementation(libs.compose.ui.tooling.preview)
    debugImplementation(libs.compose.ui.tooling)

    implementation(libs.activity.compose)
    implementation(libs.lifecycle.runtime.compose)
    implementation(libs.lifecycle.viewmodel.compose)
    implementation(libs.navigation.compose)
    implementation(libs.kotlinx.collections.immutable)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.core)
    implementation(libs.datastore.preferences)
    implementation(libs.documentfile)

    implementation(libs.room.runtime)
    implementation(libs.room.ktx)
    implementation(libs.room.paging)
    ksp(libs.room.compiler)

    implementation(libs.paging.runtime)
    implementation(libs.paging.compose)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.room.testing)
    testImplementation(libs.robolectric)
    testImplementation(libs.androidx.junit)
    testImplementation(libs.androidx.test.core)
    testImplementation(libs.paging.testing)
}
