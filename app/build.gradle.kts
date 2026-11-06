plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.hsrprobe"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    // 版本号跟 CI 的 run number 对齐, 和产物名 HSRModProbe-probe<N>.apk 一致
    val buildNo = (project.findProperty("buildNo") as String?)?.toIntOrNull() ?: 1

    defaultConfig {
        applicationId = "com.hsrprobe"
        minSdk = 24
        targetSdk = 34
        versionCode = buildNo
        versionName = "0.1.$buildNo"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17")
                arguments += listOf(
                    "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
                )
            }
        }
    }

    signingConfigs {
        create("ci") {
            storeFile = file(System.getenv("KS_PATH") ?: "ci.jks")
            storePassword = System.getenv("KS_PASS") ?: "android"
            keyAlias = System.getenv("KS_ALIAS") ?: "ci"
            keyPassword = System.getenv("KS_PASS") ?: "android"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("ci")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // 内置 api-82.jar, 不依赖 api.xposed.info (那站经常挂)
    compileOnly(files("libs/api-82.jar"))
}
