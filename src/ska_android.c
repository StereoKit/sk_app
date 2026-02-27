//
// sk_app - Android platform backend

#include "ska_internal.h"

#ifdef SKA_PLATFORM_ANDROID

#include <android_native_app_glue.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>

// Scancode translation table (Android key codes to ska_scancode_)
static ska_scancode_ ska_android_scancode_table[256];

// Previous Android button state for diffing BUTTON_PRESS/RELEASE events.
// getButtonState() returns cumulative state, so we diff against the previous
// value to determine which specific button changed.
static int32_t g_android_prev_button_state = 0;

// ============================================================================
// Pre-init state — survives the memset in ska_init()
// ============================================================================
// Set by ska_android_set_app() / ska_android_set_context() before ska_init().
// ska_platform_init() reads from here and promotes raw pointers to JNI global
// refs once the VM is available.

static struct {
	struct android_app* android_app;
	void*               android_context; // raw jobject, NOT a global ref
	void*               native_window;   // ANativeWindow* delivered before window stub exists
} g_android_early = {0};

SKA_API void ska_android_set_app(void* app) {
	g_android_early.android_app = (struct android_app*)app;
}

// ============================================================================
// Cached JNI references
// ============================================================================
// Method IDs and field IDs are stable once looked up and can be cached.
// Classes must be converted to global references to survive across JNI calls.

static struct {
	// Activity -> Window (Activity-only)
	jmethodID   activity_getWindow;
	jmethodID   activity_getWindowManager;
	// Window methods (Activity-only)
	jmethodID   window_getAttributes;
	jmethodID   window_setAttributes;
	jmethodID   window_getDecorView;
	// View methods (Activity-only)
	jmethodID   view_getWidth;
	jmethodID   view_getHeight;
	jmethodID   view_getWindowToken;
	// LayoutParams fields (Activity-only)
	jfieldID    lp_x;
	jfieldID    lp_y;
	jfieldID    lp_width;
	jfieldID    lp_height;
	// Display (Activity-only, looked up via getWindowManager)
	jclass      display_class;
	jmethodID   wm_getDefaultDisplay;
	jmethodID   display_getRefreshRate;
	// Context methods (work with any Context)
	jmethodID   ctx_getContentResolver;
	jmethodID   ctx_getSystemService;
	// Content URI helpers
	jclass      uri_class;
	jmethodID   uri_parse;
	// Clipboard (Context-based — works from Services too)
	jclass      clipboard_class;
	jmethodID   clipboard_hasPrimaryClip;
	jmethodID   clipboard_getPrimaryClip;
	jmethodID   clipboard_setPrimaryClip;
	jclass      clip_data_class;
	jmethodID   clip_data_getItemAt;
	jmethodID   clip_data_newPlainText;
	jclass      clip_item_class;
	jmethodID   clip_item_getText;
	jclass      charseq_class;
	jmethodID   charseq_toString;
	// InputMethodManager (Activity-only — needs window for focus)
	jclass      imm_class;
	jmethodID   imm_showSoftInput;
	jmethodID   imm_hideSoftInputFromWindow;
	// UI thread trampoline (SkAppActivity.skaRunOnUiThread)
	jclass      ui_helper_class;
	jmethodID   ui_helper_run;
} g_jni_cache = {0};

// Cached JNI references for file dialog
static struct {
	jclass    intent_class;
	jmethodID intent_init;
	jmethodID intent_setType;
	jmethodID intent_addCategory;
	jmethodID intent_putExtra_bool;
	jclass    pm_class;
	jmethodID pm_queryIntentActivities;
	jmethodID activity_getPackageManager;
	jclass    fragment_class;
	jmethodID fragment_launch;
	bool      fragment_available;
	bool      initialized;
} g_file_dialog_jni = {0};

// Cached JNI references for KVP store (SharedPreferences + Base64)
static struct {
	jmethodID ctx_getSharedPreferences;
	jmethodID prefs_getString;
	jmethodID prefs_edit;
	jmethodID editor_putString;
	jmethodID editor_remove;
	jmethodID editor_apply;
	jclass    base64_class;
	jmethodID base64_encodeToString;
	jmethodID base64_decode;
	bool      initialized;
} g_kvp_jni = {0};

// ============================================================================
// JNI Helpers
// ============================================================================

// Android always has exactly one JVM. AttachCurrentThread is a no-op if the
// calling thread is already attached (which it will be from Java/C# threads).
SKA_API void* ska_android_get_jni_env(void) {
	JavaVM *vm = (JavaVM*)g_ska.java_vm;
	if (!vm) return NULL;

	JNIEnv *env = NULL;
	(*vm)->AttachCurrentThread(vm, &env, NULL);
	return env;
}

// FindClass uses the system class loader on native threads, which can't find
// app classes. This helper uses the Activity's class loader instead. Returns
// a local ref on success, NULL on failure (exception is cleared).
static jclass ska_android_find_app_class(JNIEnv *env, const char *dotted_name) {
	if (!g_ska.android_context) return NULL;

	jclass    activity_class = (*env)->GetObjectClass(env, (jobject)g_ska.android_context);
	jmethodID getClassLoader = (*env)->GetMethodID(env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
	jobject   class_loader   = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context, getClassLoader);
	(*env)->DeleteLocalRef(env, activity_class);
	if (!class_loader) return NULL;

	jclass    loader_class = (*env)->GetObjectClass(env, class_loader);
	jmethodID loadClass    = (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
	(*env)->DeleteLocalRef(env, loader_class);

	jstring class_name = (*env)->NewStringUTF(env, dotted_name);
	jclass  result     = (jclass)(*env)->CallObjectMethod(env, class_loader, loadClass, class_name);
	(*env)->DeleteLocalRef(env, class_name);
	(*env)->DeleteLocalRef(env, class_loader);

	if (!result || (*env)->ExceptionCheck(env)) {
		if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
		if (result) (*env)->DeleteLocalRef(env, result);
		return NULL;
	}
	return result;
}

// ============================================================================
// UI-thread trampoline
// ============================================================================
// Many Android APIs (Window.setAttributes, InputMethodManager, etc.) must be
// called from the main/UI thread. The native app thread posts work here via
// SkAppActivity.skaRunOnUiThread(), which wraps the call in
// Activity.runOnUiThread() and invokes nativeUiCallback on the UI thread.

typedef struct {
	void (*fn)(void*);
	void *data;
} ska_ui_callback_t;

JNIEXPORT void JNICALL Java_net_stereokit_sk_app_SkAppActivity_nativeUiCallback(
	JNIEnv* env, jclass cls, jlong callback_ptr)
{
	(void)env; (void)cls;
	ska_ui_callback_t *cb = (ska_ui_callback_t*)(intptr_t)callback_ptr;
	if (cb) {
		cb->fn(cb->data);
		ska_free(cb);
	}
}

static void ska_android_run_on_ui_thread(void (*fn)(void*), void *data) {
	JNIEnv *env = (JNIEnv*)ska_android_get_jni_env();
	// skaRunOnUiThread expects an Activity — skip for Service contexts
	if (!env || !g_jni_cache.ui_helper_class || !g_ska.android_context || !g_ska.android_is_activity) return;

	ska_ui_callback_t *cb = (ska_ui_callback_t*)ska_malloc(sizeof(ska_ui_callback_t));
	if (!cb) return;
	cb->fn   = fn;
	cb->data = data;

	(*env)->CallStaticVoidMethod(env,
		g_jni_cache.ui_helper_class,
		g_jni_cache.ui_helper_run,
		(jobject)g_ska.android_context,
		(jlong)(intptr_t)cb);
}

SKA_API void ska_android_set_context(void *context) {
	JNIEnv *env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) {
		// Pre-init: VM not yet discovered, stash raw pointer for
		// ska_platform_init() to promote to a global ref later.
		g_android_early.android_context = context;
		return;
	}

	// Release previous global ref if set
	if (g_ska.android_context) {
		(*env)->DeleteGlobalRef(env, (jobject)g_ska.android_context);
		g_ska.android_context = NULL;
	}

	if (context) {
		g_ska.android_context = (*env)->NewGlobalRef(env, (jobject)context);
	}
}

