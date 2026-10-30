package org.synth1.android;

final class Native {
    static { System.loadLibrary("synth1"); }
    static native void nInit(String filesDir);
    static native String nStartAudio();
    static native void nStopAudio();
    static native String nStatus();
    static native void nSetSkin(String name);
    static native void nSurface(android.view.Surface s, int w, int h);
    static native void nSurfaceDestroyed();
    /** action: 0 down, 1 move, 2 up, 3 cancel(all); id = MotionEvent pointer id; timeMs = event time. */
    static native void nTouch(int action, int id, float x, float y, long timeMs);
    static native void nPoll();
    static native int nMidiOpen(android.media.midi.MidiDevice device, int port);
    static native void nMidiCloseAll();
    static native long nMidiMessages();
    static native String nRenderProtocol(String preset, String out, int program);
    private Native() {}
}
