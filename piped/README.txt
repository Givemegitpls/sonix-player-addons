Piped - an addon for the Sonix Player (TempoTec V3 Analog)

INSTALL
  These files belong in <card>/Addons/piped/ - addon.json has to sit directly
  in that folder, not a level deeper. The tile appears in Streaming when the
  card is read; a restart is normally not needed.

INSTANCES
  The player cannot reach YouTube itself, so the addon goes through a Piped
  instance: a public relay that fetches from YouTube and passes the audio on.
  Two are built in. The Instance row marks the one in use; Automatic takes the
  first that answers, Add your own... takes any Piped API address, Test
  connection says what the current one replies.

  The one maintained list of public instances is in the Piped project's
  documentation: github.com/TeamPiped/documentation,
  content/docs/public-instances/index.md. Paste an instance's API address - the
  host usually starts with pipedapi. or api.piped. - and check it with Test
  connection: most hosts on that list are dead, answer 502, or say "Piped has
  shutdown". LibreTube keeps no public list of its own any more, only the
  instances the user has added.

  If search works and tracks do not start, YouTube has most likely blocked the
  relay's own address for a while. That is not something this side can fix, and
  another instance is the remedy until it passes.

FILES
  Addons/piped/settings.ini   the pinned instance, empty = automatic
  Addons/piped/cache/         downloaded tracks, their tags and covers

FULL INSTRUCTIONS
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#8-piped

Piped - аддон для Sonix Player (TempoTec V3 Analog)

УСТАНОВКА
  Эти файлы кладутся в <card>/Addons/piped/ - addon.json должен лежать прямо
  в этой папке, не уровнем глубже. Плитка появляется в Streaming, когда карта
  прочитана; перезапуск обычно не нужен.

ИНСТАНСЫ
  Плеер до самого YouTube не достаёт, поэтому аддон работает через инстанс
  Piped - публичный посредник, который обращается к YouTube и отдаёт звук. Два
  встроены. Строка Instance отмечает используемый; Automatic берёт первый
  ответивший, Add your own... принимает любой адрес Piped API, Test connection
  показывает ответ текущего.

  Единственный поддерживаемый список публичных инстансов - в документации
  проекта Piped: github.com/TeamPiped/documentation,
  content/docs/public-instances/index.md. Вписывается адрес API инстанса - хост
  обычно начинается с pipedapi. или api.piped. - и проверяется через Test
  connection: большинство хостов из этого списка мертвы, отвечают 502 или
  "Piped has shutdown". LibreTube своего публичного списка больше не держит,
  только добавленные пользователем.

  Если поиск работает, а треки не начинаются, скорее всего YouTube временно
  заблокировал адрес самого посредника. С этой стороны это не лечится, помогает
  другой инстанс, пока блокировка не снимется.

ФАЙЛЫ
  Addons/piped/settings.ini   выбранный инстанс, пусто = автоматически
  Addons/piped/cache/         скачанные треки, их теги и обложки

ПОДРОБНАЯ ИНСТРУКЦИЯ
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#8-piped
