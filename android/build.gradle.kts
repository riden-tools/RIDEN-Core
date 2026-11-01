plugins { id("com.android.library") version "8.9.2" }
android {
    namespace = "io.riden.core"
    compileSdk = 35
    ndkVersion = "27.0.12077973"
    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
        externalNativeBuild { cmake { arguments += "-DBUILD_TESTING=OFF" } }
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86") }
    }
    externalNativeBuild { cmake { path = file("../CMakeLists.txt"); version = "3.22.1" } }
    buildFeatures { aidl = true; prefabPublishing = true }
    prefab { create("riden_core") { headers = "../include" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
}
dependencies { implementation("com.github.mik3y:usb-serial-for-android:3.9.0") }
