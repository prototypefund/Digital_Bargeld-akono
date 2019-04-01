Building node for Android isn't officially supported, so there are some difficulties:

* Node tries to build v8 via gyp, but the gyp build is broken for cross-compiling to Android.
  Also, the V8 team doesn't maintain the gyp build files anymore.  Only the gn build (gn is Chromium's build
  tool) works.
* The --build-v8-with-gn option for node is also broken.


We build node with the --without-bundled-v8 option.  This is necessary since we
manually have to compile the bundled v8.


Resources:
* https://v8.dev/docs/cross-compile-arm
