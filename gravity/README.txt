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

Gravity Defied
==============

Мото-триал 2004 года на J2ME в том виде, в каком он есть в C++-порте
rgimad/gravity_defied_cpp, работающий аддоном Sonix Player.

ЧТО ЭТО
-------

Игра не наша. Она начиналась как "Gravity Defied" от Codebrew Software (2004,
J2ME), была переписана на C++ с SDL2 rgimad, AntonEvmenenko и Max Logaev и
используется здесь по GPL версии 2:

    https://github.com/rgimad/gravity_defied_cpp

Имя, логотип и графика принадлежат Codebrew Software; этот аддон с ними не
связан. См. LICENSE (GPL-2.0, лицензия апстрима) и NOTICE.md (что взято, что
изменено и почему).

ЧТО ИЗМЕНЕНО ДЛЯ ПЛЕЕРА
-----------------------

Никакого SDL. Апстрим рисует через SDL2, SDL2_image и SDL2_ttf; у аддона на этой
карте нет ни SDL, ни окна, ни GPU, поэтому слой, с которым говорит игра --
lcdui-шные Graphics, Image, Font и Canvas -- реализован поверх собственного
фреймбуфера плеера (gravity/platform/). Код самой игры, её физика и уровни --
апстримные, без изменений, кроме того, что перечислено в NOTICE.md.

КАК ДЕРЖАТЬ
-----------

Игра в ландшафте: поверните плеер на четверть оборота, верхним краем влево.
Экран при этом 480x320.

Управление -- кнопками плеера, потому что тачскрин этой платы сообщает об одном
пальце за раз, а мотоциклу нужны газ и наклон одновременно:

    кнопка play/pause     газ
    кнопка next song      тормоз
    громкость вверх       наклон назад
    громкость вниз        наклон вперёд
    кнопка prev song      собственное меню игры

    касание экрана        выбор в меню игры
    шеврон "назад"        выход из аддона -- рисуется, только пока открыто меню
                          игры, так что в самом заезде экран целиком у игры

Пока игра идёт, кнопки принадлежат ей: нажатия не меняют громкость, не
переключают трек и ничего не ставят на паузу. Исключение -- кнопка питания, она
возвращается плееру.

То, что играло в плеере, ставится на паузу при открытии игры. После неё оно
остаётся на паузе: вернуть можно кнопкой play самого плеера.

Шеврон "назад" (или "Exit Game" в игре) завершает игру и пишет рекорды и
настройки в saves/; следующий заход снова начинает игру с логотипа.

ФАЙЛЫ
-----

    addon.json      что это за плитка
    addon.so        код
    assets/         графика, уровни (levels.mrg) и исходный шрифт
    saves/          рекорды, создаются на карте при первом запуске

Ничего здесь не пишет в настройки самого плеера.

ЗАМЕЧАНИЕ О СКОРОСТИ
--------------------

Кадр рисуется процессором в буфер 480x320, LVGL поворачивает его на четверть
оборота и отдаёт панели. Это 1 ГГц MIPS без GPU: игра идёт в темпе апстрима,
30 шагов в секунду, и вся картинка перерисовывается вручную. Если кажется
медленным -- вот куда уходит время.