static void ska_jni_cache_init(void) {
	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) {
		return;
	}

	if (!g_ska.android_context) return;

	// Detect whether the context is an Activity (has getWindow, UI thread,
	// etc.) or a plain Context/Service (no window, no decor view).
	jclass activity_base = (*env)->FindClass(env, "android/app/Activity");
	g_ska.android_is_activity = activity_base &&
		(*env)->IsInstanceOf(env, (jobject)g_ska.android_context, activity_base);
	if (activity_base) (*env)->DeleteLocalRef(env, activity_base);

	if (g_ska.android_is_activity) {
		// Activity-specific methods (getWindow, getWindowManager, etc.)
		jclass activity_class = (*env)->GetObjectClass(env, (jobject)g_ska.android_context);
		g_jni_cache.activity_getWindow        = (*env)->GetMethodID(env, activity_class, "getWindow",        "()Landroid/view/Window;");
		g_jni_cache.activity_getWindowManager = (*env)->GetMethodID(env, activity_class, "getWindowManager", "()Landroid/view/WindowManager;");
		(*env)->DeleteLocalRef(env, activity_class);

		// Window methods
		jclass window_class = (*env)->FindClass(env, "android/view/Window");
		g_jni_cache.window_getAttributes = (*env)->GetMethodID(env, window_class, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
		g_jni_cache.window_setAttributes = (*env)->GetMethodID(env, window_class, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
		g_jni_cache.window_getDecorView  = (*env)->GetMethodID(env, window_class, "getDecorView",  "()Landroid/view/View;");
		(*env)->DeleteLocalRef(env, window_class);

		// View methods
		jclass view_class = (*env)->FindClass(env, "android/view/View");
		g_jni_cache.view_getWidth       = (*env)->GetMethodID(env, view_class, "getWidth",       "()I");
		g_jni_cache.view_getHeight      = (*env)->GetMethodID(env, view_class, "getHeight",      "()I");
		g_jni_cache.view_getWindowToken = (*env)->GetMethodID(env, view_class, "getWindowToken", "()Landroid/os/IBinder;");
		(*env)->DeleteLocalRef(env, view_class);

		// LayoutParams fields
		jclass lp_class = (*env)->FindClass(env, "android/view/WindowManager$LayoutParams");
		g_jni_cache.lp_x      = (*env)->GetFieldID(env, lp_class, "x",      "I");
		g_jni_cache.lp_y      = (*env)->GetFieldID(env, lp_class, "y",      "I");
		g_jni_cache.lp_width  = (*env)->GetFieldID(env, lp_class, "width",  "I");
		g_jni_cache.lp_height = (*env)->GetFieldID(env, lp_class, "height", "I");
		(*env)->DeleteLocalRef(env, lp_class);

		// Display (for refresh rate via getWindowManager)
		jclass wm_class = (*env)->FindClass(env, "android/view/WindowManager");
		g_jni_cache.wm_getDefaultDisplay = (*env)->GetMethodID(env, wm_class, "getDefaultDisplay", "()Landroid/view/Display;");
		(*env)->DeleteLocalRef(env, wm_class);

		jclass display_class = (*env)->FindClass(env, "android/view/Display");
		g_jni_cache.display_class          = (*env)->NewGlobalRef(env, display_class);
		g_jni_cache.display_getRefreshRate = (*env)->GetMethodID (env, display_class, "getRefreshRate", "()F");
		(*env)->DeleteLocalRef(env, display_class);

		// InputMethodManager (virtual keyboard — needs Activity window)
		jclass imm_class = (*env)->FindClass(env, "android/view/inputmethod/InputMethodManager");
		g_jni_cache.imm_class                    = (*env)->NewGlobalRef(env, imm_class);
		g_jni_cache.imm_showSoftInput            = (*env)->GetMethodID (env, imm_class, "showSoftInput",           "(Landroid/view/View;I)Z");
		g_jni_cache.imm_hideSoftInputFromWindow  = (*env)->GetMethodID (env, imm_class, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z");
		(*env)->DeleteLocalRef(env, imm_class);
	} else {
		ska_log(ska_log_info, "Context is not an Activity — window management unavailable");
	}

	// Context methods — work with any Context (Activity, Service, etc.)
	jclass ctx_class = (*env)->FindClass(env, "android/content/Context");
	g_jni_cache.ctx_getContentResolver = (*env)->GetMethodID(env, ctx_class, "getContentResolver", "()Landroid/content/ContentResolver;");
	g_jni_cache.ctx_getSystemService   = (*env)->GetMethodID(env, ctx_class, "getSystemService",   "(Ljava/lang/String;)Ljava/lang/Object;");
	(*env)->DeleteLocalRef(env, ctx_class);

	// Content URI helpers
	jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
	g_jni_cache.uri_class = (*env)->NewGlobalRef     (env, uri_class);
	g_jni_cache.uri_parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
	(*env)->DeleteLocalRef(env, uri_class);

	// Clipboard — Context-based, works from Services too
	jclass cm_class = (*env)->FindClass(env, "android/content/ClipboardManager");
	g_jni_cache.clipboard_class          = (*env)->NewGlobalRef(env, cm_class);
	g_jni_cache.clipboard_hasPrimaryClip = (*env)->GetMethodID (env, cm_class, "hasPrimaryClip", "()Z");
	g_jni_cache.clipboard_getPrimaryClip = (*env)->GetMethodID (env, cm_class, "getPrimaryClip", "()Landroid/content/ClipData;");
	g_jni_cache.clipboard_setPrimaryClip = (*env)->GetMethodID (env, cm_class, "setPrimaryClip", "(Landroid/content/ClipData;)V");
	(*env)->DeleteLocalRef(env, cm_class);

	jclass cd_class = (*env)->FindClass(env, "android/content/ClipData");
	g_jni_cache.clip_data_class        = (*env)->NewGlobalRef     (env, cd_class);
	g_jni_cache.clip_data_getItemAt    = (*env)->GetMethodID      (env, cd_class, "getItemAt",    "(I)Landroid/content/ClipData$Item;");
	g_jni_cache.clip_data_newPlainText = (*env)->GetStaticMethodID(env, cd_class, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
	(*env)->DeleteLocalRef(env, cd_class);

	jclass ci_class = (*env)->FindClass(env, "android/content/ClipData$Item");
	g_jni_cache.clip_item_class   = (*env)->NewGlobalRef(env, ci_class);
	g_jni_cache.clip_item_getText = (*env)->GetMethodID (env, ci_class, "getText", "()Ljava/lang/CharSequence;");
	(*env)->DeleteLocalRef(env, ci_class);

	jclass cs_class = (*env)->FindClass(env, "java/lang/CharSequence");
	g_jni_cache.charseq_class    = (*env)->NewGlobalRef(env, cs_class);
	g_jni_cache.charseq_toString = (*env)->GetMethodID (env, cs_class, "toString", "()Ljava/lang/String;");
	(*env)->DeleteLocalRef(env, cs_class);

	// UI-thread trampoline — SkAppActivity.skaRunOnUiThread()
	jclass ui_class = ska_android_find_app_class(env, "net.stereokit.sk_app.SkAppActivity");
	if (ui_class) {
		g_jni_cache.ui_helper_class = (*env)->NewGlobalRef     (env, ui_class);
		g_jni_cache.ui_helper_run   = (*env)->GetStaticMethodID(env, ui_class,
			"skaRunOnUiThread", "(Landroid/app/Activity;J)V");

		// Register native callback so JVM can find it even when the
		// library was loaded via dlopen rather than System.loadLibrary.
		static const JNINativeMethod methods[] = {{
			"nativeUiCallback", "(J)V",
			(void*)Java_net_stereokit_sk_app_SkAppActivity_nativeUiCallback
		}};
		(*env)->RegisterNatives(env, ui_class, methods, 1);
		(*env)->DeleteLocalRef (env, ui_class);
	} else {
		ska_log(ska_log_warn, "SkAppActivity not found — UI-thread dispatch unavailable");
	}
}

static void ska_jni_cache_shutdown(void) {
	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (env) {
		if (g_jni_cache.display_class)   (*env)->DeleteGlobalRef(env, g_jni_cache.display_class);
		if (g_jni_cache.imm_class)       (*env)->DeleteGlobalRef(env, g_jni_cache.imm_class);
		if (g_jni_cache.uri_class)       (*env)->DeleteGlobalRef(env, g_jni_cache.uri_class);
		if (g_jni_cache.clipboard_class) (*env)->DeleteGlobalRef(env, g_jni_cache.clipboard_class);
		if (g_jni_cache.clip_data_class) (*env)->DeleteGlobalRef(env, g_jni_cache.clip_data_class);
		if (g_jni_cache.clip_item_class) (*env)->DeleteGlobalRef(env, g_jni_cache.clip_item_class);
		if (g_jni_cache.charseq_class)   (*env)->DeleteGlobalRef(env, g_jni_cache.charseq_class);
		if (g_jni_cache.ui_helper_class) (*env)->DeleteGlobalRef(env, g_jni_cache.ui_helper_class);
	}
	memset(&g_jni_cache, 0, sizeof(g_jni_cache));
}

// Helper to get window position via JNI (for freeform/DEX mode)
static void ska_android_get_window_position(ska_window_t* window) {
	if (!window) {
		return;
	}

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) {
		return;
	}

	if (!g_ska.android_context || !g_ska.android_is_activity) return;

	jobject jwindow = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context, g_jni_cache.activity_getWindow);

	if (jwindow) {
		jobject layout_params = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getAttributes);

		if (layout_params) {
			window->x = (*env)->GetIntField(env, layout_params, g_jni_cache.lp_x);
			window->y = (*env)->GetIntField(env, layout_params, g_jni_cache.lp_y);

			(*env)->DeleteLocalRef(env, layout_params);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
}

static void ska_init_scancode_table(void) {
	// Initialize all to unknown
	for (int32_t i = 0; i < 256; i++) {
		ska_android_scancode_table[i] = ska_scancode_unknown;
	}

	// Letters
	ska_android_scancode_table[AKEYCODE_A] = ska_scancode_a;
	ska_android_scancode_table[AKEYCODE_B] = ska_scancode_b;
	ska_android_scancode_table[AKEYCODE_C] = ska_scancode_c;
	ska_android_scancode_table[AKEYCODE_D] = ska_scancode_d;
	ska_android_scancode_table[AKEYCODE_E] = ska_scancode_e;
	ska_android_scancode_table[AKEYCODE_F] = ska_scancode_f;
	ska_android_scancode_table[AKEYCODE_G] = ska_scancode_g;
	ska_android_scancode_table[AKEYCODE_H] = ska_scancode_h;
	ska_android_scancode_table[AKEYCODE_I] = ska_scancode_i;
	ska_android_scancode_table[AKEYCODE_J] = ska_scancode_j;
	ska_android_scancode_table[AKEYCODE_K] = ska_scancode_k;
	ska_android_scancode_table[AKEYCODE_L] = ska_scancode_l;
	ska_android_scancode_table[AKEYCODE_M] = ska_scancode_m;
	ska_android_scancode_table[AKEYCODE_N] = ska_scancode_n;
	ska_android_scancode_table[AKEYCODE_O] = ska_scancode_o;
	ska_android_scancode_table[AKEYCODE_P] = ska_scancode_p;
	ska_android_scancode_table[AKEYCODE_Q] = ska_scancode_q;
	ska_android_scancode_table[AKEYCODE_R] = ska_scancode_r;
	ska_android_scancode_table[AKEYCODE_S] = ska_scancode_s;
	ska_android_scancode_table[AKEYCODE_T] = ska_scancode_t;
	ska_android_scancode_table[AKEYCODE_U] = ska_scancode_u;
	ska_android_scancode_table[AKEYCODE_V] = ska_scancode_v;
	ska_android_scancode_table[AKEYCODE_W] = ska_scancode_w;
	ska_android_scancode_table[AKEYCODE_X] = ska_scancode_x;
	ska_android_scancode_table[AKEYCODE_Y] = ska_scancode_y;
	ska_android_scancode_table[AKEYCODE_Z] = ska_scancode_z;

	// Numbers
	ska_android_scancode_table[AKEYCODE_0] = ska_scancode_0;
	ska_android_scancode_table[AKEYCODE_1] = ska_scancode_1;
	ska_android_scancode_table[AKEYCODE_2] = ska_scancode_2;
	ska_android_scancode_table[AKEYCODE_3] = ska_scancode_3;
	ska_android_scancode_table[AKEYCODE_4] = ska_scancode_4;
	ska_android_scancode_table[AKEYCODE_5] = ska_scancode_5;
	ska_android_scancode_table[AKEYCODE_6] = ska_scancode_6;
	ska_android_scancode_table[AKEYCODE_7] = ska_scancode_7;
	ska_android_scancode_table[AKEYCODE_8] = ska_scancode_8;
	ska_android_scancode_table[AKEYCODE_9] = ska_scancode_9;

	// Function keys
	ska_android_scancode_table[AKEYCODE_ENTER] = ska_scancode_return;
	ska_android_scancode_table[AKEYCODE_ESCAPE] = ska_scancode_escape;
	ska_android_scancode_table[AKEYCODE_BACK] = ska_scancode_escape;  // Back button = Escape
	ska_android_scancode_table[AKEYCODE_DEL] = ska_scancode_backspace;
	ska_android_scancode_table[AKEYCODE_TAB] = ska_scancode_tab;
	ska_android_scancode_table[AKEYCODE_SPACE] = ska_scancode_space;

	// Symbols
	ska_android_scancode_table[AKEYCODE_MINUS] = ska_scancode_minus;
	ska_android_scancode_table[AKEYCODE_EQUALS] = ska_scancode_equals;
	ska_android_scancode_table[AKEYCODE_LEFT_BRACKET] = ska_scancode_leftbracket;
	ska_android_scancode_table[AKEYCODE_RIGHT_BRACKET] = ska_scancode_rightbracket;
	ska_android_scancode_table[AKEYCODE_BACKSLASH] = ska_scancode_backslash;
	ska_android_scancode_table[AKEYCODE_SEMICOLON] = ska_scancode_semicolon;
	ska_android_scancode_table[AKEYCODE_APOSTROPHE] = ska_scancode_apostrophe;
	ska_android_scancode_table[AKEYCODE_GRAVE] = ska_scancode_grave;
	ska_android_scancode_table[AKEYCODE_COMMA] = ska_scancode_comma;
	ska_android_scancode_table[AKEYCODE_PERIOD] = ska_scancode_period;
	ska_android_scancode_table[AKEYCODE_SLASH] = ska_scancode_slash;

	ska_android_scancode_table[AKEYCODE_CAPS_LOCK] = ska_scancode_capslock;

	// F keys
	ska_android_scancode_table[AKEYCODE_F1] = ska_scancode_f1;
	ska_android_scancode_table[AKEYCODE_F2] = ska_scancode_f2;
	ska_android_scancode_table[AKEYCODE_F3] = ska_scancode_f3;
	ska_android_scancode_table[AKEYCODE_F4] = ska_scancode_f4;
	ska_android_scancode_table[AKEYCODE_F5] = ska_scancode_f5;
	ska_android_scancode_table[AKEYCODE_F6] = ska_scancode_f6;
	ska_android_scancode_table[AKEYCODE_F7] = ska_scancode_f7;
	ska_android_scancode_table[AKEYCODE_F8] = ska_scancode_f8;
	ska_android_scancode_table[AKEYCODE_F9] = ska_scancode_f9;
	ska_android_scancode_table[AKEYCODE_F10] = ska_scancode_f10;
	ska_android_scancode_table[AKEYCODE_F11] = ska_scancode_f11;
	ska_android_scancode_table[AKEYCODE_F12] = ska_scancode_f12;

	// Navigation
	ska_android_scancode_table[AKEYCODE_MOVE_HOME] = ska_scancode_home;
	ska_android_scancode_table[AKEYCODE_PAGE_UP] = ska_scancode_pageup;
	ska_android_scancode_table[AKEYCODE_FORWARD_DEL] = ska_scancode_delete;
	ska_android_scancode_table[AKEYCODE_MOVE_END] = ska_scancode_end;
	ska_android_scancode_table[AKEYCODE_PAGE_DOWN] = ska_scancode_pagedown;
	ska_android_scancode_table[AKEYCODE_DPAD_RIGHT] = ska_scancode_right;
	ska_android_scancode_table[AKEYCODE_DPAD_LEFT] = ska_scancode_left;
	ska_android_scancode_table[AKEYCODE_DPAD_DOWN] = ska_scancode_down;
	ska_android_scancode_table[AKEYCODE_DPAD_UP] = ska_scancode_up;

	// Modifiers
	ska_android_scancode_table[AKEYCODE_CTRL_LEFT] = ska_scancode_lctrl;
	ska_android_scancode_table[AKEYCODE_SHIFT_LEFT] = ska_scancode_lshift;
	ska_android_scancode_table[AKEYCODE_ALT_LEFT] = ska_scancode_lalt;
	ska_android_scancode_table[AKEYCODE_META_LEFT] = ska_scancode_lgui;
	ska_android_scancode_table[AKEYCODE_CTRL_RIGHT] = ska_scancode_rctrl;
	ska_android_scancode_table[AKEYCODE_SHIFT_RIGHT] = ska_scancode_rshift;
	ska_android_scancode_table[AKEYCODE_ALT_RIGHT] = ska_scancode_ralt;
	ska_android_scancode_table[AKEYCODE_META_RIGHT] = ska_scancode_rgui;
}

static ska_mouse_button_ ska_android_map_button(int32_t android_button_bit) {
	if (android_button_bit & AMOTION_EVENT_BUTTON_PRIMARY)   return ska_mouse_button_left;
	if (android_button_bit & AMOTION_EVENT_BUTTON_SECONDARY) return ska_mouse_button_right;
	if (android_button_bit & AMOTION_EVENT_BUTTON_TERTIARY)  return ska_mouse_button_middle;
	if (android_button_bit & AMOTION_EVENT_BUTTON_BACK)      return ska_mouse_button_x1;
	if (android_button_bit & AMOTION_EVENT_BUTTON_FORWARD)   return ska_mouse_button_x2;
	return 0;
}

static uint16_t ska_android_get_modifiers(int32_t meta_state) {
	uint16_t mods = 0;
	if (meta_state & AMETA_SHIFT_ON) mods |= ska_keymod_shift;
	if (meta_state & AMETA_CTRL_ON) mods |= ska_keymod_ctrl;
	if (meta_state & AMETA_ALT_ON) mods |= ska_keymod_alt;
	if (meta_state & AMETA_META_ON) mods |= ska_keymod_gui;
	return mods;
}

// ============================================================================
// Public Lifecycle API
// ============================================================================

SKA_API void ska_android_on_event(ska_android_event_ event) {
	ska_event_t ev = {0};
	ev.timestamp = (uint32_t)ska_time_get_elapsed_ms();

	switch (event) {
		case ska_android_event_resume:
			ev.type = ska_event_app_foreground;
			g_ska.app_is_visible = true;
			ska_post_event(&ev);
			ska_log(ska_log_info, "App resumed");
			break;

		case ska_android_event_pause:
			ev.type = ska_event_app_background;
			g_ska.app_is_visible = false;
			ska_post_event(&ev);
			ska_log(ska_log_info, "App paused");
			break;

		case ska_android_event_destroy:
			ev.type = ska_event_quit;
			ska_post_event(&ev);
			ska_log(ska_log_info, "App destroy requested");
			break;

		case ska_android_event_focus_gained:
			g_ska.app_has_focus = true;
			if (g_ska.window_count > 0 && g_ska.windows[0]) {
				ev.type = ska_event_window_focus_gained;
				ev.window.window_id = g_ska.windows[0]->id;
				g_ska.windows[0]->has_focus = true;
				ska_post_event(&ev);
			}
			break;

		case ska_android_event_focus_lost:
			g_ska.app_has_focus = false;
			if (g_ska.window_count > 0 && g_ska.windows[0]) {
				ev.type = ska_event_window_focus_lost;
				ev.window.window_id = g_ska.windows[0]->id;
				g_ska.windows[0]->has_focus = false;
				ska_post_event(&ev);
			}
			break;

		case ska_android_event_low_memory:
			ev.type = ska_event_app_lowmemory;
			ska_post_event(&ev);
			ska_log(ska_log_warn, "Low memory warning");
			break;
	}
}

SKA_API void ska_android_on_window_created(void *native_window) {
	if (!native_window) return;

	// If the window stub doesn't exist yet, stash for
	// ska_platform_window_create to pick up.
	if (g_ska.window_count == 0) {
		g_android_early.native_window = native_window;
		return;
	}

	ska_window_t *window = g_ska.windows[0];
	if (!window) return;

	window->native_window = (ANativeWindow *)native_window;

	int32_t width  = ANativeWindow_getWidth(window->native_window);
	int32_t height = ANativeWindow_getHeight(window->native_window);

	window->width           = width;
	window->height          = height;
	window->drawable_width  = width;
	window->drawable_height = height;
	window->is_visible      = true;
	window->dpi_scale       = ska_platform_get_dpi_scale(window);

	// Get position (relevant in freeform/DEX mode)
	ska_android_get_window_position(window);

	ska_event_t ev = {0};
	ev.timestamp        = (uint32_t)ska_time_get_elapsed_ms();
	ev.type             = ska_event_window_shown;
	ev.window.window_id = window->id;
	ska_post_event(&ev);

	ska_log(ska_log_info, "Android window created: %dx%d at (%d,%d) (dpi_scale=%.2f)",
		width, height, window->x, window->y, window->dpi_scale);
}

SKA_API void ska_android_on_window_destroyed(void) {
	if (g_ska.window_count == 0) return;

	ska_window_t *window = g_ska.windows[0];
	if (!window) return;

	window->is_visible = false;

	ska_event_t ev = {0};
	ev.timestamp        = (uint32_t)ska_time_get_elapsed_ms();
	ev.type             = ska_event_window_hidden;
	ev.window.window_id = window->id;
	ska_post_event(&ev);

	window->native_window = NULL;
}

SKA_API void ska_android_on_window_resized(int32_t width, int32_t height) {
	if (g_ska.window_count == 0) return;

	ska_window_t *window = g_ska.windows[0];
	if (!window) return;

	// Also update position (may change in freeform mode)
	int32_t old_x = window->x;
	int32_t old_y = window->y;
	ska_android_get_window_position(window);

	if (window->x != old_x || window->y != old_y) {
		ska_event_t move_ev = {0};
		move_ev.timestamp        = (uint32_t)ska_time_get_elapsed_ms();
		move_ev.type             = ska_event_window_moved;
		move_ev.window.window_id = window->id;
		move_ev.window.data1     = window->x;
		move_ev.window.data2     = window->y;
		ska_post_event(&move_ev);
	}

	if (width != window->width || height != window->height) {
		window->width           = width;
		window->height          = height;
		window->drawable_width  = width;
		window->drawable_height = height;

		ska_event_t ev = {0};
		ev.timestamp        = (uint32_t)ska_time_get_elapsed_ms();
		ev.type             = ska_event_window_resized;
		ev.window.window_id = window->id;
		ev.window.data1     = width;
		ev.window.data2     = height;
		ska_post_event(&ev);

		ska_log(ska_log_info, "Android window resized: %dx%d", width, height);
	}
}

// ============================================================================
// Input Helpers (private)
// ============================================================================

// Posts a touch/motion event. Action values match both NDK AMOTION_EVENT_ACTION_*
// and Java MotionEvent.ACTION_*: 0=DOWN, 1=UP, 2=MOVE, 5=POINTER_DOWN, 6=POINTER_UP
static bool ska_android_post_motion(int32_t action_masked, float x, float y) {
	if (g_ska.window_count == 0 || !g_ska.windows[0]) return false;

	ska_window_t *window = g_ska.windows[0];
	ska_event_t ev = {0};
	ev.timestamp = (uint32_t)ska_time_get_elapsed_ms();

	switch (action_masked) {
		case AMOTION_EVENT_ACTION_DOWN:
		case AMOTION_EVENT_ACTION_POINTER_DOWN:
			ev.type                   = ska_event_mouse_button_down;
			ev.mouse_button.window_id = window->id;
			ev.mouse_button.button    = ska_mouse_button_left;
			ev.mouse_button.pressed   = true;
			ev.mouse_button.clicks    = 1;
			ev.mouse_button.x         = (int32_t)x;
			ev.mouse_button.y         = (int32_t)y;

			g_ska.input_state.mouse_buttons |= (1 << (ska_mouse_button_left - 1));
			g_ska.input_state.mouse_x = (int32_t)x;
			g_ska.input_state.mouse_y = (int32_t)y;

			ska_post_event(&ev);
			return true;

		case AMOTION_EVENT_ACTION_UP:
		case AMOTION_EVENT_ACTION_POINTER_UP:
			ev.type                   = ska_event_mouse_button_up;
			ev.mouse_button.window_id = window->id;
			ev.mouse_button.button    = ska_mouse_button_left;
			ev.mouse_button.pressed   = false;
			ev.mouse_button.clicks    = 1;
			ev.mouse_button.x         = (int32_t)x;
			ev.mouse_button.y         = (int32_t)y;

			g_ska.input_state.mouse_buttons &= ~(1 << (ska_mouse_button_left - 1));
			g_ska.input_state.mouse_x = (int32_t)x;
			g_ska.input_state.mouse_y = (int32_t)y;

			ska_post_event(&ev);
			return true;

		case AMOTION_EVENT_ACTION_MOVE:
			ev.type                   = ska_event_mouse_motion;
			ev.mouse_motion.window_id = window->id;
			ev.mouse_motion.x         = (int32_t)x;
			ev.mouse_motion.y         = (int32_t)y;
			ev.mouse_motion.xrel      = (int32_t)x - g_ska.input_state.mouse_x;
			ev.mouse_motion.yrel      = (int32_t)y - g_ska.input_state.mouse_y;

			g_ska.input_state.mouse_x    = (int32_t)x;
			g_ska.input_state.mouse_y    = (int32_t)y;
			g_ska.input_state.mouse_xrel = ev.mouse_motion.xrel;
			g_ska.input_state.mouse_yrel = ev.mouse_motion.yrel;

			ska_post_event(&ev);
			return true;
	}

	return false;
}

// Posts a key event. Action values: 0=DOWN, 1=UP, 2=MULTIPLE (repeat).
static bool ska_android_post_key(int32_t action, int32_t keycode, int32_t meta_state) {
	if (g_ska.window_count == 0 || !g_ska.windows[0]) return false;
	if (keycode < 0 || keycode >= 256) return false;

	ska_window_t *window = g_ska.windows[0];
	bool pressed = (action == AKEY_EVENT_ACTION_DOWN);
	bool repeat  = (action == AKEY_EVENT_ACTION_MULTIPLE);

	ska_event_t ev = {0};
	ev.timestamp           = (uint32_t)ska_time_get_elapsed_ms();
	ev.type                = pressed ? ska_event_key_down : ska_event_key_up;
	ev.keyboard.window_id  = window->id;
	ev.keyboard.pressed    = pressed;
	ev.keyboard.repeat     = repeat;
	ev.keyboard.scancode   = ska_android_scancode_table[keycode];
	ev.keyboard.modifiers  = ska_android_get_modifiers(meta_state);

	if (ev.keyboard.scancode != ska_scancode_unknown) {
		g_ska.input_state.keyboard[ev.keyboard.scancode] = pressed ? 1 : 0;
	}
	g_ska.input_state.key_modifiers = ev.keyboard.modifiers;

	ska_post_event(&ev);
	return true;
}

// ============================================================================
// JNI Input Injection
// ============================================================================

// Cached JNI class/method IDs for input event extraction.
static struct {
	bool      initialized;
	jclass    motion_event_class;
	jmethodID motion_get_action;
	jmethodID motion_get_x;
	jmethodID motion_get_y;
	jclass    key_event_class;
	jmethodID key_get_action;
	jmethodID key_get_key_code;
	jmethodID key_get_meta_state;
} g_jni_input_cache = {0};

static bool ska_jni_input_cache_init(JNIEnv *env) {
	if (g_jni_input_cache.initialized) return true;

	jclass me = (*env)->FindClass(env, "android/view/MotionEvent");
	if (!me) return false;
	g_jni_input_cache.motion_event_class = (jclass)(*env)->NewGlobalRef(env, me);
	g_jni_input_cache.motion_get_action  = (*env)->GetMethodID(env, me, "getAction", "()I");
	g_jni_input_cache.motion_get_x       = (*env)->GetMethodID(env, me, "getX",      "()F");
	g_jni_input_cache.motion_get_y       = (*env)->GetMethodID(env, me, "getY",      "()F");
	(*env)->DeleteLocalRef(env, me);

	if (!g_jni_input_cache.motion_get_action ||
		!g_jni_input_cache.motion_get_x ||
		!g_jni_input_cache.motion_get_y) {
		if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
		return false;
	}

	jclass ke = (*env)->FindClass(env, "android/view/KeyEvent");
	if (!ke) {
		if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
		return false;
	}
	g_jni_input_cache.key_event_class    = (jclass)(*env)->NewGlobalRef(env, ke);
	g_jni_input_cache.key_get_action     = (*env)->GetMethodID(env, ke, "getAction",    "()I");
	g_jni_input_cache.key_get_key_code   = (*env)->GetMethodID(env, ke, "getKeyCode",   "()I");
	g_jni_input_cache.key_get_meta_state = (*env)->GetMethodID(env, ke, "getMetaState", "()I");
	(*env)->DeleteLocalRef(env, ke);

	if (!g_jni_input_cache.key_get_action ||
		!g_jni_input_cache.key_get_key_code ||
		!g_jni_input_cache.key_get_meta_state) {
		if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
		return false;
	}

	g_jni_input_cache.initialized = true;
	return true;
}

SKA_API bool ska_android_on_input(void *java_input_event) {
	if (!java_input_event) return false;

	JNIEnv *env = ska_android_get_jni_env();
	if (!env) return false;

	if (!ska_jni_input_cache_init(env)) {
		ska_log(ska_log_error, "Failed to initialize JNI input cache");
		return false;
	}

	jobject event = (jobject)java_input_event;

	if ((*env)->IsInstanceOf(env, event, g_jni_input_cache.motion_event_class)) {
		int32_t action = (*env)->CallIntMethod(env, event, g_jni_input_cache.motion_get_action);
		int32_t action_masked = action & 0xFF; // AMOTION_EVENT_ACTION_MASK
		float x = (*env)->CallFloatMethod(env, event, g_jni_input_cache.motion_get_x);
		float y = (*env)->CallFloatMethod(env, event, g_jni_input_cache.motion_get_y);
		return ska_android_post_motion(action_masked, x, y);
	}

	if ((*env)->IsInstanceOf(env, event, g_jni_input_cache.key_event_class)) {
		int32_t action     = (*env)->CallIntMethod(env, event, g_jni_input_cache.key_get_action);
		int32_t keycode    = (*env)->CallIntMethod(env, event, g_jni_input_cache.key_get_key_code);
		int32_t meta_state = (*env)->CallIntMethod(env, event, g_jni_input_cache.key_get_meta_state);
		return ska_android_post_key(action, keycode, meta_state);
	}

	ska_log(ska_log_warn, "ska_android_on_input: unrecognized event type");
	return false;
}

// ============================================================================
// Standalone Glue Callbacks
// ============================================================================

// Command handler for app lifecycle events (standalone mode).
// Dispatches to the public injection API so both paths share the same logic.
static void ska_android_handle_cmd(struct android_app* app, int32_t cmd) {
	switch (cmd) {
		case APP_CMD_INIT_WINDOW:
			if (app->window) ska_android_on_window_created(app->window);
			break;

		case APP_CMD_TERM_WINDOW:
			ska_android_on_window_destroyed();
			break;

		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONFIG_CHANGED:
			if (app->window && g_ska.window_count > 0)
				ska_android_on_window_resized(ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
			break;

		case APP_CMD_GAINED_FOCUS:  ska_android_on_event(ska_android_event_focus_gained); break;
		case APP_CMD_LOST_FOCUS:    ska_android_on_event(ska_android_event_focus_lost);   break;
		case APP_CMD_PAUSE:         ska_android_on_event(ska_android_event_pause);        break;
		case APP_CMD_RESUME:        ska_android_on_event(ska_android_event_resume);       break;
		case APP_CMD_LOW_MEMORY:    ska_android_on_event(ska_android_event_low_memory);   break;
		case APP_CMD_DESTROY:       ska_android_on_event(ska_android_event_destroy);      break;
	}
}

// Input event handler
static int32_t ska_android_handle_input(struct android_app* app, AInputEvent* input_event) {
	if (g_ska.window_count == 0) {
		return 0;
	}

	ska_window_t* window = g_ska.windows[0];
	if (!window) {
		return 0;
	}

	int32_t event_type = AInputEvent_getType(input_event);

	if (event_type == AINPUT_EVENT_TYPE_KEY) {
		return ska_android_post_key(
			AKeyEvent_getAction(input_event),
			AKeyEvent_getKeyCode(input_event),
			AKeyEvent_getMetaState(input_event)) ? 1 : 0;

	} else if (event_type == AINPUT_EVENT_TYPE_MOTION) {
		ska_event_t event = {0};
		event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
		int32_t action = AMotionEvent_getAction(input_event);
		int32_t action_masked = action & AMOTION_EVENT_ACTION_MASK;
		int32_t source = AInputEvent_getSource(input_event);

		// Get coordinates - Android provides float precision
		// Note: getX/getY return coordinates relative to the window
		// getXOffset/getYOffset are NOT position offsets - they're historical offsets
		// for batch events. We should use getX/getY directly for window-relative coords.
		float x_float = AMotionEvent_getX(input_event, 0);
		float y_float = AMotionEvent_getY(input_event, 0);

		// Round to avoid cumulative positioning errors
		int32_t x = (int32_t)(x_float + 0.5f);
		int32_t y = (int32_t)(y_float + 0.5f);

		// Handle mouse scroll wheel
		if (action_masked == AMOTION_EVENT_ACTION_SCROLL) {
			float vscroll = AMotionEvent_getAxisValue(input_event, AMOTION_EVENT_AXIS_VSCROLL, 0);
			float hscroll = AMotionEvent_getAxisValue(input_event, AMOTION_EVENT_AXIS_HSCROLL, 0);

			event.type = ska_event_mouse_wheel;
			event.mouse_wheel.window_id = window->id;
			event.mouse_wheel.x = (int32_t)hscroll;
			event.mouse_wheel.y = (int32_t)vscroll;
			event.mouse_wheel.precise_x = hscroll;
			event.mouse_wheel.precise_y = vscroll;

			ska_post_event(&event);
			return 1;
		}

		// Handle mouse button events (for mice/trackpads connected to Android).
		// getButtonState() returns cumulative state, so we diff against the
		// previous value to determine which specific button changed.
		if ((source & AINPUT_SOURCE_MOUSE) && action_masked == AMOTION_EVENT_ACTION_BUTTON_PRESS) {
			int32_t button_state  = AMotionEvent_getButtonState(input_event);
			int32_t newly_pressed = button_state & ~g_android_prev_button_state;
			g_android_prev_button_state = button_state;

			ska_mouse_button_ button = ska_android_map_button(newly_pressed);
			if (!button) return 1;

			event.type = ska_event_mouse_button_down;
			event.mouse_button.window_id = window->id;
			event.mouse_button.button = button;
			event.mouse_button.pressed = true;
			event.mouse_button.clicks = 1;
			event.mouse_button.x = (int32_t)x;
			event.mouse_button.y = (int32_t)y;

			g_ska.input_state.mouse_buttons |= (1 << (button - 1));

			ska_post_event(&event);
			return 1;
		}

		if ((source & AINPUT_SOURCE_MOUSE) && action_masked == AMOTION_EVENT_ACTION_BUTTON_RELEASE) {
			int32_t button_state = AMotionEvent_getButtonState(input_event);
			int32_t released     = g_android_prev_button_state & ~button_state;
			g_android_prev_button_state = button_state;

			ska_mouse_button_ button = ska_android_map_button(released);
			if (!button) return 1;

			event.type = ska_event_mouse_button_up;
			event.mouse_button.window_id = window->id;
			event.mouse_button.button = button;
			event.mouse_button.pressed = false;
			event.mouse_button.clicks = 1;
			event.mouse_button.x = (int32_t)x;
			event.mouse_button.y = (int32_t)y;

			g_ska.input_state.mouse_buttons &= ~(1 << (button - 1));

			ska_post_event(&event);
			return 1;
		}

		// Handle mouse hover motion (when no button is pressed)
		if ((source & AINPUT_SOURCE_MOUSE) && action_masked == AMOTION_EVENT_ACTION_HOVER_MOVE) {
			event.type = ska_event_mouse_motion;
			event.mouse_motion.window_id = window->id;
			event.mouse_motion.x = (int32_t)x;
			event.mouse_motion.y = (int32_t)y;
			event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
			event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

			g_ska.input_state.mouse_x = (int32_t)x;
			g_ska.input_state.mouse_y = (int32_t)y;
			g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
			g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

			ska_post_event(&event);
			return 1;
		}

		switch (action_masked) {
			case AMOTION_EVENT_ACTION_DOWN:
				// Primary touch down = Mouse motion + Left button down
				// First send motion event to update position
				if ((int32_t)x != g_ska.input_state.mouse_x || (int32_t)y != g_ska.input_state.mouse_y) {
					event.type = ska_event_mouse_motion;
					event.mouse_motion.window_id = window->id;
					event.mouse_motion.x = (int32_t)x;
					event.mouse_motion.y = (int32_t)y;
					event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
					event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

					g_ska.input_state.mouse_x = (int32_t)x;
					g_ska.input_state.mouse_y = (int32_t)y;
					g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
					g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

					ska_post_event(&event);
				}

				// Then send button down
				event.type = ska_event_mouse_button_down;
				event.mouse_button.window_id = window->id;
				event.mouse_button.button = ska_mouse_button_left;
				event.mouse_button.pressed = true;
				event.mouse_button.clicks = 1;
				event.mouse_button.x = (int32_t)x;
				event.mouse_button.y = (int32_t)y;

				g_ska.input_state.mouse_buttons |= (1 << (ska_mouse_button_left - 1));

				ska_post_event(&event);
				return 1;

			case AMOTION_EVENT_ACTION_UP:
				// Primary touch up = Mouse motion + Left button up
				// First send motion event to update position
				if ((int32_t)x != g_ska.input_state.mouse_x || (int32_t)y != g_ska.input_state.mouse_y) {
					event.type = ska_event_mouse_motion;
					event.mouse_motion.window_id = window->id;
					event.mouse_motion.x = (int32_t)x;
					event.mouse_motion.y = (int32_t)y;
					event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
					event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

					g_ska.input_state.mouse_x = (int32_t)x;
					g_ska.input_state.mouse_y = (int32_t)y;
					g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
					g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

					ska_post_event(&event);
				}

				// Then send button up
				event.type = ska_event_mouse_button_up;
				event.mouse_button.window_id = window->id;
				event.mouse_button.button = ska_mouse_button_left;
				event.mouse_button.pressed = false;
				event.mouse_button.clicks = 1;
				event.mouse_button.x = (int32_t)x;
				event.mouse_button.y = (int32_t)y;

				g_ska.input_state.mouse_buttons &= ~(1 << (ska_mouse_button_left - 1));

				ska_post_event(&event);
				return 1;

			case AMOTION_EVENT_ACTION_MOVE:
				// Touch/Mouse move = Mouse motion
				// Note: For touch, this only fires while touching. For mouse, this fires when dragging.
				event.type = ska_event_mouse_motion;
				event.mouse_motion.window_id = window->id;
				event.mouse_motion.x = (int32_t)x;
				event.mouse_motion.y = (int32_t)y;
				event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
				event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

				g_ska.input_state.mouse_x = (int32_t)x;
				g_ska.input_state.mouse_y = (int32_t)y;
				g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
				g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

				ska_post_event(&event);
				return 1;
		}
	}

	return 0;
}

bool ska_platform_init(void) {
	// Copy pre-init state into g_ska
	g_ska.android_app = g_android_early.android_app;

	// JNI_GetCreatedJavaVMs isn't a link-time symbol in the NDK.
	// libnativehelper.so is in the public linker namespace on API 31+;
	// on API 24-30 the dlopen may fail, so we fall back to RTLD_DEFAULT.
	typedef jint (*pfn_JNI_GetCreatedJavaVMs)(JavaVM**, jsize, jsize*);
	void *lib = dlopen("libnativehelper.so", RTLD_NOW);
	pfn_JNI_GetCreatedJavaVMs getVMs = (pfn_JNI_GetCreatedJavaVMs)dlsym(
		lib ? lib : RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
	if (getVMs) {
		JavaVM *vm    = NULL;
		jsize   count = 0;
		if (getVMs(&vm, 1, &count) == JNI_OK && count > 0)
			g_ska.java_vm = vm;
	}
	if (!g_ska.java_vm)
		ska_log(ska_log_error, "Failed to discover JavaVM via JNI_GetCreatedJavaVMs");

	// Initialize scancode table (needed in both modes)
	ska_init_scancode_table();

	if (g_ska.android_app) {
		// Standalone mode: set up glue callbacks and get context from
		// NativeActivity
		g_ska.android_app->onAppCmd     = ska_android_handle_cmd;
		g_ska.android_app->onInputEvent = ska_android_handle_input;
		ska_android_set_context(g_ska.android_app->activity->clazz);
	} else if (g_android_early.android_context) {
		// Library mode: promote raw context pointer to a JNI global ref
		ska_android_set_context(g_android_early.android_context);
		g_android_early.android_context = NULL;
	}

	// Cache JNI method/field IDs for window management (needs activity set)
	ska_jni_cache_init();

	// Set up AAssetManager — standalone gets it from the glue struct,
	// library mode extracts it from the Context via JNI.
	if (g_ska.android_app && g_ska.android_app->activity &&
	    g_ska.android_app->activity->assetManager) {
		g_ska.asset_manager = g_ska.android_app->activity->assetManager;
	} else if (g_ska.android_context) {
		JNIEnv *env = (JNIEnv*)ska_android_get_jni_env();
		if (env) {
			jclass    ctx_class = (*env)->GetObjectClass(env, (jobject)g_ska.android_context);
			jmethodID getAssets = (*env)->GetMethodID(env, ctx_class, "getAssets",
				"()Landroid/content/res/AssetManager;");
			jobject java_am = (*env)->CallObjectMethod(env,
				(jobject)g_ska.android_context, getAssets);
			if (java_am) {
				g_ska.asset_manager = AAssetManager_fromJava(env, java_am);
				(*env)->DeleteLocalRef(env, java_am);
			}
			(*env)->DeleteLocalRef(env, ctx_class);
		}
	}

	ska_log(ska_log_info, "Android platform initialized");

	return true;
}

void ska_platform_shutdown(void) {
	g_ska.asset_manager = NULL;
	g_android_prev_button_state = 0;

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();

	ska_jni_cache_shutdown();

	// Release input cache global refs
	if (env && g_jni_input_cache.initialized) {
		if (g_jni_input_cache.motion_event_class) (*env)->DeleteGlobalRef(env, g_jni_input_cache.motion_event_class);
		if (g_jni_input_cache.key_event_class)    (*env)->DeleteGlobalRef(env, g_jni_input_cache.key_event_class);
		memset(&g_jni_input_cache, 0, sizeof(g_jni_input_cache));
	}

	// Release file dialog cache global refs
	if (env && g_file_dialog_jni.initialized) {
		if (g_file_dialog_jni.intent_class)   (*env)->DeleteGlobalRef(env, g_file_dialog_jni.intent_class);
		if (g_file_dialog_jni.pm_class)       (*env)->DeleteGlobalRef(env, g_file_dialog_jni.pm_class);
		if (g_file_dialog_jni.fragment_class) (*env)->DeleteGlobalRef(env, g_file_dialog_jni.fragment_class);
		memset(&g_file_dialog_jni, 0, sizeof(g_file_dialog_jni));
	}

	// Release kvp store cache global refs
	if (env && g_kvp_jni.initialized) {
		if (g_kvp_jni.base64_class) (*env)->DeleteGlobalRef(env, g_kvp_jni.base64_class);
		memset(&g_kvp_jni, 0, sizeof(g_kvp_jni));
	}

	if (g_ska.android_app) {
		g_ska.android_app->onAppCmd     = NULL;
		g_ska.android_app->onInputEvent = NULL;
	}

	// Release the Activity global ref
	ska_android_set_context(NULL);

	ska_log(ska_log_info, "Android platform shutdown");
}

void* ska_android_get_vm(void) {
	return g_ska.java_vm;
}

void* ska_android_get_activity(void) {
	return g_ska.android_context;
}

bool ska_platform_window_create(
	ska_window_t* window,
	const char* title,
	int32_t x, int32_t y,
	int32_t w, int32_t h,
	uint32_t flags
) {
	(void)x; (void)y; (void)w; (void)h; (void)flags;

	// Android only provides a single native window per Activity.
	// Note: ska_window_alloc() already incremented window_count for this
	// window before calling us, so the current window is included in the count.
	if (g_ska.window_count > 1) {
		ska_set_error("Android does not support multiple windows");
		return false;
	}

	// On Android, we don't create windows - the system provides one
	// We'll use the ANativeWindow when it becomes available

	window->title = ska_strdup(title ? title : "sk_app");

	// Window dimensions will be set when we get APP_CMD_INIT_WINDOW
	window->native_window = NULL;
	window->is_visible = false;

	// Check if a native window was delivered before the stub was created
	// (e.g. Xamarin surface callback arrived very early).
	if (g_android_early.native_window) {
		window->native_window = (ANativeWindow *)g_android_early.native_window;
		g_android_early.native_window = NULL;
	}

	// Non-Activity contexts (Services) don't have windows — bail instead of
	// spinning forever waiting for a native window that will never arrive.
	if (!g_ska.android_app && !g_ska.android_is_activity) {
		ska_set_error("Window creation requires an Activity context");
		return false;
	}

	ska_log(ska_log_info, "Android window stub created, waiting for native window");

	// Wait for the native window to become available by polling the event loop.
	// Standalone: android_main() processes APP_CMD_INIT_WINDOW.
	// Library:    host calls ska_android_on_window_created() directly.
	while (window->native_window == NULL) {
		ska_time_sleep(10);

		if (g_ska.android_app && g_ska.android_app->destroyRequested) {
			ska_set_error("App destroy requested while waiting for window");
			return false;
		}
	}

	ska_log(ska_log_info, "Native window is now available: %dx%d", window->width, window->height);

	return true;
}

void ska_platform_window_destroy(ska_window_t* window) {
	// On Android, we don't destroy the window - the system manages it
	// Just clear our reference
	window->native_window = NULL;
}

void ska_platform_window_set_title(ska_window_t* window, const char* title) {
	// Android doesn't support changing window title at runtime
	if (window->title) {
		ska_free(window->title);
	}
	window->title = ska_strdup(title);
}

// Callback data for window layout changes dispatched to the UI thread.
typedef struct {
	int32_t a, b;     // x/y or w/h
	jfieldID field_a; // lp_x or lp_width
	jfieldID field_b; // lp_y or lp_height
} ska_layout_change_t;

static void ska_set_layout_params_cb(void *data) {
	ska_layout_change_t *lc = (ska_layout_change_t*)data;

	JNIEnv *env = (JNIEnv*)ska_android_get_jni_env();
	if (!env || !g_ska.android_context || !g_ska.android_is_activity) { ska_free(lc); return; }

	jobject jwindow = (*env)->CallObjectMethod(env,
		(jobject)g_ska.android_context, g_jni_cache.activity_getWindow);
	if (jwindow) {
		jobject lp = (*env)->CallObjectMethod(env, jwindow,
			g_jni_cache.window_getAttributes);
		if (lp) {
			(*env)->SetIntField(env, lp, lc->field_a, lc->a);
			(*env)->SetIntField(env, lp, lc->field_b, lc->b);
			(*env)->CallVoidMethod(env, jwindow,
				g_jni_cache.window_setAttributes, lp);
			(*env)->DeleteLocalRef(env, lp);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
	ska_free(lc);
}

void ska_platform_window_set_frame_position(ska_window_t* window, int32_t x, int32_t y) {
	// In freeform/DEX mode, change window position via WindowManager.LayoutParams.
	// setAttributes must run on the UI thread.
	ska_layout_change_t *lc = (ska_layout_change_t*)ska_malloc(sizeof(*lc));
	if (!lc) return;
	*lc = (ska_layout_change_t){ .a = x, .b = y,
		.field_a = g_jni_cache.lp_x, .field_b = g_jni_cache.lp_y };
	ska_android_run_on_ui_thread(ska_set_layout_params_cb, lc);

	window->x = x;
	window->y = y;
}

void ska_platform_window_set_frame_size(ska_window_t* window, int32_t w, int32_t h) {
	// In freeform/DEX mode, change window size via WindowManager.LayoutParams.
	// setAttributes must run on the UI thread.
	(void)window; // Size change reflected via APP_CMD_WINDOW_RESIZED callback

	ska_layout_change_t *lc = (ska_layout_change_t*)ska_malloc(sizeof(*lc));
	if (!lc) return;
	*lc = (ska_layout_change_t){ .a = w, .b = h,
		.field_a = g_jni_cache.lp_width, .field_b = g_jni_cache.lp_height };
	ska_android_run_on_ui_thread(ska_set_layout_params_cb, lc);
}

void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* out_left, int32_t* out_right, int32_t* out_top, int32_t* out_bottom) {
	// In freeform/DEX mode, windows may have decorations (title bar, borders)
	// We get these via getWindow().getDecorView() dimensions vs content view
	if (out_left)   *out_left   = 0;
	if (out_right)  *out_right  = 0;
	if (out_top)    *out_top    = 0;
	if (out_bottom) *out_bottom = 0;

	if (!window || !window->native_window) {
		return;
	}

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) {
		return;
	}

	if (!g_ska.android_context || !g_ska.android_is_activity) {
		return;
	}

	jobject jwindow = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context, g_jni_cache.activity_getWindow);

	if (jwindow) {
		jobject decor_view = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getDecorView);

		if (decor_view) {
			jint decor_width  = (*env)->CallIntMethod(env, decor_view, g_jni_cache.view_getWidth);
			jint decor_height = (*env)->CallIntMethod(env, decor_view, g_jni_cache.view_getHeight);

			// Get content (client) area size from native window
			int32_t content_width  = ANativeWindow_getWidth(window->native_window);
			int32_t content_height = ANativeWindow_getHeight(window->native_window);

			// Frame extents = decor size - content size
			// For Android, decorations are typically only at the top (title bar)
			int32_t total_h_diff = decor_width - content_width;
			int32_t total_v_diff = decor_height - content_height;

			// Assume symmetric horizontal borders (if any)
			int32_t left = total_h_diff / 2;
			if (out_left)   *out_left   = left;
			if (out_right)  *out_right  = total_h_diff - left;
			// Title bar at top, no bottom border typically
			if (out_top)    *out_top    = total_v_diff;
			if (out_bottom) *out_bottom = 0;

			(*env)->DeleteLocalRef(env, decor_view);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
}

void ska_platform_window_show(ska_window_t* window) {
	// Android windows are always visible when active
	window->is_visible = true;
}

void ska_platform_window_hide(ska_window_t* window) {
	// Cannot hide Android windows
	window->is_visible = false;
}

void ska_platform_window_maximize(ska_window_t* window) {
	// Android windows are always maximized
	(void)window;
}

void ska_platform_window_minimize(ska_window_t* window) {
	// Cannot minimize Android windows programmatically
	(void)window;
}

void ska_platform_window_restore(ska_window_t* window) {
	// No-op on Android
	(void)window;
}

void ska_platform_window_raise(ska_window_t* window) {
	// Android windows are always on top
	(void)window;
}

void ska_platform_window_get_drawable_size(ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (opt_out_width)  *opt_out_width  = window->drawable_width;
	if (opt_out_height) *opt_out_height = window->drawable_height;
}

float ska_platform_get_dpi_scale(const ska_window_t* window) {
	(void)window;

	// Get display density from Android configuration
	if (g_ska.android_app && g_ska.android_app->config) {
		int32_t density = AConfiguration_getDensity(g_ska.android_app->config);
		if (density > 0 && density != ACONFIGURATION_DENSITY_NONE) {
			// Android uses 160 DPI as baseline (mdpi)
			return (float)density / 160.0f;
		}
	}

	// Library mode fallback: get density from Context via JNI
	// context.getResources().getDisplayMetrics().density
	if (g_ska.android_context) {
		JNIEnv *env = (JNIEnv*)ska_android_get_jni_env();
		if (env) {
			jclass    ctx_class    = (*env)->GetObjectClass(env, (jobject)g_ska.android_context);
			jmethodID getResources = (*env)->GetMethodID(env, ctx_class,
				"getResources", "()Landroid/content/res/Resources;");
			jobject res = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context, getResources);
			(*env)->DeleteLocalRef(env, ctx_class);
			if (res) {
				jclass    res_class  = (*env)->GetObjectClass(env, res);
				jmethodID getMetrics = (*env)->GetMethodID(env, res_class,
					"getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
				jobject metrics = (*env)->CallObjectMethod(env, res, getMetrics);
				(*env)->DeleteLocalRef(env, res_class);
				(*env)->DeleteLocalRef(env, res);
				if (metrics) {
					jclass   dm_class     = (*env)->GetObjectClass(env, metrics);
					jfieldID densityField = (*env)->GetFieldID(env, dm_class, "density", "F");
					float    density      = (*env)->GetFloatField(env, metrics, densityField);
					(*env)->DeleteLocalRef(env, dm_class);
					(*env)->DeleteLocalRef(env, metrics);
					return density;
				}
			}
		}
	}

	return 1.0f;
}

float ska_platform_get_refresh_rate(const ska_window_t* window) {
	(void)window;

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) {
		return 0.0f;
	}

	if (!g_ska.android_context || !g_ska.android_is_activity) {
		return 0.0f;
	}

	jobject wm = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context,
		g_jni_cache.activity_getWindowManager);
	if (!wm) return 0.0f;

	jobject display = (*env)->CallObjectMethod(env, wm, g_jni_cache.wm_getDefaultDisplay);
	(*env)->DeleteLocalRef(env, wm);
	if (!display) return 0.0f;

	jfloat rate = (*env)->CallFloatMethod(env, display, g_jni_cache.display_getRefreshRate);
	(*env)->DeleteLocalRef(env, display);

	return (float)rate;
}

void ska_platform_warp_mouse(ska_window_t* window, int32_t x, int32_t y) {
	// Cannot warp cursor on touchscreen
	(void)window; (void)x; (void)y;
}

void ska_platform_set_cursor(ska_system_cursor_ cursor) {
	// No cursor on touchscreen
	(void)cursor;
}

void ska_platform_show_cursor(bool show) {
	// No cursor on touchscreen
	(void)show;
}

bool ska_platform_set_relative_mouse_mode(bool enabled) {
	// Not applicable on touchscreen
	(void)enabled;
	return false;
}

void ska_platform_pump_events(void) {
	// On Android, events are already being pumped by the android_main() loop
	// in the main thread. The user's main() runs in a separate thread and
	// consumes events from the thread-safe event queue.
	// Calling ALooper_pollOnce() here would fail with "No looper for this thread!"
	// because the user thread doesn't have a looper attached.
}

/////////////////////////////////////////
// Android specific subset of Vulkan header
/////////////////////////////////////////

typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR {
	VkStructureType                   sType;
	const void*                       pNext;
	VkAndroidSurfaceCreateFlagsKHR    flags;
	ANativeWindow*                    window;
} VkAndroidSurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateAndroidSurfaceKHR)(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* pCreateInfo, const /*VkAllocationCallbacks*/ void* pAllocator, VkSurfaceKHR* pSurface);

/////////////////////////////////////////

const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count) {
	static const char* extensions[] = {
		"VK_KHR_surface",
		"VK_KHR_android_surface"
	};
	*out_count = 2;
	return extensions;
}

