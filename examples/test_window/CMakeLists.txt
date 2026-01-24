# Test Window - sk_app Test Suite

add_skapp(test_window
	PACKAGE_NAME net.stereokit.test_window
	APP_NAME "Test Window"
)

target_sources(test_window PRIVATE
	test_window.c
)
