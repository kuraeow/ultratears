// The launcher's words in English and Russian. A new language: one more column, LANG_COUNT and lang_name / lang_code.
// The codes of setup.py --json (dump_*, hw cases of tools/hw_check.py, step errors) have their texts here: "<code>"
// is the headline, "<code>.d" the detail; {name} is filled from the event's args.
#include "launcher.h"
#include <cstring>

Lang g_lang = LANG_EN;

struct Text { const char *key, *s[LANG_COUNT]; };

static const Text TEXTS[] = {
    {"dump_title", {"Path to original game files", "Путь к оригинальной игре"}},
    {"dump_sub", {"UltraTears is built from your own copy of the game. Choose the folder with it - the launcher "
                  "checks the version and compatibility itself.",
                  "UltraTears собирается из вашей собственной копии игры. Выберите папку с ней - лаунчер сам проверит "
                  "версию и совместимость."}},
    {"choose_folder", {"Choose folder", "Выбрать папку"}},
    {"drop_hint", {"You can also drop the folder onto this window", "Можно перетащить в это окно"}},
    {"checking", {"Checking the folder…", "Проверяю папку…"}},
    {"choose_other", {"Choose another folder", "Выбрать другую папку"}},
    {"dump_label", {"Folder with the original game", "Папка с исходной игрой"}},
    {"data_label", {"The game's data", "Данные игры"}},
    {"data_note", {"Already prepared from your copy: the original game is no longer needed.",
                   "Уже подготовлены из вашей копии — исходная игра больше не нужна."}},
    {"version", {"Version {v}", "Версия {v}"}},
    {"data_sub", {"The game was prepared here before: the launcher builds again only what has changed since.",
                  "Игра здесь уже готовилась — лаунчер пересоберёт только то, что изменилось с тех пор."}},
    {"free_only", {"Free: {free} GB.", "Свободно {free} GB."}},
    {"copying", {"Copying the builder", "Копирование сборщика"}},
    {"install_copy", {"the builder could not be copied to {path}", "не удалось скопировать сборщик в {path}"}},
    {"broken_title", {"The launcher's files are missing", "Не найдены файлы лаунчера"}},
    {"broken_text", {"The launcher was started without the rest of the archive: from inside the ZIP, or copied on its "
                     "own. Extract the whole archive into a folder of its own and start UltraTears Launcher.exe there.",
                     "Лаунчер запущен без остальных файлов архива — прямо из ZIP или скопирован отдельно. Распакуйте "
                     "архив целиком в отдельную папку и запустите UltraTears Launcher.exe оттуда."}},
    {"install_label", {"Where the game will be installed", "Финальное место распаковки игры"}},
    {"change", {"Change", "Изменить"}},
    {"need_free", {"Needs ≈ {need} GB. Free: {free} GB.", "Нужно ≈ {need} GB. Свободно {free} GB."}},
    {"wait_time", {"≈ {lo}-{hi} minutes of waiting", "≈ время ожидания {lo}-{hi} минут"}},
    {"can_delete", {"Once the game is ready, the original files in {dump} can be deleted.",
                    "По окончании установки оригинальные файлы из {dump} можно удалить."}},
    {"no_space", {"Not enough space on {drive} — choose another folder or free some space.",
                  "Не хватает места на диске {drive} — выберите другую папку или освободите место."}},
    {"prepare", {"Prepare the game", "Подготовить игру"}},
    {"resume", {"Continue preparing", "Продолжить подготовку"}},
    {"resume_hint", {"The last preparation stopped midway: it continues where it left off.",
                     "Прошлая подготовка остановилась на середине — продолжится с того же места."}},
    {"rebuild", {"Update the game", "Обновить игру"}},
    {"rebuild_hint", {"UltraTears was updated: only what changed is rebuilt, usually a few minutes.",
                      "UltraTears обновилась — пересоберётся только изменённое, обычно несколько минут."}},
    {"upd_banner", {"Version {v} is out", "Доступна версия {v}"}},
    {"upd_open", {"Update", "Обновить"}},
    {"upd_title", {"Update to {v}", "Обновление до {v}"}},
    {"upd_from", {"Installed: {old}. New: {v}.", "Установлена {old}, новая — {v}."}},
    {"upd_notes", {"What's new", "Что нового"}},
    {"upd_what", {"The launcher downloads the new version from GitHub, checks it and puts it in place of the old one. "
                  "Your game data and saves stay; then only what changed is rebuilt.",
                  "Лаунчер скачает новую версию с GitHub, проверит её и поставит вместо старой. Данные игры и сохранения "
                  "останутся, затем пересоберётся только изменённое."}},
    {"upd_go", {"Download the update", "Скачать обновление"}},
    {"upd_retry", {"Try again", "Повторить"}},
    {"upd_later", {"Not now", "Не сейчас"}},
    {"upd_download", {"Downloading", "Загрузка"}},
    {"upd_apply", {"Installing the update", "Установка обновления"}},
    {"upd_keep", {"The launcher restarts by itself when it is done.", "Когда всё будет готово, лаунчер перезапустится сам."}},
    {"upd_failed", {"The update failed", "Обновить не удалось"}},
    {"upd_restart", {"The update is in place: start the launcher again.", "Обновление установлено — запустите лаунчер заново."}},
    {"prep_title", {"Preparing the game", "Подготовка игры"}},
    {"prep_sub", {"The launcher does all the technical work itself", "Всю техническую работу лаунчер сделает сам"}},
    {"processing", {"Processing files", "Обработка файлов"}},
    {"step_check", {"Hardware check", "Проверка оборудования"}},
    {"step_romfs", {"Copying the game data", "Копирование данных игры"}},
    {"step_lift", {"Translating the game code", "Перевод кода игры"}},
    {"step_compile", {"Building the game", "Сборка игры"}},
    {"step_shaders", {"Preparing the graphics", "Подготовка графики"}},
    {"step_textures", {"Compressing textures", "Сжатие текстур"}},
    {"step_deck", {"Packing for the Steam Deck", "Упаковка для Steam Deck"}},
    {"plat_windows", {"For Windows", "Для Windows"}},
    {"plat_linux", {"For Linux (Steam Deck)", "Для Linux (Steam Deck)"}},
    {"plat_both", {"Both platforms", "Обе платформы"}},
    {"linux_kit_note", {"The Linux kit will be\ndownloaded as well", "Дополнительно будет скачан\nнабор для Linux"}},
    {"deck_carry", {"For the Steam Deck: copy {file} ({gb} GB) to the Deck and unpack it there.",
                    "Для Steam Deck: перенесите на Deck файл {file} ({gb} GB) и распакуйте его там."}},
    {"show_file", {"Show the file", "Показать файл"}},
    {"already_running", {"UltraTears Launcher is already running.", "Лаунчер UltraTears уже запущен."}},
    {"start_failed", {"The launcher could not open its window:\n{err}\n\nThe details are in {log}.",
                      "Лаунчер не смог открыть окно:\n{err}\n\nПодробности — в {log}."}},
    {"left", {"about {t} left", "осталось ≈ {t}"}},
    {"may_take", {"may take about {t}", "может потребоваться около {t}"}},
    {"took", {"took {t}", "заняло {t}"}},
    {"was_done", {"already done", "уже готово"}},
    {"cancel", {"Cancel", "Отмена"}},
    {"bg_hint", {"You can minimize the window: preparing goes on in the background.",
                 "Можно свернуть окно — подготовка продолжится в фоне."}},
    {"cancel_q", {"Stop preparing? Next time it continues where it stopped.",
                  "Остановить подготовку? В следующий раз она продолжится с того же места."}},
    {"stop", {"Stop", "Остановить"}},
    {"go_on", {"Go on", "Продолжить"}},
    {"failed_title", {"Preparing the game failed", "Подготовка игры завершилась неудачей"}},
    {"report1", {"If you think this is a bug, please open an ", "Если считаете это ошибкой, пожалуйста, создайте "}},
    {"report_link", {"issue on GitHub", "issue на GitHub"}},
    {"report2", {" and attach ultratears-setup.log.", " и приложите ultratears-setup.log."}},
    {"open_log", {"Open the log folder", "Открыть папку с логом"}},
    {"done_title", {"The game is ready", "Подготовка игры завершена"}},
    {"done_btn", {"Done", "Готово"}},
    {"hw_note", {"below the recommended hardware", "оборудование не соответствует рекомендуемому"}},
    {"hw_title", {"Hardware check", "Проверка оборудования"}},
    {"hw_text1", {"Your graphics card does not meet the minimum requirements. The game may not work. See the ",
                  "Ваша видеокарта не соответствует минимальным характеристикам. Работоспособность не может быть "
                  "гарантирована. Ознакомьтесь с "}},
    {"hw_link", {"system requirements", "техническими требованиями"}},
    {"hw_text2", {" of the game.", " игры."}},
    {"hw_go", {"Continue anyway", "Всё равно продолжить"}},
    {"tab_main", {"General", "Основные"}},
    {"tab_lang", {"Language", "Язык"}},
    {"tab_debug", {"Debug", "Отладка"}},
    {"tab_controls", {"Controls", "Управление"}},
    {"tab_mods", {"Mods", "Моды"}},
    {"mods_folder", {"Open mods folder", "Открыть папку модов"}},
    {"mods_refresh", {"Refresh", "Обновить"}},
    {"mods_note", {"The higher mod wins a file two mods change. Changes apply the next time the game starts.",
                   "Если два мода меняют один файл, побеждает тот, что выше. Изменения применяются при следующем запуске игры."}},
    {"mods_loading", {"Reading the mods folder...", "Читаю папку модов..."}},
    {"mods_failed", {"The mods folder could not be read (see ultratears-mods.log).",
                     "Не удалось прочитать папку модов (подробности в ultratears-mods.log)."}},
    {"mods_none", {"No mods yet. Unpack each mod into a folder of its own inside the mods folder, so that its romfs, exefs or "
                   "cheats folder sits directly in it. Mods made for the Switch emulators work as they are.",
                   "Модов пока нет. Распакуйте каждый мод в отдельную папку внутри папки модов так, чтобы его папка romfs, "
                   "exefs или cheats лежала прямо в ней. Моды для эмуляторов Switch подходят как есть."}},
    {"mods_code_stale", {"The code changes of mods and cheats are prepared when you press Play (a few seconds).",
                         "Изменения кода из модов и читов подготовятся, когда вы нажмёте «Играть» (несколько секунд)."}},
    {"mods_building", {"Preparing the mods...", "Готовлю моды..."}},
    {"mods_building_note", {"Only when mods' code patches or cheats changed: a few seconds.",
                            "Только если менялись патчи кода или читы модов: несколько секунд."}},
    {"mod_files", {"replaces {r} game files, adds {a}", "заменяет файлов игры: {r}, добавляет: {a}"}},
    {"mod_other_version", {"{n} named for another game version", "для другой версии игры: {n}"}},
    {"mod_overridden", {"{n} of its files taken by {m} above", "файлов, которые берутся из мода выше ({m}): {n}"}},
    {"mod_code", {"code patch", "патч кода"}},
    {"mod_code_other", {"code patch for another game version: not used", "патч кода для другой версии игры: не используется"}},
    {"mod_cheats", {"cheats: {n}", "читов: {n}"}},
    {"mod_empty", {"No romfs, exefs or cheats folder inside: move them up so they sit directly in the mod's folder.",
                   "Внутри нет папок romfs, exefs или cheats: перенесите их на уровень выше, прямо в папку мода."}},
    {"mod_bad_name", {"The folder's name contains |: rename it.", "В имени папки есть символ |: переименуйте её."}},
    {"mod_up", {"Up", "Выше"}},
    {"mod_down", {"Down", "Ниже"}},
    {"mod_cheats_show", {"Cheats ({n})", "Читы ({n})"}},
    {"mod_cheats_hide", {"Hide cheats", "Скрыть читы"}},
    {"cheat_always", {"always on", "всегда включён"}},
    {"cheat_keys", {"while its buttons are held", "пока зажаты его кнопки"}},
    {"cheat_restore", {"not needed: a cheat switched off restores the code", "не нужен: выключенный чит сам возвращает код"}},
    {"cheat_why_opcode", {"not supported: an opcode the port does not run", "не поддерживается: команда, которую порт не выполняет"}},
    {"cheat_why_write", {"not supported: a code write the port cannot build", "не поддерживается: такую запись в код порт не собирает"}},
    {"cheat_why_mixed", {"not supported: mixes code writes with other commands", "не поддерживается: смешивает запись в код с другими командами"}},
    {"cheat_why_unreadable", {"not supported: the line cannot be read", "не поддерживается: строку не прочитать"}},
    {"ctl_gamepad", {"Gamepad buttons", "Кнопки геймпада"}},
    {"ctl_sticks", {"Sticks and gyro", "Стики и гироскоп"}},
    {"ctl_keyboard", {"Keyboard", "Клавиатура"}},
    {"sec_lstick", {"Left stick", "Левый стик"}},
    {"sec_rstick", {"Right stick", "Правый стик"}},
    {"sec_gyro", {"Gyro", "Гироскоп"}},
    {"row_deadzone", {"Dead zone", "Мёртвая зона"}},
    {"dz_auto", {"Auto", "Авто"}},
    {"deg_s", {"°/s", "°/с"}},
    {"calib", {"Set up automatically", "Настроить автоматически"}},
    {"calib_wait", {"Don't touch the gamepad… {n}", "Не трогайте геймпад… {n}"}},
    {"calib_moved", {"The gamepad moved: put it down, let go of the sticks and try again.",
                     "Геймпад двигался — положите его, отпустите стики и попробуйте ещё раз."}},
    {"calib_done", {"Done: left stick {l}, right stick {r}, gyro {g}.", "Готово: левый стик {l}, правый {r}, гироскоп {g}."}},
    {"sticks_note", {"A stick let go and a gamepad lying still stay in the grey. Changes apply the next time the game starts.",
                     "Отпущенный стик и лежащий геймпад должны оставаться в сером. Изменения — при следующем запуске игры."}},
    {"no_pad", {"Connect a gamepad to see its sticks and gyro", "Подключите геймпад, чтобы видеть стики и гироскоп"}},
    {"no_gyro", {"This gamepad has no gyro", "У этого геймпада нет гироскопа"}},
    {"sec_layout", {"Layout", "Раскладка"}},
    {"row_layout", {"Button layout", "Расположение кнопок"}},
    {"layout_nintendo", {"Nintendo", "Nintendo"}},
    {"layout_xbox", {"Xbox (Steam Deck)", "Xbox (Steam Deck)"}},
    {"layout_note", {"Nintendo: A is the right button, as on a Switch controller. Xbox: A is the bottom one, as on Xbox "
                     "controllers and the Steam Deck.",
                     "Nintendo — A справа, как на контроллере Switch. Xbox — A снизу, как на контроллерах Xbox и "
                     "Steam Deck."}},
    {"sec_buttons", {"Buttons", "Кнопки"}},
    {"sec_move", {"Movement and camera", "Движение и камера"}},
    {"sec_mouse", {"Mouse", "Мышь"}},
    {"row_mouse_sens", {"Camera sensitivity", "Чувствительность камеры"}},
    {"row_mouse_aim", {"Sensitivity while aiming a bow", "Чувствительность при натянутом луке"}},
    {"row_mouse_invert", {"Invert up and down", "Инверсия по вертикали"}},
    {"mouse_note", {"The mouse turns the camera and aims. Holding L, it picks an ability; holding a d-pad key, the "
                    "wheel or moving the mouse sideways steps through weapons, shields and materials.",
                    "Мышь поворачивает камеру и целится. С зажатой L она выбирает способность, с зажатой клавишей "
                    "d-pad колесо или движение мыши вбок листают оружие, щиты и материалы."}},
    {"press_button", {"Press a button…", "Нажмите кнопку…"}},
    {"press_key", {"Press a key…", "Нажмите клавишу…"}},
    {"ctl_reset", {"Back to default", "Вернуть по умолчанию"}},
    {"ctl_reset_all", {"Reset all", "Сбросить всё"}},
    {"ctl_unbound", {"none", "нет"}},
    {"ctl_applies", {"Changes apply the next time the game starts. A mouse button can be pressed in place of a key; Esc cancels waiting.",
                     "Изменения — при следующем запуске игры. Вместо клавиши можно нажать кнопку мыши; Esc отменяет ожидание."}},
    {"mouse_left", {"Left click", "ЛКМ"}},
    {"mouse_right", {"Right click", "ПКМ"}},
    {"mouse_middle", {"Middle click", "Средняя кнопка мыши"}},
    {"act_a", {"A — interact, pick up", "A — действие, подобрать"}},
    {"act_b", {"B — run, cancel", "B — бег, отмена"}},
    {"act_x", {"X — jump", "X — прыжок"}},
    {"act_y", {"Y — attack", "Y — атака"}},
    {"act_l", {"L — abilities", "L — способности"}},
    {"act_r", {"R — throw", "R — бросок"}},
    {"act_zl", {"ZL — shield, lock on", "ZL — щит, захват цели"}},
    {"act_zr", {"ZR — bow", "ZR — лук"}},
    {"act_plus", {"+ — menu", "+ — меню"}},
    {"act_minus", {"− — map", "− — карта"}},
    {"act_lstick", {"Left stick press — crouch", "Нажатие левого стика — присесть"}},
    {"act_rstick", {"Right stick press — first-person view", "Нажатие правого стика — вид от первого лица"}},
    {"act_up", {"D-pad up — quick-select", "Крестовина вверх — быстрый выбор"}},
    {"act_down", {"D-pad down — quick-select", "Крестовина вниз — быстрый выбор"}},
    {"act_left", {"D-pad left — quick-select", "Крестовина влево — быстрый выбор"}},
    {"act_right", {"D-pad right — quick-select", "Крестовина вправо — быстрый выбор"}},
    {"act_move_up", {"Move forward", "Вперёд"}},
    {"act_move_down", {"Move back", "Назад"}},
    {"act_move_left", {"Move left", "Влево"}},
    {"act_move_right", {"Move right", "Вправо"}},
    {"act_cam_up", {"Camera up", "Камера вверх"}},
    {"act_cam_down", {"Camera down", "Камера вниз"}},
    {"act_cam_left", {"Camera left", "Камера влево"}},
    {"act_cam_right", {"Camera right", "Камера вправо"}},
    {"row_debug", {"Debug mode", "Режим отладки"}},
    {"row_snapshots", {"Extended debugging with snapshots", "Расширенная отладка со снимками"}},
    {"snapshots_note", {"F9 saves extended snapshots of the frame. The results are in the diag folder.",
                        "Позволяет на кнопку F9 делать расширенные снимки. Результаты в папке diag."}},
    {"row_gpu_marks", {"GPU markers", "Метки видеокарты"}},
    {"gpu_marks_note", {"If the graphics card fails, the log says what it was doing last. May slow the game a little.",
                        "Если видеокарта упадёт, в логе будет её последняя команда. Может немного замедлить игру."}},
    {"full_title", {"Rebuild the game completely", "Полная пересборка игры"}},
    {"full_time", {"May take {lo}-{hi} minutes", "Может занять {lo}-{hi} минут"}},
    {"full_btn", {"Rebuild completely", "Пересобрать полностью"}},
    {"full_q", {"Rebuild everything from scratch? It takes {lo}-{hi} minutes.",
                "Пересобрать всё заново? Это займёт {lo}-{hi} минут."}},
    {"full_go", {"Rebuild", "Пересобрать"}},
    {"sec_screen", {"Display", "Экран"}},
    {"sec_ui_lang", {"Launcher language", "Язык лаунчера"}},
    {"sec_game_lang", {"Game language", "Язык игры"}},
    {"row_ui_text", {"Text", "Текст"}},
    {"row_window", {"Window mode", "Режим окна"}},
    {"win_full", {"Fullscreen", "Полный экран"}},
    {"win_borderless", {"Borderless", "Без рамки"}},
    {"win_window", {"Window", "Окно"}},
    {"row_res", {"Resolution", "Разрешение"}},
    {"row_scale", {"Scene scale", "Масштабирование"}},
    {"scale_perf", {"Performance (50%)", "Производительность (50%)"}},
    {"scale_bal", {"Balanced (59%)", "Баланс (59%)"}},
    {"scale_qual", {"Quality (67%)", "Качество (67%)"}},
    {"scale_ultra", {"Ultra quality (77%)", "Ультра качество (77%)"}},
    {"scale_game", {"As the game (83%)", "Как в игре (83%)"}},
    {"scale_150", {"Supersampling 150%", "Суперсэмплинг 150%"}},
    {"scale_200", {"Supersampling 200%", "Суперсэмплинг 200%"}},
    {"res_display", {"As the screen", "Как у экрана"}},
    {"aa_game", {"FSR 1 (the game's)", "FSR 1 (как в игре)"}},
    {"up_none", {"None", "Нет"}},
    {"row_aa", {"Anti-aliasing", "Сглаживание"}},
    {"aa_fxaa", {"FXAA (the game's)", "FXAA (как в игре)"}},
    {"aa_off", {"Off", "Выкл"}},
    {"row_sharp", {"Sharpness", "Резкость"}},
    {"sharp_off", {"Off", "Выкл"}},
    {"sharp_default", {"50% (default)", "50% (по умолчанию)"}},
    {"row_fps", {"Frame limit", "Ограничение кадров"}},
    {"fps_120", {"120 (beta)", "120 (бета)"}},
    {"fps_144", {"144 (beta)", "144 (бета)"}},
    {"fps_display", {"Display rate (beta)", "Как у монитора (бета)"}},
    {"fps_unlimited", {"Up to 240 (beta)", "До 240 (бета)"}},
    {"row_vsync", {"Vertical sync", "Вертикальная синхронизация"}},
    {"row_counter", {"In-game FPS counter", "Счётчик FPS в игре"}},
    {"counter_key", {"F10 turns it on or off", "F10 включение или отключение"}},
    {"row_text", {"Text and subtitles", "Текст и субтитры"}},
    {"lang_system", {"As in the system", "Как в системе"}},
    {"lang_ja", {"Japanese", "Японский"}},
    {"lang_ko", {"Korean", "Корейский"}},
    {"lang_zh", {"Chinese (Simplified)", "Китайский (упрощённый)"}},
    {"upd_avail", {"Update {v} is available", "Доступно обновление {v}"}},
    {"cur_ver", {"Current port version {v}", "Текущая версия порта {v}"}},
    {"port_ver", {"Port version {v}", "Версия порта {v}"}},
    {"play", {"Play", "Играть"}},
    {"play_failed", {"The game did not start.", "Не удалось запустить игру."}},
    {"pick_title", {"Choose a folder", "Выбор папки"}},
    {"pick_up", {"Up", "Вверх"}},
    {"pick_home", {"Home", "Домой"}},
    {"pick_media", {"SD card / USB", "SD-карта / USB"}},
    {"pick_this", {"Choose this folder", "Выбрать эту папку"}},
    {"pick_empty", {"No folders here", "Здесь нет папок"}},
    {"gfx_check", {"Checking the graphics…", "Проверка графики…"}},
    {"gfx_prep", {"Preparing the graphics for this graphics card: {done} of {total}",
                  "Подготовка графики под видеокарту: {done} из {total}"}},
    {"gfx_note", {"Once per graphics driver, so the game runs without stutters.",
                  "Один раз для каждого драйвера, чтобы игра шла без рывков."}},
    // the exe alone, without the rest of the zip: run from inside the zip, or copied somewhere by itself
    {"no_python", {"The launcher's files are missing: extract the whole archive and start the launcher from the extracted folder.",
                   "Не найдены файлы лаунчера: распакуйте архив целиком и запускайте лаунчер из распакованной папки."}},
    {"setup_crash", {"The builder stopped unexpectedly (exit code {code}).", "Сборщик неожиданно остановился (код {code})."}},
    {"error_generic", {"error: the details are in the log", "ошибка — подробности в логе"}},
    {"min", {"{m} min", "{m} мин"}},
    {"ms", {"{m}m {s}s", "{m}м {s}с"}},
    {"s", {"{s}s", "{s}с"}},
    {"hm", {"{h}h {m}m", "{h}ч {m}м"}},

    // the dump (setup.py --check-dump)
    {"dump_missing", {"The folder does not exist", "Папка не найдена"}},
    {"dump_missing.d", {"Choose the folder that holds exefs and romfs of the game, version 1.4.3.",
                        "Выберите папку, в которой лежат exefs и romfs игры версии 1.4.3."}},
    {"dump_encrypted", {"This is an encrypted game package", "Это зашифрованный файл игры"}},
    {"dump_encrypted.d", {"{file}: the extracted exefs and romfs folders are needed. UltraTears decrypts nothing.",
                          "{file}: нужны уже извлечённые папки exefs и romfs. UltraTears ничего не расшифровывает."}},
    {"dump_no_folders", {"No game of version 1.4.3 was found in this folder", "В этой папке не найдена игра версии 1.4.3"}},
    {"dump_no_folders.d", {"Choose another folder: the one that holds exefs and romfs (extracted, as plain files).",
                           "Выберите другую папку — ту, в которой лежат exefs и romfs (распакованные, обычными файлами)."}},
    {"dump_incomplete", {"The game is not extracted completely", "Игра извлечена не полностью"}},
    {"dump_incomplete.d", {"{file} is missing. Extract the game again.", "Не хватает {file}. Извлеките игру заново."}},
    {"dump_version", {"This version of the game does not fit", "Эта версия игры не подходит"}},
    {"dump_version.d", {"Version 1.4.3 is needed. Update the original game to 1.4.3 or choose another folder.",
                        "Нужна версия 1.4.3. Обновите оригинальную игру до 1.4.3 или укажите другой путь."}},
    {"dump_not_totk", {"This is not Tears of the Kingdom", "Это не Tears of the Kingdom"}},
    {"dump_not_totk.d", {"The romfs folder has none of this game's files.", "В папке romfs нет файлов этой игры."}},
    {"romfs_version", {"the RomFS is not of version 1.4.3", "RomFS не от версии 1.4.3"}},
    {"tools", {"the build tools could not be prepared", "не удалось подготовить инструменты сборки"}},
    {"linux_kit_missing", {"the Linux kit (deps\\linux) is missing: download the release again and extract it over this folder",
                           "нет набора для Linux (deps\\linux): скачайте релиз заново и распакуйте поверх этой папки"}},
    {"linux_kit_gone", {"No Linux kit in this folder:\ndownload the release again", "Нет набора для Linux:\nскачайте релиз заново"}},
    {"disk_space", {"not enough disk space: {need_gb} GB needed on {drive}, {free_gb} GB free",
                    "не хватает места: нужно {need_gb} GB на {drive}, свободно {free_gb} GB"}},

    // the hardware (tools/hw_check.py)
    {"no_vulkan", {"No Vulkan graphics driver was found", "Не найден графический драйвер Vulkan"}},
    {"no_vulkan.d", {"Install the current driver for your NVIDIA, AMD or Intel card.",
                     "Установите свежий драйвер видеокарты NVIDIA, AMD или Intel."}},
    {"vulkan_old", {"The {gpu} driver offers Vulkan {version}; 1.3 is needed", "Драйвер {gpu} даёт Vulkan {version}, нужен 1.3"}},
    {"vulkan_old.d", {"Updating the graphics driver usually helps.", "Обычно помогает обновление драйвера видеокарты."}},
    {"no_shader_object", {"{gpu} lacks VK_EXT_shader_object", "{gpu} не поддерживает VK_EXT_shader_object"}},
    {"no_shader_object.d", {"Usual for AMD RX 400/500, Vega and Intel HD: such cards are not supported yet.",
                            "Так бывает у AMD RX 400/500, Vega и Intel HD — такие видеокарты пока не поддерживаются."}},
    {"no_shader_object.layer", {"It can run through the installed Khronos shader object layer: slower and with glitches.",
                                "Игра может работать через установленный слой Khronos shader object — медленнее и с ошибками "
                                "изображения."}},
    {"no_ext_mem_host", {"The {gpu} driver lacks VK_EXT_external_memory_host", "Драйвер {gpu} не поддерживает VK_EXT_external_memory_host"}},
    {"no_ext_mem_host.d", {"Update the graphics driver.", "Обновите драйвер видеокарты."}},
    {"no_swapchain", {"{gpu} cannot show pictures", "{gpu} не умеет выводить изображение"}},
    {"no_swapchain.d", {"It is a compute device without a display output.", "Это вычислительное устройство без вывода на экран."}},
    {"vram_low", {"{gpu}: {gb} GB of video memory", "{gpu}: {gb} GB видеопамяти"}},
    {"vram_low.d", {"4 GB or more is recommended: the game may stutter.", "Рекомендуется 4 GB и больше — возможны подтормаживания."}},
    {"ram_low", {"{gb} GB of RAM", "Оперативной памяти {gb} GB"}},
    {"ram_low.d", {"16 GB or more is recommended.", "Рекомендуется 16 GB и больше."}},
    {"cpu_no_avx2", {"The CPU lacks AVX2", "Процессор без AVX2"}},
    {"cpu_no_avx2.d", {"The game is built for SSE4.2 and runs a little slower.", "Игра соберётся под SSE4.2 и будет работать немного медленнее."}},
    {"kernel_old", {"Linux {version}: 6.7 or newer is needed", "Ядро Linux {version}: нужно 6.7 или новее"}},
    {"kernel_old.d", {"The game cannot track its memory writes and will run very slowly. Update the system.",
                      "Игра не сможет отслеживать записи в свою память и будет работать очень медленно. Обновите систему."}},
    {"amd_untested", {"{gpu}: AMD cards are not tested yet", "{gpu}: видеокарты AMD ещё не проверены"}},
    {"amd_untested.d", {"Please tell how the game runs: it helps.", "Расскажите, как идёт игра, — это поможет."}},
    // ---- the game's settings overlay (src/overlay): its own words, and each row's description (<row>.desc)
    {"ovl_select", {"Select", "Выбрать"}},
    {"ovl_back", {"Back", "Назад"}},
    {"ovl_quit", {"Quit game", "Выйти из игры"}},
    {"ovl_restart_title", {"Restart the game?", "Перезапустить игру?"}},
    {"ovl_restart_text", {"Some of the changes take effect only after a restart. Unsaved progress will be lost.",
                          "Часть изменений вступит в силу только после перезапуска. Несохранённый прогресс будет потерян."}},
    {"ovl_restart_now", {"Restart now", "Перезапустить"}},
    {"ovl_later", {"Later", "Позже"}},
    {"ovl_quit_title", {"Quit the game?", "Выйти из игры?"}},
    {"ovl_quit_text", {"Unsaved progress will be lost.", "Несохранённый прогресс будет потерян."}},
    {"ovl_quit_go", {"Quit", "Выйти"}},
    {"ovl_applies_now", {"Applies at once.", "Применяется сразу."}},
    {"ovl_after_restart", {"after a restart", "после перезапуска"}},
    {"ovl_applies_restart", {"Applies after a restart of the game.", "Применяется после перезапуска игры."}},
    {"ctl_applies_now", {"Changes apply at once. A mouse button can be pressed in place of a key; Esc cancels waiting.",
                         "Изменения применяются сразу. Вместо клавиши можно нажать кнопку мыши; Esc отменяет ожидание."}},
    {"sticks_note_now", {"A stick let go and a gamepad lying still stay in the grey. Changes apply at once.",
                         "Отпущенный стик и лежащий геймпад остаются в сером круге. Изменения применяются сразу."}},
    {"mods_note_game", {"The higher mod wins a file two mods change. Changes apply after a restart of the game.",
                        "Если два мода меняют один файл, побеждает тот, что выше. Изменения — после перезапуска игры."}},
    {"tab_main.desc", {"How the game is shown: window, resolution, image quality and frame rate. Point at a setting to read what it does.",
                       "Как показывается игра: окно, разрешение, качество картинки и частота кадров. Наведите на настройку, чтобы прочитать, что она делает."}},
    {"tab_controls.desc", {"Which buttons and keys do what, the sticks' dead zones, the gyro and the mouse. The bindings also drive this menu.",
                           "Какие кнопки и клавиши что делают, мёртвые зоны стиков, гироскоп и мышь. Это меню тоже управляется этими назначениями."}},
    {"tab_mods.desc", {"Mods from the mods folder: switch them on or off, change their order, switch cheats. The game reads them when it starts.",
                       "Моды из папки mods: включение, порядок, читы. Игра читает их при запуске."}},
    {"tab_debug.desc", {"For reports of problems: more lines in the log and snapshots of frames. Not needed for playing.",
                        "Для отчётов о проблемах: больше строк в логе и снимки кадров. Для игры не нужно."}},
    {"row_window.desc", {"Fullscreen takes the screen over and may change its mode; Borderless is a window the size of the screen, quick to "
                         "switch away from; Window can be moved and resized. F11 switches too.",
                         "Полный экран занимает экран целиком; Без рамки — окно размером с экран, из него быстрее переключаться в "
                         "другие программы; Окно можно двигать и менять его размер. F11 тоже переключает."}},
    {"row_res.desc", {"The size of the picture the game composes, its menus included. More is sharper and costs the graphics card "
                      "more; the screen's own size looks best. Another shape than 16:9 widens the view.",
                      "Размер картинки, которую собирает игра, вместе с меню. Больше — чётче, но тяжелее для видеокарты; лучше "
                      "всего смотрится родное разрешение экрана. Форма шире 16:9 расширяет обзор."}},
    {"row_scale.desc", {"The 3D scene drawn smaller and scaled up to the resolution: the biggest effect on the frame rate. FSR 3.1 "
                        "also smooths edges over several frames and keeps more detail at the same scale; FSR 1 is the game's own.",
                        "3D-сцена рисуется меньше и растягивается до разрешения — сильнее всего влияет на частоту кадров. FSR 3.1 "
                        "ещё и сглаживает края по нескольким кадрам и при том же масштабе сохраняет больше деталей; FSR 1 — как в "
                        "самой игре."}},
    {"row_aa.desc", {"Smooths jagged edges. FXAA is the game's own, cheap and slightly soft; SMAA is sharper for a little more work.",
                     "Сглаживает ступеньки на краях. FXAA — как в игре, дешёвое и немного мылит; SMAA чётче и чуть тяжелее."}},
    {"row_sharp.desc", {"Sharpening of the final picture (AMD's RCAS). Higher brings out detail after scaling; too high shows halos "
                        "around edges.",
                        "Повышение резкости итоговой картинки (RCAS от AMD). Больше — заметнее детали после масштабирования; "
                        "слишком много даёт ореолы вокруг краёв."}},
    {"row_fps.desc", {"The most frames a second the game shows. Above 60 is a beta: the game itself was not made for it, and the "
                      "camera or climbing may behave oddly in places. Switching to or from 30 applies after a restart.",
                      "Сколько кадров в секунду игра показывает самое большее. Выше 60 — бета: сама игра на это не рассчитана, "
                      "местами странно ведут себя камера или лазание. Переход на 30 и обратно — после перезапуска."}},
    {"row_vsync.desc", {"On: whole frames only, no tearing. Off: frames are shown at once, a little less input delay, but the "
                        "picture may tear.",
                        "Вкл: только целые кадры, без разрывов. Выкл: кадры показываются сразу, чуть меньше задержка ввода, но "
                        "картинка может рваться."}},
    {"row_counter.desc", {"Frames per second and their time in the top right corner. F10 switches it in the game.",
                          "Кадры в секунду и их время в правом верхнем углу. В игре переключается клавишей F10."}},
    {"row_layout.desc", {"Which face button is A: on the right as on a Switch controller, or at the bottom as on Xbox pads and the "
                         "Steam Deck.",
                         "Какая из правых кнопок — A: справа, как на контроллере Switch, или снизу, как на геймпадах Xbox и Steam Deck."}},
    {"act.desc", {"Select the field, then press the button or key for this action; the action that had it gets this one's in "
                  "exchange. The arrow at the left puts the default back.",
                  "Выберите поле и нажмите кнопку или клавишу для этого действия; действие, у которого она была, получит "
                  "взамен прежнюю. Стрелка слева возвращает значение по умолчанию."}},
    {"row_deadzone.desc", {"How far a stick may lean, or the gyro turn, before the game takes it as moved. Raise it if Link walks "
                           "or the aim drifts on its own.",
                           "Насколько стик может отклониться или гироскоп повернуться, прежде чем игра сочтёт это движением. "
                           "Увеличьте, если Линк идёт сам или прицел уплывает."}},
    {"row_mouse_sens.desc", {"How fast the mouse turns the camera.", "Как быстро мышь поворачивает камеру."}},
    {"row_mouse_aim.desc", {"The mouse's speed while a bow is drawn, as a share of the camera's.",
                            "Скорость мыши при натянутом луке — как доля от скорости камеры."}},
    {"row_mouse_invert.desc", {"Moving the mouse up looks down.", "Мышь вверх — взгляд вниз."}},
    {"row_debug.desc", {"Statistics lines in the log and a console window beside the game: for reports of problems.",
                        "Строки статистики в логе и окно консоли рядом с игрой — для отчётов о проблемах."}},
    {"row_snapshots.desc", {"F9 saves the frame's draws and textures into the diag folder: for picture bugs. Takes disk space.",
                            "F9 сохраняет отрисовки и текстуры кадра в папку diag — для ошибок картинки. Занимает место на диске."}},
    {"row_gpu_marks.desc", {"If the graphics card fails, the log says what it was doing last. May slow the game a little.",
                            "Если видеокарта сбоит, лог покажет, чем она занималась последней. Может немного замедлить игру."}},
};