bool ska_platform_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	if (!window->native_window) {
		ska_set_error("Native window not available");
		return false;
	}

	void* module = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!module) {
		ska_set_error("Failed to load Vulkan .so");
		return false;
	}

	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr) {
		ska_set_error("Failed to load vkGetInstanceProcAddr");
		return false;
	}

	PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR");
	if (!vkCreateAndroidSurfaceKHR) {
		ska_set_error("Failed to load vkCreateAndroidSurfaceKHR");
		return false;
	}

	VkAndroidSurfaceCreateInfoKHR create_info = {0};
	create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	create_info.window = window->native_window;

	VkResult result = vkCreateAndroidSurfaceKHR(instance, &create_info, NULL, out_surface);
	if (result != VK_SUCCESS) {
		ska_set_error("Failed to create Vulkan Android surface: %d", result);
		return false;
	}

	return true;
}

// ========== Text Input Platform Functions ==========

void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type) {
	// Requires an Activity — Services have no window/decor view for keyboard focus
	JNIEnv* env = ska_android_get_jni_env();
	if (!env || !g_ska.android_context || !g_ska.android_is_activity) {
		return;
	}

	jstring svc_name = (*env)->NewStringUTF(env, "input_method");
	jobject imm      = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context, g_jni_cache.ctx_getSystemService, svc_name);
	(*env)->DeleteLocalRef(env, svc_name);
	if (!imm) return;

	// Get decor view from activity's window — used by both show and hide paths
	jobject jwindow = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context, g_jni_cache.activity_getWindow);
	if (!jwindow) { (*env)->DeleteLocalRef(env, imm); return; }

	jobject decor_view = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getDecorView);
	(*env)->DeleteLocalRef(env, jwindow);
	if (!decor_view) { (*env)->DeleteLocalRef(env, imm); return; }

	if (visible) {
		(*env)->CallBooleanMethod(env, imm, g_jni_cache.imm_showSoftInput, decor_view, 0);
	} else {
		jobject token = (*env)->CallObjectMethod(env, decor_view, g_jni_cache.view_getWindowToken);
		if (token) {
			(*env)->CallBooleanMethod(env, imm, g_jni_cache.imm_hideSoftInputFromWindow, token, 0);
			(*env)->DeleteLocalRef   (env, token);
		}
	}

	(*env)->DeleteLocalRef(env, decor_view);
	(*env)->DeleteLocalRef(env, imm);
	(void)type; // TODO: Use type to set input mode
}

