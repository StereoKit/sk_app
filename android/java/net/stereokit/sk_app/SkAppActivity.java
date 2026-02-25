package net.stereokit.sk_app;

import android.app.Activity;
import android.app.NativeActivity;

/**
 * Minimal NativeActivity subclass for sk_app standalone apps.
 * Activity results (file dialogs, etc.) are handled by SkAppResultFragment.
 */
public class SkAppActivity extends NativeActivity {

	private static native void nativeUiCallback(long callbackPtr);

	/**
	 * Post a native callback to run on the UI thread. Safe to call from any
	 * thread — the callback fires on the Android main/UI thread.
	 *
	 * @param activity    The host Activity (any subclass)
	 * @param callbackPtr Opaque pointer to a native {function, data} pair
	 */
	public static void skaRunOnUiThread(final Activity activity, final long callbackPtr) {
		activity.runOnUiThread(new Runnable() {
			@Override public void run() {
				nativeUiCallback(callbackPtr);
			}
		});
	}
}
