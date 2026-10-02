# scanlab keymap

A blank layout for characterizing matrix scan timing with the Scan Lab panel
in keybard-ng. Every key is `KC_NO` on every layer, so nothing can be typed or
clicked while a sweep runs the matrix at deliberately wrong timing. The board
enumerates as **Svalboard ScanLab** so it is easy to pick in the browser's
device chooser, and it reports hardware revision B regardless of the strap
(override with `-DSVAL_HW_REV_FORCE=0`).

    make svalboard/trackball/pmw3389/left:scanlab