// ========== Clipboard Platform Functions ==========

char* ska_platform_clipboard_get_text(void) {
	JNIEnv* env = ska_android_get_jni_env();
	if (!env || !g_ska.android_context) return NULL;

	char*   result     = NULL;
	jstring svc_name   = NULL;
	jobject cm         = NULL;
	jobject clip_data  = NULL;
	jobject clip_item  = NULL;
	jobject char_seq   = NULL;
	jstring text_jstr  = NULL;

	svc_name = (*env)->NewStringUTF(env, "clipboard");
	cm = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context,
		g_jni_cache.ctx_getSystemService, svc_name);
	if (!cm) goto cleanup;

	if (!(*env)->CallBooleanMethod(env, cm, g_jni_cache.clipboard_hasPrimaryClip)) goto cleanup;

	clip_data = (*env)->CallObjectMethod(env, cm, g_jni_cache.clipboard_getPrimaryClip);
	if (!clip_data) goto cleanup;

	clip_item = (*env)->CallObjectMethod(env, clip_data, g_jni_cache.clip_data_getItemAt, 0);
	if (!clip_item) goto cleanup;

	char_seq = (*env)->CallObjectMethod(env, clip_item, g_jni_cache.clip_item_getText);
	if (!char_seq) goto cleanup;

	text_jstr = (jstring)(*env)->CallObjectMethod(env, char_seq, g_jni_cache.charseq_toString);
	if (!text_jstr) goto cleanup;

	const char* utf8 = (*env)->GetStringUTFChars(env, text_jstr, NULL);
	if (utf8) {
		size_t len = strlen(utf8);
		result = (char*)ska_malloc(len + 1);
		if (result) memcpy(result, utf8, len + 1);
		(*env)->ReleaseStringUTFChars(env, text_jstr, utf8);
	}

