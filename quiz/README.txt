Quiz - an addon for the Sonix Player (TempoTec V3 Analog)

INSTALL
  These files belong in <card>/Addons/quiz/ - addon.json has to sit directly
  in that folder, not a level deeper. The tile appears in More when the card
  is read.

WHAT IT IS
  Guess the artist, the track or the album from a short snippet of a track in
  your library. The start card picks what to guess and how to play:
    Survival — hearts and points together: a hit is +10 down to +1 by the
               seconds of the snippet, any miss costs a heart (a wrong answer
               also -10, silence -5, Skip no points). Every tenth track tops the
               hearts up (ten hits in a row worth more than 5 each fill the
               row). Out of hearts ends the run, and the points are the score.
    Endless  — no lives, the streak is the score
    Timed    — a run clock and a per-answer clock, lives as in Survival
  Every game shuffles the library and asks each track (or each artist) once as
  the correct answer; when nothing is left to ask, the run ends as Library
  complete. The three distractors are drawn from the same library and, as far
  as the library allows, never share the answer's credits or a one-letter typo,
  so two names that differ by an "&" are not offered together.

  Gear — per-mode options: reveal the answer or not and for how long, lives,
  run length, per-answer limit. Settings are saved in
  Addons/quiz/settings.ini.
  Difficulty, on the start card and only when the artist is what to guess:
  Easy asks the artist the track is filed under, Medium any name the tag
  credits, Hard only the guests. A track tagged "A; B" is filed under both,
  and on Easy the answer is the one a listener would give. Tags are split the
  way the player's library splits them (its own separator settings), so the
  answer is always a name the Artists list would show.

  Skip, a row of its own under the choices, gives the round up for one heart and
  no points; the broken heart it spends is drawn on it. It is Survival's own
  button. The answer window there is the snippet: when the twelve seconds are
  gone, the round is a silence. The reveal shows the cover, the album and every
  name the tag credits.
  Trophy — personal bests as a list, one row per mode and difficulty: a run on
  Hard is its own row next to Easy. Only combinations that were played are
  shown.
  Pause — the play/pause key, or the button in the corner. A round holds while
  the panel is dark and does not resume by itself.
  Back — the chevron or the swipe steps up one level: out of a round, the
  reveal or the result card to the start card, and from there out of the addon.
  The result card carries "Main menu" beside "Play again" for the same step.

KEYS
  The addon takes the player's keys while it is on screen: next/previous do
  nothing behind a question, play/pause is the quiz's pause, volume still works,
  a short power press blanks the panel and pauses the round, a long one opens
  the power menu.

Викторина - аддон для Sonix Player (TempoTec V3 Analog)

УСТАНОВКА
  Эти файлы кладутся в <card>/Addons/quiz/ - addon.json должен лежать прямо
  в этой папке, не уровнем глубже. Плитка появляется в More, когда карта
  прочитана.

ЧТО ЭТО
  Угадайте исполнителя, трек или альбом по короткому фрагменту трека из
  медиатеки. Стартовая карточка выбирает, что угадывать и как играть:
    Выживание — жизни и баллы вместе: верный ответ +10…+1 по секундам
               фрагмента, любая ошибка стоит сердца (неверный ответ ещё −10,
               молчание −5, «Пропустить» — без баллов). Каждые 10 треков жизни
               пополняются (десять верных подряд дороже 5 баллов заполняют
               полоску целиком). Жизни кончились — конец забега, баллы и есть
               счёт.
    Бесконечный — жизней нет, счёт — серия
    На время  — общий таймер и таймер на ответ, жизни как в «Выживании»
  За игру медиатека перемешивается, и каждый трек (или исполнитель)
  загадывается один раз; когда загадывать больше нечего, игра заканчивается
  как «Медиатека пройдена». Три неверных варианта берутся из той же медиатеки и,
  насколько она это позволяет, не делят с ответом соавторов или опечатку в одну
  букву — два имени, отличающихся одним «&», рядом не окажутся.

  Шестерёнка — настройки для каждого режима: показывать ответ и на сколько,
  жизни, длину забега, лимит на ответ. Настройки хранятся в
  Addons/quiz/settings.ini.
  Сложность — на стартовой карточке и только когда угадывается исполнитель:
  «Лёгкий» спрашивает того, за кем закреплён трек, «Средний» — любого из
  указанных в теге, «Сложный» — только гостей. Трек с тегом «A; B» закреплён
  за обоими, и на «Лёгком» ответом будет то имя, которое назвал бы слушатель.
  Тег разбирается теми же разделителями, что в настройках медиатеки плеера,
  поэтому ответ — всегда имя, которое показал бы список исполнителей.

  «Пропустить» — отдельной строкой под вариантами: ход сдаётся за одно сердце и
  ничего не стоит в баллах, на кнопке нарисовано разбитое сердце. Кнопка есть
  только в «Выживании». Окно ответа там — сам фрагмент: когда двенадцать секунд
  прошли, раунд считается молчанием. Превью ответа показывает обложку, альбом и
  все имена, которые указаны в теге.
  Кубок — рекорды списком: по строке на режим и сложность. Прогон на «Сложном»
  лежит отдельно от «Лёгкого». Показываются только те сочетания, которые играли.
  Пауза — клавиша play/pause или кнопка в углу. Пока экран погашен, раунд
  стоит на паузе и сам не продолжается.
  Назад — шеврон или свайп поднимают на уровень: из раунда, превью ответа или
  карточки результата — на стартовую карточку, а с неё — из аддона. На
  карточке результата для того же есть кнопка «В главное меню».

КЛАВИШИ
  Пока аддон на экране, он забирает клавиши плеера: next/previous за вопросом
  ничего не делают, play/pause — пауза викторины, громкость работает, короткое
  нажатие питания гасит экран и ставит раунд на паузу, долгое открывает меню
  питания.
