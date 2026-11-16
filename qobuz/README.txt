Qobuz - an addon for the Sonix Player (TempoTec V3 Analog)

INSTALL
  These files belong in <card>/Addons/qobuz/ - addon.json has to sit directly
  in that folder, not a level deeper. The tile appears in Streaming when the
  card is read; a restart is normally not needed.

SIGN IN
  There is no password form. The account's user_auth_token goes into
  Addons/qobuz/token.txt, and the addon checks it when its page is built (or on
  "Sign in from the card").

  Take the token from the web player: play.qobuz.com in a browser, developer
  tools, Network tab, any API request (user/login), Headers, Request Headers,
  x-user-auth-token.

FILES
  Addons/qobuz/token.txt      the token you paste
  Addons/qobuz/session.ini    the accepted account
  Addons/qobuz/settings.ini   the application pair
  Addons/qobuz/cache/         downloaded tracks

FULL INSTRUCTIONS
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#2-qobuz

Qobuz - аддон для Sonix Player (TempoTec V3 Analog)

УСТАНОВКА
  Эти файлы кладутся в <card>/Addons/qobuz/ - addon.json должен лежать прямо
  в этой папке, не уровнем глубже. Плитка появляется в Streaming, когда карта
  прочитана; перезапуск обычно не нужен.

ВХОД
  Формы с паролем нет. user_auth_token аккаунта кладётся в
  Addons/qobuz/token.txt, аддон проверяет его при построении страницы (или по
  "Sign in from the card").

  Токен берётся из веб-плеера: play.qobuz.com в браузере, инструменты
  разработчика, вкладка Network, любой запрос к API (user/login), Headers,
  Request Headers, x-user-auth-token.

ФАЙЛЫ
  Addons/qobuz/token.txt      вписанный токен
  Addons/qobuz/session.ini    принятый аккаунт
  Addons/qobuz/settings.ini   пара приложений
  Addons/qobuz/cache/         скачанные треки

ПОДРОБНАЯ ИНСТРУКЦИЯ
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#2-qobuz