cleanup:
	if (text_jstr)  (*env)->DeleteLocalRef(env, text_jstr);
	if (char_seq)   (*env)->DeleteLocalRef(env, char_seq);
	if (clip_item)  (*env)->DeleteLocalRef(env, clip_item);
	if (clip_data)  (*env)->DeleteLocalRef(env, clip_data);
	if (cm)         (*env)->DeleteLocalRef(env, cm);
	if (svc_name)   (*env)->DeleteLocalRef(env, svc_name);
	return result;
}

bool ska_platform_clipboard_set_text(const char* text) {
	if (!text) {
		ska_set_error("ska_platform_clipboard_set_text: NULL text");
		return false;
	}

	JNIEnv* env = ska_android_get_jni_env();
	if (!env || !g_ska.android_context) {
		ska_set_error("ska_platform_clipboard_set_text: JNI or activity not available");
		return false;
	}

	bool    ok          = false;
	jstring svc_name    = NULL;
	jobject cm          = NULL;
	jstring label       = NULL;
	jstring text_jstr   = NULL;
	jobject clip_data   = NULL;

	svc_name = (*env)->NewStringUTF(env, "clipboard");
	cm = (*env)->CallObjectMethod(env, (jobject)g_ska.android_context,
		g_jni_cache.ctx_getSystemService, svc_name);
	if (!cm) {
		ska_set_error("ska_platform_clipboard_set_text: failed to get clipboard manager");
		goto cleanup;
	}

	label     = (*env)->NewStringUTF(env, "text");
	text_jstr = (*env)->NewStringUTF(env, text);
	clip_data = (*env)->CallStaticObjectMethod(env, g_jni_cache.clip_data_class,
		g_jni_cache.clip_data_newPlainText, label, text_jstr);
	if (!clip_data) {
		ska_set_error("ska_platform_clipboard_set_text: failed to create clip data");
		goto cleanup;
	}

	(*env)->CallVoidMethod(env, cm, g_jni_cache.clipboard_setPrimaryClip, clip_data);
	ok = true;

cleanup:
	if (clip_data) (*env)->DeleteLocalRef(env, clip_data);
	if (text_jstr) (*env)->DeleteLocalRef(env, text_jstr);
	if (label)     (*env)->DeleteLocalRef(env, label);
	if (cm)        (*env)->DeleteLocalRef(env, cm);
	if (svc_name)  (*env)->DeleteLocalRef(env, svc_name);
	return ok;
}