static const Text *find(const char *key) {
    for (const Text &t : TEXTS)
        if (std::strcmp(t.key, key) == 0)
            return &t;
    return nullptr;
}

bool has_text(const char *key) { return find(key) != nullptr; }

const char *T(const char *key) {
    const Text *t = find(key);
    if (!t)
        return key;
    return t->s[g_lang] ? t->s[g_lang] : t->s[LANG_EN];
}

static std::string fill(const char *text, const std::vector<std::pair<std::string, std::string>> &args) {
    std::string out;
    for (const char *p = text; *p;) {
        if (*p == '{') {
            const char *e = std::strchr(p, '}');
            if (e) {
                std::string name(p + 1, e);
                bool found = false;
                for (auto &a : args)
                    if (a.first == name) {
                        out += a.second;
                        found = true;
                        break;
                    }
                if (found) {
                    p = e + 1;
                    continue;
                }
            }
        }
        out += *p++;
    }
    return out;
}

std::string Tf(const char *key, std::initializer_list<std::pair<const char *, std::string>> args) {
    std::vector<std::pair<std::string, std::string>> v;
    for (auto &a : args)
        v.emplace_back(a.first, a.second);
    return fill(T(key), v);
}

std::string Tf(const char *key, const Json &args) {
    std::vector<std::pair<std::string, std::string>> v;
    for (size_t i = 0; i < args.keys.size(); i++)
        v.emplace_back(args.keys[i], args.items[i].str());
    return fill(T(key), v);
}

const char *lang_name(Lang l) { return l == LANG_RU ? "Русский" : "English"; }
const char *lang_code(Lang l) { return l == LANG_RU ? "ru" : "en"; }
Lang lang_from_code(const char *code) { return code && SDL_strncasecmp(code, "ru", 2) == 0 ? LANG_RU : LANG_EN; }

std::string fmt_duration(double secs) {
    long s = (long)(secs + 0.5);
    if (s < 60)
        return Tf("s", {{"s", std::to_string(s)}});
    if (s < 3600 && s % 60 == 0)
        return Tf("min", {{"m", std::to_string(s / 60)}});
    if (s < 3600)
        return Tf("ms", {{"m", std::to_string(s / 60)}, {"s", std::to_string(s % 60)}});
    return Tf("hm", {{"h", std::to_string(s / 3600)}, {"m", std::to_string(s / 60% 60)}});
}

std::string fmt_minutes(double secs) {
    long m = (long)(secs / 60 + 0.5);
    return Tf("min", {{"m", std::to_string(m < 1 ? 1 : m)}});
}
