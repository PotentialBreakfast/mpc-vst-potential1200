# FAQ and troubleshooting

Can't find your answer? [Ask in an issue](../../issues/new/choose). There are no silly questions.

## Installing

**The plugin isn't in my plugin list.**
- **MPC OS 3.10.1+:** check **Preferences → Enhancements (Beta)**: **Enable Enhancements** and **External Plugin Support** must both be ticked, and the plugin's folder must sit directly inside **VST2 Plugins** at the top of the drive.
- **Restart:** the list only updates after the MPC app restarts, so turn the MPC off and on if in doubt.
- **The wrong list:** Potential1200 is an **instrument**: make a plugin track and look in the plugin list.
- **The MPC isn't set up for third-party plugins yet:** see [Before you start](INSTALL-GUIDE.md#before-you-start-is-your-mpc-ready-for-plugins).
- **After a firmware update:** an update can switch plugin loading off again. Redo the set-up from [Open MPC](https://open-mpc.github.io/).

**It's in the list but only shows "Load Plugin" or a plain page of sliders.**
The plugin folder is on a drive the MPC can't run programs from, or isn't in a folder the MPC knows about. Reinstall with the [MPC Plugin Installer](INSTALL-GUIDE.md#way-1-the-easy-way-the-mpc-plugin-installer-app), which picks a suitable place.

**The installer says "No space left on device" or "settings edit failed".**
The MPC's internal storage is full. A common cause is the MPC's own cache of plugin graphics in `/var/tmp/filmstrips`, which can grow to gigabytes. With the MPC app stopped, deleting `/var/tmp/filmstrips/temp_*` is safe, because the MPC makes new ones when it needs them.

**Which file do I download?**
On **MPC OS 3.10.1 or later**: the one ending **`-akai-vst2.zip`**. On older MPC OS 3.x with the community set-up: the one ending **`-mpc-armv7.zip`**. The "Source code" links are for developers.

**Will it work on my MPC One+, Live III or Key 37?**
Yes, on **MPC OS 3.10.1 or later**, with the **`-akai-vst2.zip`**: it has builds for 32-bit and 64-bit MPCs. The 64-bit one hasn't been tried on a real Gen2 MPC yet, so please [tell me how it goes](../../issues/new/choose).

## Potential1200

**The KIT page says "NO SOUNDS FOUND".**
It looks for one-shots in this order:
- a folder called `DRUM ONE SHOTS` at the top of a drive (**MPC TOOLS → Plugins → MAKE DRUM FOLDER** makes one from every sound on your MPC)
- folders listed in `Potential1200/folders.txt` on a drive, one per line
- the drum sounds in your **Expansions** and **Samples** folders

Then tap **RESCAN** on the SETUP page. **WAV**, **AIFF**, **FLAC** and **MP3** files are read.

**BREAK says "NO BREAKS FOUND".**
Put your breaks in a folder called `DRUM BREAKS LIBRARY` at the top of a drive (or any top-level folder with **break** in its name), then **RESCAN**.

**A pad's type is wrong (a snare on the CLAP pad).**
When there are no sounds of a type, the nearest type is used. Use the pad's arrows to pick another sound, or add more sounds to your drum folder.

**Tuning a pad up makes it shorter.**
That's the sound: pitch changes speed, as on the original. Turn **STRETCH** on for that pad (PROGRAM page) to keep its length.

**It sounds too clean / too dirty.**
Try **SAMPLE SPEED 45 > 33** on SETUP for more crunch, or the **FILTER** fader. **CHARACTER: CLEAN** turns the 12-bit sound off completely, to compare.

**Only one hit of a pad plays at a time / it's mono.**
Like the original: one voice per pad, and a mono mix.

**Pad 1 doesn't play pad 1.**
The MPC treats a plugin track like a keyboard, so the pad bank and octave decide which note a pad sends. The plugin listens to notes 36-51 (and 0-15).

**Where does SAVE KIT put the program?**
In `Potential1200/Kits` on your sounds drive, as an `.xpm` drum program with its WAVs. Load it from the MPC's browser. It needs MPC OS 3.10.1 or later.

## Everything else

**Is it free? Can I use it in my music?**
Yes and yes. It's MIT-licensed, so use it however you like, commercially too. Credit is appreciated but not required.

**Can I suggest an idea or report a bug?**
Please do: [ideas](../../issues/new?template=idea.yml) · [bugs](../../issues/new?template=bug.yml).