// ========== Asset I/O (Android) ==========

SKA_API bool ska_asset_read(const char* asset_name, void** out_data, size_t* out_size) {
	if (!asset_name) {
		ska_set_error("ska_asset_read: NULL asset_name");
		return false;
	}

	if (!out_data) {
		ska_set_error("ska_asset_read: NULL out_data");
		return false;
	}

	if (!g_ska.asset_manager) {
		ska_set_error("ska_asset_read: AAssetManager not available");
		return false;
	}

	AAssetManager* asset_manager = (AAssetManager*)g_ska.asset_manager;

	AAsset* asset = AAssetManager_open(asset_manager, asset_name, AASSET_MODE_BUFFER);
	if (!asset) {
		return false;
	}

	off_t asset_length = AAsset_getLength(asset);
	if (asset_length < 0) {
		ska_set_error("ska_asset_read: Failed to get asset length for '%s'", asset_name);
		AAsset_close(asset);
		return false;
	}

	void* data = ska_malloc((size_t)asset_length);
	if (!data) {
		ska_set_error("ska_asset_read: Failed to allocate %ld bytes", (long)asset_length);
		AAsset_close(asset);
		return false;
	}

	int bytes_read = AAsset_read(asset, data, (size_t)asset_length);
	AAsset_close(asset);

	if (bytes_read != asset_length) {
		ska_set_error("ska_asset_read: Read %d bytes, expected %ld", bytes_read, (long)asset_length);
		ska_free(data);
		return false;
	}

	*out_data = data;
	if (out_size) {
		*out_size = (size_t)asset_length;
	}

	return true;
}

SKA_API bool ska_asset_read_text(const char* asset_name, char** out_text) {
	if (!asset_name) {
		ska_set_error("ska_asset_read_text: NULL asset_name");
		return false;
	}

	if (!out_text) {
		ska_set_error("ska_asset_read_text: NULL out_text");
		return false;
	}

	size_t file_size = 0;
	void* data = NULL;
	if (!ska_asset_read(asset_name, &data, &file_size)) {
		return false;
	}

	// Allocate +1 for null terminator
	char* text = (char*)ska_realloc(data, file_size + 1);
	if (!text) {
		ska_set_error("ska_asset_read_text: Failed to allocate null terminator");
		ska_free(data);
		return false;
	}

	text[file_size] = '\0';
	*out_text = text;
	return true;
}

// ============================================================================
// Content URI Reader
// ============================================================================

bool ska_android_content_read(const char* uri_str, void** out_data, size_t* out_size) {
	if (!uri_str || !out_data) return false;

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env || !g_ska.android_context) {
		ska_set_error("ska_android_content_read: JNI or context not available");
		return false;
	}

	// Strip #fragment (display name hint) before parsing the URI
	char* clean_uri = ska_strdup(uri_str);
	char* hash = strchr(clean_uri, '#');
	if (hash) *hash = '\0';

	// Uri.parse(clean_uri)
	jstring j_uri_str = (*env)->NewStringUTF(env, clean_uri);
	ska_free(clean_uri);
	jobject uri = (*env)->CallStaticObjectMethod(env,
		g_jni_cache.uri_class, g_jni_cache.uri_parse, j_uri_str);
	(*env)->DeleteLocalRef(env, j_uri_str);

	// context.getContentResolver()
	jobject resolver = (*env)->CallObjectMethod(env,
		(jobject)g_ska.android_context, g_jni_cache.ctx_getContentResolver);

	// resolver.openFileDescriptor(uri, "r")
	jclass    resolver_class = (*env)->GetObjectClass(env, resolver);
	jmethodID openFd         = (*env)->GetMethodID(env, resolver_class,
		"openFileDescriptor",
		"(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
	(*env)->DeleteLocalRef(env, resolver_class);
	jstring mode = (*env)->NewStringUTF(env, "r");
	jobject pfd  = (*env)->CallObjectMethod(env, resolver, openFd, uri, mode);
	(*env)->DeleteLocalRef(env, mode);
	(*env)->DeleteLocalRef(env, uri);
	(*env)->DeleteLocalRef(env, resolver);

	if (!pfd || (*env)->ExceptionCheck(env)) {
		if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
		ska_set_error("ska_android_content_read: Failed to open '%s'", uri_str);
		if (pfd) (*env)->DeleteLocalRef(env, pfd);
		return false;
	}

	// pfd.detachFd() — transfers ownership of the fd to us
	jclass    pfd_class = (*env)->GetObjectClass(env, pfd);
	jmethodID detachFd  = (*env)->GetMethodID(env, pfd_class, "detachFd", "()I");
	int       fd        = (*env)->CallIntMethod(env, pfd, detachFd);
	(*env)->DeleteLocalRef(env, pfd_class);
	(*env)->DeleteLocalRef(env, pfd);

	// Read via standard C I/O
	FILE* file = fdopen(fd, "rb");
	if (!file) {
		ska_set_error("ska_android_content_read: fdopen failed for '%s'", uri_str);
		close(fd);
		return false;
	}

	// Get size — SAF file descriptors are seekable
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (file_size < 0) {
		ska_set_error("ska_android_content_read: Failed to get size for '%s'", uri_str);
		fclose(file);
		return false;
	}

	if (file_size == 0) {
		fclose(file);
		*out_data = NULL;
		if (out_size) *out_size = 0;
		return true;
	}

	void* data = ska_malloc((size_t)file_size);
	if (!data) {
		ska_set_error("ska_android_content_read: Failed to allocate %ld bytes", file_size);
		fclose(file);
		return false;
	}

	size_t bytes_read = fread(data, 1, (size_t)file_size, file);
	fclose(file);

	if (bytes_read != (size_t)file_size) {
		ska_set_error("ska_android_content_read: Read %zu bytes, expected %ld", bytes_read, file_size);
		ska_free(data);
		return false;
	}

	*out_data = data;
	if (out_size) *out_size = (size_t)file_size;
	return true;
}

