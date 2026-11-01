<p align="center"><a href="README.md">English</a> · <b>Русский</b></p>

<p align="center"><img src="assets/cover.png" alt="UltraTears" width="100%"></p>

# UltraTears

**UltraTears** собирает нативную ПК-версию *The Legend of Zelda: Tears of the Kingdom* (версия 1.4.3) для Windows и
Linux / Steam Deck из вашей собственной копии игры. Это не эмулятор: код игры заранее переводится в нативный, а шейдеры и
текстуры готовятся при сборке игры, а не во время игры.

> [!IMPORTANT]
> **Файлов игры здесь нет.** Нужна ваша собственная копия игры с обновлением 1.4.3, снятая с вашей консоли в виде обычных
> папок `exefs` и `romfs`. Проект не даёт файлов игры, ключей, прошивки и инструментов для дампа, а UltraTears ничего
> не расшифровывает. Всё, что делается из игры, делается на вашем ПК и только для вас: не распространяйте это.
>
> UltraTears не связан с Nintendo и не одобрен ею. *The Legend of Zelda* и *Tears of the Kingdom* — товарные знаки
> Nintendo.

О проблемах пишите в [Issues](https://github.com/kuraeow/ultratears/issues) и прикладывайте `ultratears.log` из папки
игры.

## Установка

**Нужно:** Windows 10/11 или Linux (ядро 6.7 и новее, glibc 2.34 и новее; в SteamOS 3 есть оба), видеокарта со свежим
драйвером Vulkan 1.3, 16 ГБ ОЗУ, 4 ГБ видеопамяти, около 40 ГБ свободного места и ваш дамп:

```text
dump\
├── exefs\   main, subsdk0, sdk, main.npdm, ...
└── romfs\   около 304 000 файлов, 16 ГБ
```

### Windows

1. Скачайте `UltraTears-builder-<версия>-windows-x86_64.zip` со страницы [Releases](https://github.com/kuraeow/ultratears/releases)
   и распакуйте в отдельную папку.
2. Запустите `UltraTears Launcher.exe`. Если SmartScreen предупреждает, нажмите **Подробнее → Выполнить в любом случае**.
3. Выберите папку с `exefs` и `romfs` и нажмите **Подготовить игру**. Игра соберётся один раз, примерно за 30 минут.
4. Нажмите **Играть**.

Чтобы играть на Steam Deck, рядом с **Подготовить игру** выберите **Для Linux (Steam Deck)** или **Обе платформы**:
получится `UltraTears-SteamDeck.tar` (около 28 ГБ). Распакуйте его на Deck и запускайте, как ниже, с шага 4.

### Linux и Steam Deck

1. Скачайте `UltraTears-<версия>-x86_64.AppImage` со страницы [Releases](https://github.com/kuraeow/ultratears/releases).
   На Steam Deck — в режиме рабочего стола.
2. Разрешите его запуск (Свойства → Права) и запустите.
3. Выберите папку с `exefs` и `romfs` и нажмите **Подготовить игру**.
4. Нажмите **Играть**. Для игрового режима добавьте `UltraTears.AppImage` из папки игры в Steam как стороннюю игру и
   впишите `--play` в параметры запуска.

Новые версии лаунчер находит сам и пересобирает только изменённое; сохранения в `userdata` остаются.

<details>
<summary>Управление</summary>

Работают геймпады Xbox, DualSense, Switch Pro и другие; кнопки расположены как на Switch (A — правая кнопка).

Клавиатура: WASD — движение, стрелки — камера, J = A, K / пробел = B, I = X, U = Y, Q / E = L / R, Alt / Shift =
ZL / ZR, Enter = +, Tab / Esc = − (карта), T F G H = крестовина, Z / C = нажатие стиков, F11 или Alt+Enter — полный
экран.
</details>

## Сборка из исходников

Установите Git и Python 3, затем:

```powershell
git clone https://github.com/kuraeow/ultratears.git
cd ultratears
python tools/setup.py --dump D:\путь\к\дампу
```

Сборщик сам скачает недостающие инструменты и соберёт игру в `build\` (`build\UltraTears.exe`). Без `--dump` дамп
берётся из папки `dump\` в репозитории. Параметры, Linux и устройство сборки: [BUILDING.md](BUILDING.md). Как
предложить изменения: [CONTRIBUTING.md](CONTRIBUTING.md).

## Правовое

- В репозитории и в релизах нет ничего из игры: ни кода (в том числе переведённого из неё), ни ресурсов, шейдеров,
  текстур, шрифтов, ключей или прошивки. Всё это делается из вашего дампа на вашем ПК.
- Код и заголовки Nintendo SDK не используются: системный и графический слои — собственные.
- В исходниках только факты, нужные для запуска: адреса, хеши, короткие замены инструкций.
- Название, эмблема, иконка и обложка — собственные работы автора и не под GPL: [assets/BRANDING.md](assets/BRANDING.md).
  Сторонний код: [third_party/README.md](third_party/README.md).
