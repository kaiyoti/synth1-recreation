package org.synth1.android;

import android.app.Activity;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/** Thin shell: all DSP is core/ (via JNI), all GUI is panel/ + keyboard/. Java only owns the
 *  surface, raw touch, asset extraction (the skin is DATA), skin choice and USB MIDI device opening
 *  (the MIDI bytes are read natively with AMidi on the audio thread). */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "Synth1";
    private boolean kbShown = true;
    private SurfaceHolder holder;
    private int surfW, surfH;
    private SharedPreferences prefs;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private MidiManager midi;
    private final List<MidiDevice> openDevices = new ArrayList<>();
    private final List<MidiDeviceInfo> pending = new ArrayList<>();

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences("synth1", MODE_PRIVATE);
        File files = getFilesDir();
        try { extractAssets(files); } catch (IOException e) { Log.e(TAG, "asset extract", e); }
        Native.nInit(files.getAbsolutePath());
        kbShown = prefs.getBoolean("kbd_visible", true);   // default: shown
        Native.nSetKeyboardVisible(kbShown);

        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        Native.nSetDensity(dm.xdpi > 50f ? dm.xdpi : dm.densityDpi);   // white keys ~9 mm
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        sv.setOnTouchListener((v, ev) -> { handleTouch(ev); return true; });
        setContentView(sv);   // the whole screen is the synth: no Java bar (status = INFO in the VIEW column)

        // adb-driven on-device render gate: am start ... --es render_slots 001,002
        String slots = getIntent().getStringExtra("render_slots");
        if (slots != null) {
            File out = getExternalFilesDir(null);
            for (String s : slots.split(",")) {
                String r = Native.nRenderProtocol(new File(files, "presets/" + s + ".sy1").getPath(),
                        new File(out, "a2_" + s + ".wav").getPath(), Integer.parseInt(s) - 1);
                Log.i(TAG, "render " + s + " -> " + r);
            }
        }
    }

    // all pointers: down / pointer-down / move / up / pointer-up / cancel -> native multi-pointer API
    private void handleTouch(MotionEvent ev) {
        final int a = ev.getActionMasked();
        final long t = ev.getEventTime();
        switch (a) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = ev.getActionIndex();
                Native.nTouch(0, ev.getPointerId(i), ev.getX(i), ev.getY(i), t);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < ev.getPointerCount(); i++)
                    Native.nTouch(1, ev.getPointerId(i), ev.getX(i), ev.getY(i), t);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = ev.getActionIndex();
                Native.nTouch(2, ev.getPointerId(i), ev.getX(i), ev.getY(i), t);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                Native.nTouch(3, 0, 0f, 0f, t);
                break;
            default: break;
        }
    }

    @Override protected void onResume() {
        super.onResume();
        View d = getWindow().getDecorView();
        d.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        Log.i(TAG, Native.nStartAudio());
        startMidi();
        ui.post(new Runnable() { int n = 0; public void run() {
            Native.nPoll();
            boolean kv = Native.nKeyboardVisible();   // KBD toggle in the side pane -> prefs
            if (kv != kbShown) { kbShown = kv; prefs.edit().putBoolean("kbd_visible", kv).apply(); }
            n++;
            ui.postDelayed(this, 33); } });
    }

    @Override protected void onPause() {
        ui.removeCallbacksAndMessages(null);
        stopMidi();
        Native.nStopAudio();
        super.onPause();
    }

    // ---- USB MIDI (android.media.midi); bytes are read natively via AMidi (API 29+) ----
    private final MidiManager.DeviceCallback midiCb = new MidiManager.DeviceCallback() {
        @Override public void onDeviceAdded(MidiDeviceInfo info) { openMidi(info); }
        @Override public void onDeviceRemoved(MidiDeviceInfo info) { reopenMidi(); }
    };

    private void startMidi() {
        if (Build.VERSION.SDK_INT < 29) { Log.i(TAG, "MIDI input needs API 29 (AMidi)"); return; }
        if (!getPackageManager().hasSystemFeature("android.software.midi")) return;
        midi = (MidiManager) getSystemService(MIDI_SERVICE);
        if (midi == null) return;
        midi.registerDeviceCallback(midiCb, ui);
        for (MidiDeviceInfo info : midi.getDevices()) openMidi(info);
    }

    private void openMidi(MidiDeviceInfo info) {
        if (midi == null || info.getOutputPortCount() < 1) return;   // device must send MIDI to us
        midi.openDevice(info, dev -> {
            if (dev == null) { Log.w(TAG, "midi open failed: " + info); return; }
            boolean any = false;
            for (int p = 0; p < info.getOutputPortCount(); p++) any |= Native.nMidiOpen(dev, p) > 0;
            if (any) openDevices.add(dev); else try { dev.close(); } catch (IOException ignored) {}
        }, ui);
    }

    private void reopenMidi() {      // a device went away: reopen the rest cleanly
        if (midi == null) return;
        closeDevices();
        for (MidiDeviceInfo info : midi.getDevices()) openMidi(info);
    }

    private void closeDevices() {
        Native.nMidiCloseAll();
        for (MidiDevice d : openDevices) try { d.close(); } catch (IOException ignored) {}
        openDevices.clear();
    }

    private void stopMidi() {
        if (midi != null) { midi.unregisterDeviceCallback(midiCb); closeDevices(); midi = null; }
    }

    @Override public void surfaceCreated(SurfaceHolder h) { holder = h; }
    @Override public void surfaceChanged(SurfaceHolder h, int fmt, int w, int hh) {
        holder = h; surfW = w; surfH = hh;
        Native.nSurface(h.getSurface(), w, hh);
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { holder = null; Native.nSurfaceDestroyed(); }

    // ---- assets (skin/, presets/) -> filesDir, keyed by install time ----
    private void extractAssets(File files) throws IOException {
        long stamp;
        try { stamp = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime; }
        catch (Exception e) { stamp = 0; }
        File marker = new File(files, "assets.stamp");
        if (marker.exists() && new String(readAll(marker)).equals(String.valueOf(stamp))) return;
        AssetManager am = getAssets();
        for (String top : new String[] {"skin", "presets"}) copyTree(am, top, new File(files, top));
        try (FileOutputStream o = new FileOutputStream(marker)) { o.write(String.valueOf(stamp).getBytes()); }
    }
    private static byte[] readAll(File f) throws IOException {
        try (InputStream in = new java.io.FileInputStream(f)) {
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096]; int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return bo.toByteArray();
        }
    }
    private static void copyTree(AssetManager am, String path, File dst) throws IOException {
        String[] kids = am.list(path);
        if (kids != null && kids.length > 0) {
            dst.mkdirs();
            for (String k : kids) copyTree(am, path + "/" + k, new File(dst, k));
        } else {
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[65536]; int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
        }
    }
}
