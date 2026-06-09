plugins {
    id("com.android.application") version "8.2.0"
}

android {
    namespace = "com.stt.mobile"
    compileSdk = 34
    
    defaultConfig {
        applicationId = "com.stt.mobile"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
        
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }
    
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
    
    externalNativeBuild {
        cmake {
            path = file("native/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation(files("../../third_party/sherpa-onnx-1.12.39.aar"))
}