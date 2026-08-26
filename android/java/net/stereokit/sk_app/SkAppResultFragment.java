package net.stereokit.sk_app;

import android.app.Activity;
import android.app.Fragment;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

/**
 * Headless fragment that launches an Intent via startActivityForResult, or a
 * runtime permission request, and forwards the result to native code. Works
 * with any Activity, no SkAppActivity subclass required.
 *
 * Used by sk_app for file dialogs and permissions, and extensible to any
 * feature that needs an activity result (camera, contacts, share, etc.).
 */
@SuppressWarnings("deprecation")
public class SkAppResultFragment extends Fragment {
	private static final String TAG = "SkAppResultFragment";
	private static final String FRAGMENT_TAG = "sk_app_result";
	private static final String ARG_REQUEST_ID = "request_id";
	private static final String ARG_PERMISSIONS = "permissions";

	private static native void nativeOnActivityResult(int requestId, int resultCode, String[] uris);
	private static native void nativeOnPermissionResult(String[] permissions, int[] grantResults, boolean[] noRationale);

	/**
	 * Launch an Intent via a headless fragment that receives the result.
	 * Safe to call from any thread — posts to UI thread if needed.
	 *
	 * @param activity  The host Activity (any subclass)
	 * @param requestId Opaque ID forwarded to nativeOnActivityResult
	 * @param intent    The Intent to launch
	 */
	public static void launch(final Activity activity, final int requestId, final Intent intent) {
		activity.runOnUiThread(new Runnable() {
			@Override public void run() {
				SkAppResultFragment fragment = new SkAppResultFragment();
				Bundle args = new Bundle();
				args.putInt(ARG_REQUEST_ID, requestId);
				args.putParcelable("intent", intent);
				fragment.setArguments(args);

				activity.getFragmentManager()
					.beginTransaction()
					.add(fragment, FRAGMENT_TAG + "_" + requestId)
					.commitAllowingStateLoss();
			}
		});
	}

	/**
	 * Request runtime permissions via a headless fragment that receives
	 * onRequestPermissionsResult. Safe to call from any thread.
	 *
	 * @param activity    The host Activity (any subclass)
	 * @param requestId   Opaque ID for the request
	 * @param permissions Android permission strings to request
	 */
	public static void requestPermissions(final Activity activity, final int requestId, final String[] permissions) {
		activity.runOnUiThread(new Runnable() {
			@Override public void run() {
				SkAppResultFragment fragment = new SkAppResultFragment();
				Bundle args = new Bundle();
				args.putInt(ARG_REQUEST_ID, requestId);
				args.putStringArray(ARG_PERMISSIONS, permissions);
				fragment.setArguments(args);

				activity.getFragmentManager()
					.beginTransaction()
					.add(fragment, FRAGMENT_TAG + "_permission_" + requestId)
					.commitAllowingStateLoss();
			}
		});
	}

	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);

		if (savedInstanceState != null) {
			// Recreated after config change. The launched activity or
			// permission dialog is still up, result arrives in its callback.
			return;
		}

		Bundle args = getArguments();
		if (args == null) {
			remove();
			return;
		}

		String[] permissions = args.getStringArray(ARG_PERMISSIONS);
		Intent   intent      = args.getParcelable("intent");
		int      requestId   = args.getInt(ARG_REQUEST_ID);

		if (permissions != null) {
			requestPermissions(permissions, requestId);
		} else if (intent != null) {
			startActivityForResult(intent, requestId);
		} else {
			nativeOnActivityResult(requestId, Activity.RESULT_CANCELED, null);
			remove();
		}
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
		// Rationale is only meaningful right now: false after a denial means
		// the system will not prompt for this permission again.
		boolean[] noRationale = new boolean[permissions.length];
		for (int i = 0; i < permissions.length; i++)
			noRationale[i] = !shouldShowRequestPermissionRationale(permissions[i]);

		nativeOnPermissionResult(permissions, grantResults, noRationale);
		remove();
	}

	@Override
	public void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);

		String[] uris = (data != null) ? extractUris(data) : null;

		if (uris != null && data != null) {
			takePermissions(uris, data);
		}

		nativeOnActivityResult(requestCode, resultCode, uris);
		remove();
	}

	private String[] extractUris(Intent data) {
		ClipData clipData = data.getClipData();
		if (clipData != null && clipData.getItemCount() > 0) {
			String[] uris = new String[clipData.getItemCount()];
			for (int i = 0; i < clipData.getItemCount(); i++) {
				Uri uri = clipData.getItemAt(i).getUri();
				if (uri != null) uris[i] = uri.toString();
			}
			return uris;
		}

		Uri uri = data.getData();
		if (uri != null) {
			return new String[] { uri.toString() };
		}

		return null;
	}

	private void takePermissions(String[] uris, Intent data) {
		int flags = data.getFlags() &
			(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
		if (flags == 0) return;

		for (String uriStr : uris) {
			if (uriStr == null) continue;
			try {
				getActivity().getContentResolver()
					.takePersistableUriPermission(Uri.parse(uriStr), flags);
			} catch (SecurityException e) {
				Log.d(TAG, "Could not take permission for " + uriStr);
			}
		}
	}

	private void remove() {
		if (getFragmentManager() != null) {
			getFragmentManager()
				.beginTransaction()
				.remove(this)
				.commitAllowingStateLoss();
		}
	}
}
