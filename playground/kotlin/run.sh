#!/bin/sh
# Build libioma.so, compile the example against JDK 22+ (java.lang.foreign is final there), run it.
# Needs kotlinc and a JDK 22+ on PATH (or set JAVA_HOME); tested on JDK 26.0.2.1. IOMA_WORKERS / IOMA_PORT override the defaults.
set -e
cd "$(dirname "$0")"
make -s -C ../.. lib
kotlinc Server.kt -include-runtime -d server.jar
exec java --enable-native-access=ALL-UNNAMED -jar server.jar ../../libioma.so
