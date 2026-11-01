#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
#cool 🐥
if [ ! -f debug.keystore ]; then
  keytool -genkeypair -keystore debug.keystore -alias k -keyalg RSA -keysize 2048 \
          -validity 10000 -storepass android -keypass android \
          -dname "CN=vivo cfg,O=NUB" >/dev/null 2>&1
fi

echo "=== Building VivoFrameworksResRRO.apk ==="
rm -rf VivoFrameworksResRRO/build VivoFrameworksResRRO.unsigned.apk VivoFrameworksResRRO.aligned.apk VivoFrameworksResRRO.apk
apktool b VivoFrameworksResRRO -o VivoFrameworksResRRO.unsigned.apk
zipalign -f -p 4 VivoFrameworksResRRO.unsigned.apk VivoFrameworksResRRO.aligned.apk
apksigner sign --ks debug.keystore --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
    --out VivoFrameworksResRRO.apk VivoFrameworksResRRO.aligned.apk
rm -f VivoFrameworksResRRO.unsigned.apk VivoFrameworksResRRO.aligned.apk
echo "Built VivoFrameworksResRRO.apk (md5: $(md5sum VivoFrameworksResRRO.apk | awk '{print $1}'))"
