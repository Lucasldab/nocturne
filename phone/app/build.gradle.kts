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
    // Phase 5: Media3 1.10.0 requires compileSdk 36. AGP 8.13.x max is 36.
    compileSdk = 36

    defaultConfig {
        applicationId = "io.nocturne.phone"
        minSdk = 26
        targetSdk = 35
        versionCode = 42
        versionName = "0.4.35-dev"
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

    lint {
        lintConfig = file("$rootDir/lint.xml")
        abortOnError = true
        checkReleaseBuilds = true
        warningsAsErrors = false
    }
}

// === CROSS-01 enforcement: build-failing INTERNET permission audit ===
//
// Reads the merged AndroidManifest (post-AGP merge of source + library manifests)
// and fails the build if android.permission.INTERNET is present. Catches
// transitive permission injection from any future dependency (T-04-07-01).
//
// Uses AGP's typed variant artifact API so the manifest provider is a real
// build input — config-cache friendly, properly carries upstream task deps.
androidComponents {
    onVariants { variant ->
        val variantCap = variant.name.replaceFirstChar { it.uppercase() }
        val verifyTaskName = "verifyNoInternetPermission$variantCap"
        val manifestProvider = variant.artifacts.get(
            com.android.build.api.artifact.SingleArtifact.MERGED_MANIFEST,
        )
        val variantName = variant.name
        val verifyTask = tasks.register(verifyTaskName) {
            group = "verification"
            description = "Fail build if INTERNET permission appears in merged AndroidManifest for $variantName"
            inputs.file(manifestProvider)
            doLast {
                val manifest = manifestProvider.get().asFile
                if (!manifest.exists()) {
                    throw GradleException("$verifyTaskName: merged manifest not found at ${manifest.absolutePath}")
                }
                val raw = manifest.readText()
                // Strip XML comments before scanning — the manifest deliberately
                // documents CROSS-01 in a `<!-- INTERNET ABSENT -->` comment that
                // would false-positive a naive regex.
                val text = raw.replace(Regex("<!--[\\s\\S]*?-->"), "")
                if (Regex("android\\.permission\\.INTERNET").containsMatchIn(text)) {
                    throw GradleException(
                        "CROSS-01 VIOLATION: android.permission.INTERNET found in ${manifest.absolutePath}",
                    )
                }
                logger.lifecycle("CROSS-01 OK: no INTERNET permission in $variantName merged manifest")
            }
        }
        tasks.matching { it.name == "assemble$variantCap" }.configureEach {
            dependsOn(verifyTask)
        }
    }
}

// Archive name pinning so APK filenames are deterministic across machines.
// Gradle 9.x: configure via the `base` extension's archivesName property.
base {
    archivesName.set("nocturne-phone")
}

// === Phase 5: Room schema copy for Robolectric MigrationTestHelper ===
//
// MigrationTestHelper in Robolectric unit tests loads schema files from the
// android_merged_assets path recorded in test_config.properties, which points
// to the debug build's merged-assets directory. Copy KSP-generated schemas/
// into that directory so Robolectric finds them without bundling schema JSON
// in the release APK. Task is configuration-cache-safe: uses layout APIs.
tasks.register<Copy>("copyRoomSchemasToDebugAssets") {
    description = "Copy Room schemas into debug merged-assets for Robolectric MigrationTestHelper"
    group = "verification"
    dependsOn("mergeDebugAssets")
    from(layout.projectDirectory.dir("schemas"))
    into(layout.buildDirectory.dir("intermediates/assets/debug/mergeDebugAssets"))
}
tasks.matching {
    it.name in setOf(
        "testDebugUnitTest",
        "packageDebugUnitTestForUnitTest",
        "compressDebugAssets",
        "generateDebugUnitTestConfig",
    )
}.configureEach {
    dependsOn("copyRoomSchemasToDebugAssets")
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
    implementation(libs.compose.material.icons.extended)
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

    // Phase 5: Media3 playback engine + session + Compose UI
    implementation(libs.media3.exoplayer)
    implementation(libs.media3.session)
    implementation(libs.media3.ui.compose)
    implementation(libs.media3.ui.compose.m3)

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
