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
  settings.ini in this folder already carries the four keys, so there is nothing
  to fill in. They are the official client's own and the MarshalX/yandex-music-api
  library publishes them. If playback starts being refused after a Yandex
  update, bump the version in client_header there (the whole header line, name
  included) - see docs/SETUP.md, section 3. Do not overwrite your settings.ini
  when updating the addon.
  В этой папке settings.ini уже содержит все четыре ключа, заполнять нечего. Они
  принадлежат официальному клиенту и опубликованы библиотекой
  MarshalX/yandex-music-api. Если после обновления Яндекса воспроизведение
  начало отказывать, поднимите версию в client_header (там лежит вся строка
  заголовка, вместе с именем) - см. docs/SETUP.md, раздел 3. При обновлении
  аддона не перезаписывайте свой settings.ini.

SIGN IN / ВХОД
  Nothing to paste. Tap "Sign in with a code": the page shows Where
  (ya.ru/device) and Code (eight characters). Open that address on a phone
  already signed in, type the code, confirm - the addon signs in by itself.
  The page also says on its own what is missing: with no keys it names
  settings.ini and the four keys, and says no code can be asked for without
  them.
  Вписывать ничего не нужно. Нажать "Sign in with a code": страница покажет
  Where (ya.ru/device) и Code (восемь символов). Открыть адрес на телефоне, где
  уже выполнен вход, ввести код, подтвердить - аддон войдёт сам. Страница сама
  пишет, чего не хватает: без ключей она называет settings.ini и четыре ключа,
  и говорит, что без них код не выдадут.

FILES / ФАЙЛЫ
  Addons/yandex/settings.ini          the four keys, quality / ключи, качество
  Addons/yandex/yandex_session.ini    the account / аккаунт
  Addons/yandex/token.txt             only from an older card / только со старой карты
  Addons/yandex/cache/, saved.tsv     downloads and their index / загрузки и индекс

FULL INSTRUCTIONS (English and Russian) / ПОДРОБНАЯ ИНСТРУКЦИЯ
  https://github.com/Givemegitpls/sonix-player-addons/blob/main/docs/SETUP.md#3-yandex-music
