# Svalboard keymaps

The maintained everyday configuration is **`sval`**, using the `svalboard/core` community module and `sval.json` definition. It supports the matching Keybard client. Base-board **`blank`** builds are included in the release workflow;

Build an appropriate side/sensor variant, for example:

```sh
qmk compile -kb svalboard/trackball/pmw3389/right -km sval
```

See the [launch compendium](https://github.com/svalboard/keybard/blob/main/docs/launch/README.md) for features, benefits, firmware selection, and moving from Vial. See the [board README](../readme.md) for bootloader and handedness instructions. Other historical keymaps may not track the maintained board API.
