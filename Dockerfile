# Reproducible build environment for Dream, mirroring F-Droid's builder
# (Debian trixie + OpenJDK 21) with the project's exact pinned Android toolchain.
FROM debian:trixie-slim

ENV ANDROID_HOME=/opt/android-sdk \
      JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
      DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
      && apt-get install -y --no-install-recommends \
          openjdk-21-jdk-headless unzip curl ca-certificates \
      && rm -rf /var/lib/apt/lists/*

# Android command-line tools (installer front-end only; any recent build works).
# At some point this number will become out of date and the file will break.
# Get the current build number from https://developer.android.com/studio#command-line-tools
# If the URL is out of date as well, then good luck - you're on your own, future dweller.
ARG CMDLINE_TOOLS_VER=14742923
RUN mkdir -p "$ANDROID_HOME/cmdline-tools" \
      && curl -fsSL "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS_VER}_latest.zip" -o /tmp/clt.zip \
      && unzip -q /tmp/clt.zip -d "$ANDROID_HOME/cmdline-tools" \
      && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest" \
      && rm /tmp/clt.zip
ENV PATH="$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools"

# Pinned SDK / NDK, matching app/build.gradle exactly.
RUN yes | sdkmanager --licenses >/dev/null \
      && sdkmanager \
          "platform-tools" \
          "platforms;android-36" \
          "build-tools;36.0.0" \
          "ndk;27.2.12479018" \
          "cmake;3.22.1"

WORKDIR /project