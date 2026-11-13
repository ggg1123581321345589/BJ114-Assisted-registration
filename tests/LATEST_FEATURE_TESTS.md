# New feature offline tests

These tests never read the real application session. Session-store fixtures use synthetic cookies and explicit QTemporaryDir paths. The GUI fixture uses Window(root, false, false), disconnects all query workers before processing events, disables background pool recovery, and links offline_broker_guard.cpp so accidental transport fails closed.

Run in a Windows x64 MSVC developer prompt with Qt5 qmake, moc and DLLs available. Build directories are inside the repository to keep project paths relative.

## Session store

From the repository root:

~~~cmd
mkdir build-tests-features\session_store_offline_test
cd build-tests-features\session_store_offline_test
qmake ..\..\tests\latest-features.pro TEST_NAME=session_store_offline_test
nmake
bin\session_store_offline_test.exe
~~~

The Windows run has 57 assertions: cookie allowlist/limits/normalization, CurrentUser DPAPI roundtrip, no plaintext cookie fields in ciphertext, atomic replacement, corruption rejection, and explicit temporary-file removal. Other platforms check persistence fails closed.

## Incremental refresh

From the repository root:

~~~cmd
mkdir build-tests-features\smooth_refresh_offline_test\generated
cd build-tests-features\smooth_refresh_offline_test
moc ..\..\src\main.cpp -o generated\main.moc
qmake ..\..\tests\latest-features.pro TEST_NAME=smooth_refresh_offline_test
nmake
set QT_QPA_PLATFORM=windows
bin\smooth_refresh_offline_test.exe ..\..
~~~

The GUI run has 50 assertions: local catalog metadata, tagged synthetic completions, stable table item identity, latest-selection merging, stale-row booking controls, sort/filter/scroll/selection preservation, duplicate payloads, stop/close cleanup and two real Windows render sizes. Synthetic patient/card labels and screenshots are test-only. Screenshots are written to the build directory. No login, SMS, hospital query or order is sent.

Production source is unchanged. Both fixtures emit JSON assertion results and exit nonzero on failure.
