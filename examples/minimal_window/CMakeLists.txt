# Minimal Window - Bare-bones sk_app example

add_skapp(minimal_window
	PACKAGE_NAME net.stereokit.minimal_window
	APP_NAME "Minimal Window"
)

target_sources(minimal_window PRIVATE
	main.c
)
