Subsonic - an addon for the Sonix Player (TempoTec V3 Analog)
Subsonic - аддон для Sonix Player (TempoTec V3 Analog)

INSTALL / УСТАНОВКА
  These files belong in <card>/Addons/subsonic/ - addon.json has to sit directly
  in that folder, not a level deeper. The tile appears in Streaming when the
  card is read; a restart is normally not needed.
  Эти файлы кладутся в <card>/Addons/subsonic/ - addon.json должен лежать прямо
  в этой папке, не уровнем глубже. Плитка появляется в Streaming, когда карта
  прочитана; перезапуск обычно не нужен.

SIGN IN / ВХОД
  Tap "Sign in" and answer three questions: Server, User name, Password.
  Server accepts 192.168.1.10:4533, http://host:4533 or https://host - the
  scheme is kept and trailing slashes dropped. A server behind a proxy that
  redirects plain HTTP is asked again over https.
  Нажать "Sign in" и ответить на три вопроса: Server, User name, Password.
  Server принимает 192.168.1.10:4533, http://host:4533 или https://host - схема
  сохраняется, завершающие слэши убираются. Сервер за прокси, отвечающий
  редиректом на обычный HTTP, переспрашивается по https.

FILES / ФАЙЛЫ
  Addons/subsonic/servers/<slug>.ini  one account per file / один аккаунт на файл
  Addons/subsonic/servers/active      the one in use / используемый
  Addons/subsonic/cache/              downloaded tracks / скачанные треки

FULL INSTRUCTIONS (English and Russian) / ПОДРОБНАЯ ИНСТРУКЦИЯ
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#4-subsonic
