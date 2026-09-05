plugins {
    id("com.android.application")
}

val gameRoot = rootDir.parentFile
val stagedGameAssetsDir = layout.buildDirectory.dir("generated/game-assets")
val configuredRom = providers.gradleProperty("psrRom").orNull?.let(::file)

android {
    namespace = "com.pokemonstadiumrecomp.probe"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.pokemonstadium2recomp"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    sourceSets["main"].assets.srcDir(stagedGameAssetsDir)

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    ndkVersion = "27.2.12479018"

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

val stageGameAssets = tasks.register("stageGameAssets") {
    val outputDir = stagedGameAssetsDir.get().asFile
    outputs.dir(outputDir)

    doLast {
        delete(outputDir)
        copy {
            from(gameRoot.resolve("assets"))
            into(outputDir.resolve("assets"))
        }
        if (configuredRom == null || !configuredRom.exists()) {
            throw GradleException(
                "Android build requires an external ROM: " +
                    "./gradlew assembleDebug -PpsrRom=C:/path/to/pokemon-stadium-2.z64"
            )
        }
        copy { from(configuredRom); into(outputDir) }
    }
}

tasks.named("preBuild") {
    dependsOn(stageGameAssets)
}
