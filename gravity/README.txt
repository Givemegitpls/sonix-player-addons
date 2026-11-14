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

Controls are the next stage of the port: the game currently runs on its own --
the loading screen, the menu drawn over the first level -- without a way to press
a key. Gas, brake and the two leans are to be touch zones, and the menu button
the centre of the screen.

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
