AoQ Free Flip

A mod for Attack on Quest (0.5.0) that lets you flip in any direction while you're in the air. Push the right thumbstick and your whole body rotates: forward/back for front and back flips, left/right for barrel rolls. Mix them and you can spin whichever way you like.

Tested on the headset and it works.


WHAT IT DOES

- While airborne, the right stick rotates you around your head, so the view spins in place instead of swinging around your feet.
- The game's normal right-stick turning is blocked while you flip, so you don't snap-turn at the same time.
- When you land, you get smoothly levelled back upright.


SETTINGS

Open Mods -> Configure Mods -> Free Flip in the game:

Enabled - turns the mod on/off (default: on)
FlipSpeed - spin speed at full stick, degrees per second (default: 300)
Deadzone - how far you have to push the stick before it counts, 0.05 to 0.9 (default: 0.25)
InvertPitch - swaps front/back flip direction (default: off)
InvertRoll - swaps left/right roll direction (default: off)
RollEnabled - left/right rolls you. Off = front/back flips only (default: on)
RequireAir - only flip while off the ground. Off = flip anywhere (default: on)
LevelOnLand - return upright after landing (default: on)
LevelSpeed - how fast you right yourself, degrees per second (default: 540)

Changes get picked up within a couple of seconds, no restart needed.


HOW IT WORKS

It's a native mod for the AoQ-Modloader (github.com/Treyo1928/AoQ-Modloader) and hooks two game functions:

1. OVRPlayerController.Update is used as the per-frame tick. Each frame it checks whether you're airborne (CharacterController.isGrounded), reads the right stick, and rotates the player's transform around the head position. The game's own "Rolling" coroutine already rotates this same transform, so rotating it is something the game already supports.

2. OVRInput.Get(Axis2D) is hooked so the mod can read the raw stick, then report zero to the game while you're flipping. That's what stops the snap-turn.

While you're tilted the mod also switches off HmdRotatesY, because otherwise the game rewrites your rotation from your head yaw every frame. It's switched back on when you've levelled out.

Field offsets (Controller, CameraRig, HmdRotatesY) are looked up by name at runtime through the il2cpp API rather than hardcoded.


BUILDING

You need the Android NDK (I used r30) and this folder sitting next to the repo's shared and modmanager folders, since the .mk file points at ../shared and ../modmanager.

AoQ-Modloader-master/
    shared/
    modmanager/
    mod-freeflip/

From inside mod-freeflip in PowerShell:

& "<path-to-ndk>\ndk-build.cmd" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk NDK_APPLICATION_MK=./Application.mk

That gives you libs/armeabi-v7a/libaoqfreeflip.so. (The two strtok warnings from inlineHook.c are harmless.)

build.sh does the build and push in one go if you're on Linux/macOS/WSL.


INSTALLING

adb push libs\armeabi-v7a\libaoqfreeflip.so /sdcard/DCIM/AoQMods/mods/libaoqfreeflip.so
adb shell am force-stop com.AoQ.AttackOnQuest

Or just copy the .so into DCIM/AoQMods/mods/ on the headset over USB. Then relaunch the game.


DEBUGGING

adb logcat -s AoQFreeFlip,QuestHook,AoQModManager

What you should see: "AoQ Free Flip loading...", then "hooks installed", and once you're in a level a line with the resolved field offsets. Flipping logs "flipping (air=1)".

No adb? Logs are also in DCIM/AoQMods/logs/latest.log on the headset (plus a crash-<time>.log if the game crashes).


FILES

main.c - the mod itself
Android.mk / Application.mk - NDK build config (targets armeabi-v7a, android-24)
build.sh - build + push + restart script