// ============================================================================
// File Dialog
// ============================================================================

// Pending file dialog state
typedef struct {
	ska_file_dialog_id_t id;
	char*                title;
	bool                 active;
} ska_android_file_dialog_t;

static ska_android_file_dialog_t g_android_file_dialog = {0};

static void JNICALL ska_native_on_activity_result(JNIEnv*, jclass, jint, jint, jobjectArray);

static void ska_file_dialog_jni_init(void) {
	if (g_file_dialog_jni.initialized) return;

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) return;

	// Intent class and methods
	jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
	g_file_dialog_jni.intent_class = (*env)->NewGlobalRef(env, intent_class);
	g_file_dialog_jni.intent_init = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;)V");
	g_file_dialog_jni.intent_setType = (*env)->GetMethodID(env, intent_class, "setType", "(Ljava/lang/String;)Landroid/content/Intent;");
	g_file_dialog_jni.intent_addCategory = (*env)->GetMethodID(env, intent_class, "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;");
	g_file_dialog_jni.intent_putExtra_bool = (*env)->GetMethodID(env, intent_class, "putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;");
	(*env)->DeleteLocalRef(env, intent_class);

	// PackageManager
	jclass pm_class = (*env)->FindClass(env, "android/content/pm/PackageManager");
	g_file_dialog_jni.pm_class = (*env)->NewGlobalRef(env, pm_class);
	g_file_dialog_jni.pm_queryIntentActivities = (*env)->GetMethodID(env, pm_class, "queryIntentActivities", "(Landroid/content/Intent;I)Ljava/util/List;");
	(*env)->DeleteLocalRef(env, pm_class);

	// Activity methods
	jclass activity_class = (*env)->FindClass(env, "android/app/Activity");
	g_file_dialog_jni.activity_getPackageManager = (*env)->GetMethodID(env, activity_class, "getPackageManager", "()Landroid/content/pm/PackageManager;");

	// SkAppResultFragment — generic headless fragment that intercepts
	// onActivityResult. Works with any Activity, no SkAppActivity required.
	g_file_dialog_jni.fragment_available = false;
	jclass fragment_class = ska_android_find_app_class(env, "net.stereokit.sk_app.SkAppResultFragment");
	if (fragment_class) {
		g_file_dialog_jni.fragment_class = (*env)->NewGlobalRef(env, fragment_class);
		g_file_dialog_jni.fragment_launch = (*env)->GetStaticMethodID(env,
			fragment_class, "launch",
			"(Landroid/app/Activity;ILandroid/content/Intent;)V");

		// Register native callback so JVM can find it even when the
		// library was loaded via dlopen rather than System.loadLibrary.
		static const JNINativeMethod methods[] = {{
			"nativeOnActivityResult", "(II[Ljava/lang/String;)V",
			(void*)ska_native_on_activity_result
		}};
		(*env)->RegisterNatives(env, fragment_class, methods, 1);

		g_file_dialog_jni.fragment_available = true;
		(*env)->DeleteLocalRef(env, fragment_class);
	} else {
		ska_log(ska_log_warn, "SkAppResultFragment not found — "
			"file dialogs require sk_app Java classes");
	}
	(*env)->DeleteLocalRef(env, activity_class);

	g_file_dialog_jni.initialized = true;
}

bool ska_platform_file_dialog_available(ska_file_dialog_ type) {
	// File dialogs use Fragments + startActivityForResult — Activity only
	if (!g_ska.android_context || !g_ska.android_is_activity) {
		return false;
	}

	ska_file_dialog_jni_init();
	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env || !g_file_dialog_jni.initialized) {
		return false;
	}

	// Create the appropriate intent action string
	const char* action;
	switch (type) {
		case ska_file_dialog_open:
			action = "android.intent.action.OPEN_DOCUMENT";
			break;
		case ska_file_dialog_save:
			action = "android.intent.action.CREATE_DOCUMENT";
			break;
		case ska_file_dialog_open_folder:
			action = "android.intent.action.OPEN_DOCUMENT_TREE";
			break;
		default:
			return false;
	}

	// Create Intent
	jstring action_str = (*env)->NewStringUTF(env, action);
	jobject intent = (*env)->NewObject(env, g_file_dialog_jni.intent_class, g_file_dialog_jni.intent_init, action_str);
	(*env)->DeleteLocalRef(env, action_str);

	if (!intent) {
		return false;
	}

	// Set type to */* for general query
	jstring type_str = (*env)->NewStringUTF(env, "*/*");
	jobject ret_type = (*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_setType, type_str);
	(*env)->DeleteLocalRef(env, type_str);
	if (ret_type) (*env)->DeleteLocalRef(env, ret_type);

	// Add CATEGORY_OPENABLE
	jstring category_str = (*env)->NewStringUTF(env, "android.intent.category.OPENABLE");
	jobject ret_cat = (*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_addCategory, category_str);
	(*env)->DeleteLocalRef(env, category_str);
	if (ret_cat) (*env)->DeleteLocalRef(env, ret_cat);

	// Get PackageManager and query
	jobject activity = (jobject)g_ska.android_context;
	jobject pm = (*env)->CallObjectMethod(env, activity, g_file_dialog_jni.activity_getPackageManager);

	if (!pm) {
		(*env)->DeleteLocalRef(env, intent);
		return false;
	}

	// Query activities that can handle this intent
	jobject resolve_list = (*env)->CallObjectMethod(env, pm, g_file_dialog_jni.pm_queryIntentActivities, intent, 0);

	bool available = false;
	if (resolve_list) {
		// Check if list is not empty
		jclass list_class = (*env)->FindClass(env, "java/util/List");
		jmethodID size_method = (*env)->GetMethodID(env, list_class, "size", "()I");
		int size = (*env)->CallIntMethod(env, resolve_list, size_method);
		available = (size > 0);
		(*env)->DeleteLocalRef(env, list_class);
		(*env)->DeleteLocalRef(env, resolve_list);
	}

	(*env)->DeleteLocalRef(env, pm);
	(*env)->DeleteLocalRef(env, intent);

	return available;
}

bool ska_platform_file_dialog_show(ska_file_dialog_id_t id, const ska_file_dialog_request_t* request) {
	// Launch file picker via SkAppResultFragment (headless fragment that
	// receives onActivityResult and forwards it to native code). Works with
	// any Activity — no SkAppActivity subclass required.

	if (!g_ska.android_is_activity) {
		ska_set_error("File dialogs require an Activity context");
		return false;
	}

	if (g_android_file_dialog.active) {
		ska_set_error("File dialog already active");
		return false;
	}

	ska_file_dialog_jni_init();
	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env || !g_file_dialog_jni.initialized) {
		ska_set_error("JNI not initialized for file dialog");
		return false;
	}

	if (!g_file_dialog_jni.fragment_available) {
		ska_set_error("SkAppResultFragment not available — cannot show file dialog");
		return false;
	}

	// Determine intent action
	const char* action;
	switch (request->type) {
		case ska_file_dialog_open:
			action = "android.intent.action.OPEN_DOCUMENT";
			break;
		case ska_file_dialog_save:
			action = "android.intent.action.CREATE_DOCUMENT";
			break;
		case ska_file_dialog_open_folder:
			action = "android.intent.action.OPEN_DOCUMENT_TREE";
			break;
		default:
			ska_set_error("Unknown file dialog type");
			return false;
	}

	// Create Intent
	jstring action_str = (*env)->NewStringUTF(env, action);
	jobject intent = (*env)->NewObject(env, g_file_dialog_jni.intent_class, g_file_dialog_jni.intent_init, action_str);
	(*env)->DeleteLocalRef(env, action_str);

	if (!intent) {
		ska_set_error("Failed to create file picker intent");
		return false;
	}

	// Set MIME type
	const char* mime_type = "*/*";
	if (request->filters && request->filter_count > 0) {
		// Use first filter's MIME type
		mime_type = ska_filter_get_mime(&request->filters[0]);
	}
	jstring type_str = (*env)->NewStringUTF(env, mime_type);
	jobject ret = (*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_setType, type_str);
	(*env)->DeleteLocalRef(env, type_str);
	if (ret) (*env)->DeleteLocalRef(env, ret);

	// Add CATEGORY_OPENABLE for file intents (not folder)
	if (request->type != ska_file_dialog_open_folder) {
		jstring category_str = (*env)->NewStringUTF(env, "android.intent.category.OPENABLE");
		ret = (*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_addCategory, category_str);
		(*env)->DeleteLocalRef(env, category_str);
		if (ret) (*env)->DeleteLocalRef(env, ret);
	}

	// Allow multiple selection if requested
	if (request->allow_multiple && request->type == ska_file_dialog_open) {
		jstring extra_str = (*env)->NewStringUTF(env, "android.intent.extra.ALLOW_MULTIPLE");
		ret = (*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_putExtra_bool, extra_str, JNI_TRUE);
		(*env)->DeleteLocalRef(env, extra_str);
		if (ret) (*env)->DeleteLocalRef(env, ret);
	}

	// Allocate result entry (must be done before launching activity so JNI callback can find it)
	ska_file_dialog_result_t* result = ska_file_dialog_result_alloc(id, request->title);
	if (!result) {
		(*env)->DeleteLocalRef(env, intent);
		ska_set_error("Failed to allocate file dialog result");
		return false;
	}

	// Store state
	g_android_file_dialog.id = id;
	g_android_file_dialog.title = request->title ? ska_strdup(request->title) : NULL;
	g_android_file_dialog.active = true;

	// Launch via headless SkAppResultFragment
	jint request_id = 0x5B00 | (id & 0xFF);
	jobject activity = (jobject)g_ska.android_context;
	(*env)->CallStaticVoidMethod(env, g_file_dialog_jni.fragment_class,
		g_file_dialog_jni.fragment_launch, activity, request_id, intent);
	(*env)->DeleteLocalRef(env, intent);

	// Check for exception
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		g_android_file_dialog.active = false;
		if (g_android_file_dialog.title) {
			ska_free(g_android_file_dialog.title);
			g_android_file_dialog.title = NULL;
		}
		ska_set_error("Failed to start file picker activity");
		return false;
	}

	ska_log(ska_log_info, "File dialog launched (id=%u)", id);

	return true;
}

// ============================================================================
// JNI Callbacks for Activity Results
// ============================================================================
// Note: underscore in package name "sk_app" is escaped as "_1" in JNI.

