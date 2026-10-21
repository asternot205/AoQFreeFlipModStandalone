#!/bin/bash
# Run from inside the mod-freeflip folder (next to ../shared and ../modmanager).
set -e
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk NDK_APPLICATION_MK=./Application.mk
adb push libs/armeabi-v7a/libaoqfreeflip.so /sdcard/DCIM/AoQMods/mods/libaoqfreeflip.so
adb shell am force-stop com.AoQ.AttackOnQuest
echo "Pushed. Launch the game, then: adb logcat -s AoQFreeFlip,QuestHook,AoQModManager"
