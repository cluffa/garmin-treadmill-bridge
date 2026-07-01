package com.cluffa.garmin_ftms_app

import android.app.Application

class App : Application() {
    override fun onCreate() {
        // Fix for Garmin CIQ SDK crashing on Android 12+ with ClassNotFoundException
        // when deserializing IQDevice parcelables in broadcast receivers.
        Thread.currentThread().contextClassLoader = classLoader
        super.onCreate()
    }
}
