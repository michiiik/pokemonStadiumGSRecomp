package com.pokemonstadiumrecomp.probe;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.view.WindowManager;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("main");
    }

    private static native void nativeSetAndroidSurfaceReady(boolean ready);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        setContentView(surface);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSetAndroidSurfaceReady(true);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetAndroidSurfaceReady(false);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // The native renderer reads the current surface from the Activity.
    }
}
