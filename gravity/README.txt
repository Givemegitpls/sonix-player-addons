Gravity Defied
==============

The 2004 J2ME moto-trial game, as the C++ port rgimad/gravity_defied_cpp has it,
running as an addon of the Sonix Player.

WHAT THIS IS
------------

The game is not ours. It started as "Gravity Defied" by Codebrew Software (2004,
J2ME), was rewritten in C++ with SDL2 by rgimad, AntonEvmenenko and Max Logaev,
and is used here under the GPL, version 2:

    https://github.com/rgimad/gravity_defied_cpp

The name, the logotype and the artwork belong to Codebrew Software; this addon is
not associated with them. See LICENSE (GPL-2.0, the upstream licence) and
NOTICE.md (what was taken, what was changed, and why).

WHAT WAS CHANGED FOR THE PLAYER
-------------------------------

No SDL. Upstream draws through SDL2, SDL2_image and SDL2_ttf; an addon on this
card has no SDL, no window and no GPU, so the layer the game talks to -- lcdui's
Graphics, Image, Font and Canvas -- is implemented against the player's own frame
buffer instead (gravity/platform/). The game's own code, its physics and its
levels are upstream's, unchanged except where NOTICE.md says otherwise.

HOLDING IT
----------

The game is landscape: turn the player a quarter turn, its top edge to the left.
The screen is 480x320 that way.

Controls are the player's own buttons, because this board's touchscreen reports
one finger at a time and a bike needs gas and a lean together:

    play/pause button    gas
    next song button     brake
    volume up            lean back
    volume down          lean forward
    prev song button     the game's own menu

    a tap on the screen   select, in the game's menus
    the back chevron      leave the addon -- drawn only while one of the game's
                          menus is open, so the race itself has the whole screen

While the game runs the buttons belong to it: pressing them does not change the
volume, skip a track or pause anything. The power button is the exception and is
handed back to the player.

Whatever the player was playing is paused when the game opens. It stays paused
afterwards: the player's own play button brings it back.

The back chevron (or the game's "Exit Game") ends the game and writes the records
and settings into saves/; the next visit starts the game again from the logo.

FILES
-----

    addon.json      what the tile is
    addon.so        the code
    assets/         the artwork, the levels (levels.mrg) and the source font
    saves/          the records, made on the card at the first run

Nothing here writes to the player's own settings.

A NOTE ABOUT SPEED
------------------

The frame is drawn by the CPU into a 480x320 buffer, turned a quarter turn by
LVGL and handed to the panel. This is a 1 GHz MIPS with no GPU: the game runs at
the pace upstream paces it, 30 steps a second, and the whole picture is redrawn
by hand. If it looks slow, that is where the time goes.
