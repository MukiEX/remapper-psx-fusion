# Example HID Remapper JSON Files

These files can be imported in via Actions -> Import JSON File in the HID Remapper configuration Web-UI

They will work fine as-is, but are primarily designed to work with the PSX conversion

## taito_egret_ii_mouse.json

This is designed to basically turn the Taito Egret Mini Trackball and Paddle controller into a normal computer mouse.

By default, this controller had a really weird configuration. The trackball was a mouse that ran at about 1/8th the expected speed and the spinner knob worked as a mouse scroll wheel. The remap turns both into mouse cursor movement. The trackball now moves 8 times faster and the scroll wheel also moves the mouse on the X axis.

The purple buttons on the left and right function as left and right mouse clicks.

The white button will reverse the knob's x-axis mouse direction, which I thought felt more natural in Tempest X for PS1.

## mouse_joystick_spin.json

Also designed for the Egret Mini Trackball controller. This will map the scroll wheel on the mouse, which the controller uses as the rotary knob, to spinning the left joystick in a circle. This was partially slop-designed, as between Gemini, Grok, and Claude, nobody could write a functioning set of expressions, but Claude got close enough that I was able to fix it.o

This is INCREDIBLY niche. It's basically designed for the Warlords clone called "Lords of Lunar", a mini-game included on the "Making of" CD for Lunar: Silver Star Story Complete on PSX.

## yuangeki_gamepad.json

May delete this, as it's not particularly relevant to the project. This converts the Yuancon Yuangeki controller into a standard gamepad, with the angle of the lever converted to the X axis on the joystick, and the rest to face and shoulder buttons/triggers. The left/right d-pad buttons were doubled up into L3/R3 on account of the fact that USB gamepads in Windows have hard-locked axes, making it impossible to press the left and right buttons at the same time.

