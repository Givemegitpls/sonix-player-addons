Qobuz - an addon for the Sonix Player (TempoTec V3 Analog)
Qobuz - аддон для Sonix Player (TempoTec V3 Analog)

INSTALL / УСТАНОВКА
  These files belong in <card>/Addons/qobuz/ - addon.json has to sit directly
  in that folder, not a level deeper. The tile appears in Streaming when the
  card is read; a restart is normally not needed.
  Эти файлы кладутся в <card>/Addons/qobuz/ - addon.json должен лежать прямо в
  этой папке, не уровнем глубже. Плитка появляется в Streaming, когда карта
  прочитана; перезапуск обычно не нужен.

SIGN IN / ВХОД
  There is no password form. The account's user_auth_token goes into
  Addons/qobuz/token.txt, and the addon checks it when its page is built (or on
  "Sign in from the card").
  Take the token from the web player: play.qobuz.com in a browser, developer
  tools, Network tab, any API request (user/login), Headers, Request Headers,
  x-user-auth-token.
  Формы с паролем нет. user_auth_token аккаунта кладётся в
  Addons/qobuz/token.txt, аддон проверяет его при построении страницы (или по
  "Sign in from the card").
  Токен берётся из веб-плеера: play.qobuz.com в браузере, инструменты
  разработчика, вкладка Network, любой запрос к API (user/login), Headers,
  Request Headers, x-user-auth-token.

FILES / ФАЙЛЫ
  Addons/qobuz/token.txt      the token you paste / вписанный токен
  Addons/qobuz/session.ini    the accepted account / принятый аккаунт
  Addons/qobuz/settings.ini   the application pair / пара приложений
  Addons/qobuz/cache/         downloaded tracks / скачанные треки

FULL INSTRUCTIONS (English and Russian) / ПОДРОБНАЯ ИНСТРУКЦИЯ
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#2-qobuz
