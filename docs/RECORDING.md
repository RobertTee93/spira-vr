# Record a clean gameplay video

Record **SteamVR's spectator window**, which shows the submitted headset view.
The FFX desktop window contains a widened intermediate projection and can look
stretched; it is not the right source for a demonstration.

## SteamVR

1. Connect the headset, start SteamVR, and launch SpiraVR.
2. In the small SteamVR status window, open the menu and choose
   **Display VR View**.
3. In VR View's view/eye menu, select **Left Eye** or **Right Eye**. Avoid a
   both-eyes layout or a combined view with a seam.
4. Enlarge the window. Wear the headset, load your scene, and press F8 facing
   forward with FFX focused.

The Display VR View command was checked in SteamVR 2.18.4. Labels and the eye
selector's placement can vary with SteamVR versions. If the image freezes while
the headset is off, wake/wear it; SpiraVR pauses submissions during headset standby.

## OBS Studio

Use the free [OBS Studio](https://obsproject.com/).

1. Add a **Window Capture** source and select the **VR View** window, rather
   than FFX. Turn off **Capture Cursor**.
2. Set the canvas/output to 1920 x 1080 and 30 FPS as a starting point. The
   game still has its original frame-rate cap; recording at 60 FPS does not
   make it render 60 unique frames.
3. Right-click the source, then **Transform > Fit to Screen**. This preserves
   proportions and may leave side bars. Do not use Stretch to Screen.
4. If you prefer an edge-to-edge 16:9 image, crop the single-eye source to
   16:9, then fit it to the canvas. Hold Alt while dragging the source edges
   or use **Edit Transform**. A centred crop removes some vertical view, so
   check the menus before recording. Keeping bars is safer for full UI visibility.
5. Capture the audio output carrying FFX and check the meter. Mute unused
   microphone/desktop sources and avoid capturing the same audio twice.
6. Record to MKV. Make a ten-second sample, stop, and play it back to confirm
   the view, aspect ratio, cursor visibility, and sound. Remux to MP4 in OBS
   afterwards if your editor or upload site needs it.

If Window Capture is black, try its Windows capture method and keep VR View
unminimized. Capture only the intended window to avoid including other apps.

Official OBS references: [Window Capture](https://obsproject.com/kb/window-capture-sources)
and [source sizing/cropping](https://obsproject.com/kb/sources-guide).

## A useful 30–45 second demonstration

- Start in a quiet field with the character stationary. Turn your head slowly
  left and right to show that the view comes from inside the scene.
- Walk a few steps with the controller.
- Open a short dialogue or menu to show the forward UI panel.
- Show a battle target indicator while making one slow head turn.

Keep world stereo off for the recommended presentation, and avoid rapid head
motion. This is a normal flat video of your headset view, not a 360-degree video.
Two tester-provided examples are available in the README's
[Gameplay videos](../README.md#gameplay-videos) section. The MP4s are stored
unchanged in `media/` and are not included in the runtime release ZIP.