// File dialog result handler (request IDs with 0x5B00 prefix)
static void ska_android_file_dialog_handle_result(
	JNIEnv* env, jint dialog_id, jobjectArray uris, jboolean cancelled)
{
	ska_log(ska_log_info, "File dialog result: dialog_id=%d, cancelled=%d", dialog_id, cancelled);

	// Find the pending result for this dialog
	ska_file_dialog_result_t* result = NULL;
	for (int32_t i = 0; i < g_ska_file_dialog.result_count; i++) {
		if (g_ska_file_dialog.results[i].id == (ska_file_dialog_id_t)dialog_id) {
			result = &g_ska_file_dialog.results[i];
			break;
		}
	}

	if (!result) {
		ska_log(ska_log_warn, "File dialog result for unknown dialog ID %d", dialog_id);
		return;
	}

	// Process URIs if not cancelled. For content:// URIs, query the display
	// name and append it as a fragment (content://...#filename.ext) so callers
	// can detect the file type from the extension.
	if (!cancelled && uris != NULL) {
		// Set up JNI refs for display name query
		jobject   resolver   = (*env)->CallObjectMethod(env,
			(jobject)g_ska.android_context, g_jni_cache.ctx_getContentResolver);
		jclass    cr_class   = (*env)->GetObjectClass(env, resolver);
		jmethodID cr_query   = (*env)->GetMethodID(env, cr_class, "query",
			"(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;"
			"[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");
		(*env)->DeleteLocalRef(env, cr_class);
		jclass    cursor_cls = (*env)->FindClass(env, "android/database/Cursor");
		jmethodID cur_move   = (*env)->GetMethodID(env, cursor_cls, "moveToFirst", "()Z");
		jmethodID cur_getIdx = (*env)->GetMethodID(env, cursor_cls, "getColumnIndex",
			"(Ljava/lang/String;)I");
		jmethodID cur_getStr = (*env)->GetMethodID(env, cursor_cls, "getString",
			"(I)Ljava/lang/String;");
		jmethodID cur_close  = (*env)->GetMethodID(env, cursor_cls, "close", "()V");
		(*env)->DeleteLocalRef(env, cursor_cls);

		jstring col_name = (*env)->NewStringUTF(env, "_display_name");
		jclass  string_cls = (*env)->FindClass(env, "java/lang/String");
		jobjectArray projection = (*env)->NewObjectArray(env, 1, string_cls, col_name);
		(*env)->DeleteLocalRef(env, string_cls);

		jsize uri_count = (*env)->GetArrayLength(env, uris);
		for (jsize i = 0; i < uri_count; i++) {
			jstring uri_jstr = (jstring)(*env)->GetObjectArrayElement(env, uris, i);
			if (!uri_jstr) continue;

			const char* uri_utf = (*env)->GetStringUTFChars(env, uri_jstr, NULL);
			if (!uri_utf) { (*env)->DeleteLocalRef(env, uri_jstr); continue; }

			// Query display name for content:// URIs
			char* path = NULL;
			if (strncmp(uri_utf, "content://", 10) == 0) {
				jobject parsed = (*env)->CallStaticObjectMethod(env,
					g_jni_cache.uri_class, g_jni_cache.uri_parse, uri_jstr);

				if (!parsed || (*env)->ExceptionCheck(env)) {
					if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
					if (parsed) (*env)->DeleteLocalRef(env, parsed);
					goto add_path;
				}

				jobject cursor = (*env)->CallObjectMethod(env, resolver, cr_query,
					parsed, projection, NULL, NULL, NULL);

				if (cursor) {
					if (!(*env)->ExceptionCheck(env)) {
						if ((*env)->CallBooleanMethod(env, cursor, cur_move)) {
							jint col = (*env)->CallIntMethod(env, cursor, cur_getIdx, col_name);
							if (col >= 0) {
								jstring name_jstr = (jstring)(*env)->CallObjectMethod(env, cursor, cur_getStr, col);
								if (name_jstr) {
									const char* name = (*env)->GetStringUTFChars(env, name_jstr, NULL);
									if (name) {
										// content://provider/doc/id#display_name.ext
										size_t uri_len  = strlen(uri_utf);
										size_t name_len = strlen(name);
										path = (char*)ska_malloc(uri_len + 1 + name_len + 1);
										if (path) {
											memcpy(path, uri_utf, uri_len);
											path[uri_len] = '#';
											memcpy(path + uri_len + 1, name, name_len + 1);
										}
										(*env)->ReleaseStringUTFChars(env, name_jstr, name);
									}
									(*env)->DeleteLocalRef(env, name_jstr);
								}
							}
						}
					} else {
						(*env)->ExceptionClear(env);
					}
					(*env)->CallVoidMethod(env, cursor, cur_close);
					(*env)->DeleteLocalRef(env, cursor);
				} else {
					if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
				}
				(*env)->DeleteLocalRef(env, parsed);
			}

		add_path:
			ska_file_dialog_result_add_path(result, path ? path : uri_utf);
			if (path) ska_free(path);

			(*env)->ReleaseStringUTFChars(env, uri_jstr, uri_utf);
			(*env)->DeleteLocalRef(env, uri_jstr);
		}

		(*env)->DeleteLocalRef(env, col_name);
		(*env)->DeleteLocalRef(env, projection);
		(*env)->DeleteLocalRef(env, resolver);
	}

	// Mark complete and post event
	ska_file_dialog_result_complete(result, cancelled);

	// Clear pending state
	g_android_file_dialog.active = false;
	if (g_android_file_dialog.title) {
		ska_free(g_android_file_dialog.title);
		g_android_file_dialog.title = NULL;
	}

	ska_log(ska_log_info, "File dialog completed: %d paths, cancelled=%d",
		result->path_count, result->cancelled);
}

// Dispatch activity result based on request ID prefix.
// RESULT_OK = -1 on Android.
static void ska_android_dispatch_activity_result(
	JNIEnv* env, jint request_id, jint result_code, jobjectArray uris)
{
	// File dialog: 0x5B00 prefix
	if ((request_id & 0xFF00) == 0x5B00) {
		jint dialog_id = request_id & 0xFF;
		jboolean cancelled = (result_code != -1);
		ska_android_file_dialog_handle_result(env, dialog_id, uris, cancelled);
		return;
	}

	ska_log(ska_log_warn, "Unhandled activity result: request_id=0x%04x, result=%d",
		request_id, result_code);
}

// Called from SkAppResultFragment (generic headless fragment). Forward-declared
// so RegisterNatives can bind it during init — required when the library is
// loaded via dlopen (e.g. .NET Android P/Invoke) rather than
// System.loadLibrary.
static void JNICALL
ska_native_on_activity_result(
	JNIEnv* env, jclass clazz, jint request_id, jint result_code, jobjectArray uris)
{
	(void)clazz;
	ska_android_dispatch_activity_result(env, request_id, result_code, uris);
}

// ============================================================================
// KVP Store (Android: SharedPreferences with Base64 encoding)
// ============================================================================

static bool ska_kvpstore_jni_init(void) {
	if (g_kvp_jni.initialized) return true;

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env || !g_ska.android_context) return false;

	// Context.getSharedPreferences(String name, int mode)
	jclass ctx_class = (*env)->GetObjectClass(env, (jobject)g_ska.android_context);
	g_kvp_jni.ctx_getSharedPreferences = (*env)->GetMethodID(
		env, ctx_class, "getSharedPreferences",
		"(Ljava/lang/String;I)Landroid/content/SharedPreferences;"
	);
	(*env)->DeleteLocalRef(env, ctx_class);

	// SharedPreferences methods
	jclass prefs_class = (*env)->FindClass(env, "android/content/SharedPreferences");
	g_kvp_jni.prefs_getString = (*env)->GetMethodID(
		env, prefs_class, "getString",
		"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
	);
	g_kvp_jni.prefs_edit = (*env)->GetMethodID(
		env, prefs_class, "edit",
		"()Landroid/content/SharedPreferences$Editor;"
	);
	(*env)->DeleteLocalRef(env, prefs_class);

	// SharedPreferences.Editor methods
	jclass editor_class = (*env)->FindClass(env, "android/content/SharedPreferences$Editor");
	g_kvp_jni.editor_putString = (*env)->GetMethodID(
		env, editor_class, "putString",
		"(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;"
	);
	g_kvp_jni.editor_remove = (*env)->GetMethodID(
		env, editor_class, "remove",
		"(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;"
	);
	g_kvp_jni.editor_apply = (*env)->GetMethodID(
		env, editor_class, "apply", "()V"
	);
	(*env)->DeleteLocalRef(env, editor_class);

	// Base64 for encoding binary data
	jclass base64_local = (*env)->FindClass(env, "android/util/Base64");
	g_kvp_jni.base64_class = (*env)->NewGlobalRef(env, base64_local);
	(*env)->DeleteLocalRef(env, base64_local);

	g_kvp_jni.base64_encodeToString = (*env)->GetStaticMethodID(
		env, g_kvp_jni.base64_class, "encodeToString",
		"([BI)Ljava/lang/String;"
	);
	g_kvp_jni.base64_decode = (*env)->GetStaticMethodID(
		env, g_kvp_jni.base64_class, "decode",
		"(Ljava/lang/String;I)[B"
	);

	g_kvp_jni.initialized = true;
	return true;
}

SKA_API bool ska_kvpstore_save(const char* key, const void* data, size_t size) {
	if (!ska_kvpstore_validate_key(key)) return false;
	if (!data && size > 0) {
		ska_set_error("ska_kvpstore_save: NULL data with non-zero size");
		return false;
	}

	if (!g_ska.android_context) {
		ska_set_error("ska_kvpstore: android activity not available");
		return false;
	}

	if (!ska_kvpstore_jni_init()) {
		ska_set_error("ska_kvpstore: JNI initialization failed");
		return false;
	}

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) return false;

	jobject activity = (jobject)g_ska.android_context;

	// Get SharedPreferences
	jstring prefs_name = (*env)->NewStringUTF(env, ska_kvpstore_get_app_name());
	jobject prefs = (*env)->CallObjectMethod(env, activity,
		g_kvp_jni.ctx_getSharedPreferences, prefs_name, 0 /* MODE_PRIVATE */);

	// Encode data to Base64
	jbyteArray byte_array = (*env)->NewByteArray(env, (jsize)size);
	if (data && size > 0) {
		(*env)->SetByteArrayRegion(env, byte_array, 0, (jsize)size, (const jbyte*)data);
	}

	jstring encoded = (*env)->CallStaticObjectMethod(env, g_kvp_jni.base64_class,
		g_kvp_jni.base64_encodeToString, byte_array, 0 /* DEFAULT */);

	// Save to SharedPreferences
	jstring jkey = (*env)->NewStringUTF(env, key);
	jobject editor = (*env)->CallObjectMethod(env, prefs, g_kvp_jni.prefs_edit);
	jobject editor_ret = (*env)->CallObjectMethod(env, editor, g_kvp_jni.editor_putString, jkey, encoded);
	if (editor_ret) (*env)->DeleteLocalRef(env, editor_ret);
	(*env)->CallVoidMethod(env, editor, g_kvp_jni.editor_apply);

	// Cleanup local refs
	(*env)->DeleteLocalRef(env, prefs_name);
	(*env)->DeleteLocalRef(env, prefs);
	(*env)->DeleteLocalRef(env, byte_array);
	(*env)->DeleteLocalRef(env, encoded);
	(*env)->DeleteLocalRef(env, jkey);
	(*env)->DeleteLocalRef(env, editor);

	return true;
}

SKA_API bool ska_kvpstore_load(const char* key, void* opt_buffer, size_t buffer_size, size_t* opt_out_size) {
	if (!ska_kvpstore_validate_key(key)) return false;

	if (!g_ska.android_context) {
		ska_set_error("ska_kvpstore: android activity not available");
		return false;
	}

	if (!ska_kvpstore_jni_init()) {
		ska_set_error("ska_kvpstore: JNI initialization failed");
		return false;
	}

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) return false;

	jobject activity = (jobject)g_ska.android_context;

	// Get SharedPreferences
	jstring prefs_name = (*env)->NewStringUTF(env, ska_kvpstore_get_app_name());
	jobject prefs = (*env)->CallObjectMethod(env, activity,
		g_kvp_jni.ctx_getSharedPreferences, prefs_name, 0);

	// Get stored string
	jstring jkey = (*env)->NewStringUTF(env, key);
	jstring encoded = (*env)->CallObjectMethod(env, prefs, g_kvp_jni.prefs_getString, jkey, NULL);

	if (!encoded) {
		(*env)->DeleteLocalRef(env, prefs_name);
		(*env)->DeleteLocalRef(env, prefs);
		(*env)->DeleteLocalRef(env, jkey);
		return false;
	}

	// Decode Base64
	jbyteArray decoded = (*env)->CallStaticObjectMethod(env, g_kvp_jni.base64_class,
		g_kvp_jni.base64_decode, encoded, 0);

	jsize data_size = (*env)->GetArrayLength(env, decoded);

	if (opt_out_size) {
		*opt_out_size = (size_t)data_size;
	}

	// Copy to buffer if provided
	if (opt_buffer && buffer_size > 0) {
		jsize copy_size = (buffer_size < (size_t)data_size) ? (jsize)buffer_size : data_size;
		(*env)->GetByteArrayRegion(env, decoded, 0, copy_size, (jbyte*)opt_buffer);
	}

	// Cleanup
	(*env)->DeleteLocalRef(env, prefs_name);
	(*env)->DeleteLocalRef(env, prefs);
	(*env)->DeleteLocalRef(env, jkey);
	(*env)->DeleteLocalRef(env, encoded);
	(*env)->DeleteLocalRef(env, decoded);

	return true;
}

SKA_API bool ska_kvpstore_delete(const char* key) {
	if (!ska_kvpstore_validate_key(key)) return false;

	if (!g_ska.android_context) {
		ska_set_error("ska_kvpstore: android activity not available");
		return false;
	}

	if (!ska_kvpstore_jni_init()) {
		ska_set_error("ska_kvpstore: JNI initialization failed");
		return false;
	}

	JNIEnv* env = (JNIEnv*)ska_android_get_jni_env();
	if (!env) return false;

	jobject activity = (jobject)g_ska.android_context;

	// Get SharedPreferences
	jstring prefs_name = (*env)->NewStringUTF(env, ska_kvpstore_get_app_name());
	jobject prefs = (*env)->CallObjectMethod(env, activity,
		g_kvp_jni.ctx_getSharedPreferences, prefs_name, 0);

	// Remove key
	jstring jkey = (*env)->NewStringUTF(env, key);
	jobject editor = (*env)->CallObjectMethod(env, prefs, g_kvp_jni.prefs_edit);
	jobject editor_ret = (*env)->CallObjectMethod(env, editor, g_kvp_jni.editor_remove, jkey);
	if (editor_ret) (*env)->DeleteLocalRef(env, editor_ret);
	(*env)->CallVoidMethod(env, editor, g_kvp_jni.editor_apply);

	// Cleanup
	(*env)->DeleteLocalRef(env, prefs_name);
	(*env)->DeleteLocalRef(env, prefs);
	(*env)->DeleteLocalRef(env, jkey);
	(*env)->DeleteLocalRef(env, editor);

	return true;
}

#endif // SKA_PLATFORM_ANDROID
