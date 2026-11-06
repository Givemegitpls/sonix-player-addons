Yandex Music - an addon for the Sonix Player (TempoTec V3 Analog)
Яндекс Музыка - аддон для Sonix Player (TempoTec V3 Analog)

INSTALL / УСТАНОВКА
  These files belong in <card>/Addons/yandex/ - addon.json has to sit directly
  in that folder, not a level deeper. The tile appears in Streaming when the
  card is read; a restart is normally not needed.
  Эти файлы кладутся в <card>/Addons/yandex/ - addon.json должен лежать прямо в
  этой папке, не уровнем глубже. Плитка появляется в Streaming, когда карта
  прочитана; перезапуск обычно не нужен.

KEYS / КЛЮЧИ
  The addon ships no credentials. Four keys go into Addons/yandex/settings.ini:
  client_id, client_secret, sign_key, client_header. Without all four the page
  says "no keys" and the service stays off. Where the values come from is in
  docs/SETUP.md, section 3.
  Аддон не несёт учётных данных. Четыре ключа кладутся в
  Addons/yandex/settings.ini: client_id, client_secret, sign_key,
  client_header. Без всех четырёх страница пишет "no keys", сервис выключен.
  Откуда брать значения - в docs/SETUP.md, раздел 3.

SIGN IN / ВХОД
  Nothing to paste. Tap "Sign in with a code": the page shows Where
  (ya.ru/device) and Code (eight characters). Open that address on a phone
  already signed in, type the code, confirm - the addon signs in by itself.
  Вписывать ничего не нужно. Нажать "Sign in with a code": страница покажет
  Where (ya.ru/device) и Code (восемь символов). Открыть адрес на телефоне, где
  уже выполнен вход, ввести код, подтвердить - аддон войдёт сам.

FILES / ФАЙЛЫ
  Addons/yandex/settings.ini          the four keys, quality / ключи, качество
  Addons/yandex/yandex_session.ini    the account / аккаунт
  Addons/yandex/token.txt             only from an older card / только со старой карты
  Addons/yandex/cache/, saved.tsv     downloads and their index / загрузки и индекс

FULL INSTRUCTIONS (English and Russian) / ПОДРОБНАЯ ИНСТРУКЦИЯ
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#3-yandex-music
