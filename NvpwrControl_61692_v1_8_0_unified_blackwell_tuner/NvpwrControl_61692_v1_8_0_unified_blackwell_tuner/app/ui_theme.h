#pragma once
// Presentation only: no device operations, services, power setters or driver addresses.
#include <map>
#include <cwchar>
#include <string>
#include <sstream>
#include <iomanip>

static bool g_uiRussian = false;
static int g_uiDpi = 96;
static HFONT g_uiFont = nullptr, g_uiHeadingFont = nullptr, g_uiStateFont = nullptr;
static HFONT g_uiMetricFont = nullptr, g_uiButtonFont = nullptr, g_uiMonoFont = nullptr, g_uiSmallFont = nullptr;
static HBRUSH g_uiBackground = nullptr, g_uiCard = nullptr, g_uiConsole = nullptr;
static HPEN g_uiCardPen = nullptr;
static HWND g_title = nullptr, g_subtitle = nullptr, g_label = nullptr, g_hint = nullptr;
static HWND g_details = nullptr, g_note = nullptr, g_footer = nullptr, g_language = nullptr;
static std::map<HWND, std::wstring> g_uiText;
static HWND g_summaryLabel[3]{}, g_summaryValue[3]{}, g_summaryHint[3]{};
static COLORREF g_uiStateColor = RGB(56, 189, 248);

static const COLORREF kUiBackground   = RGB(14, 18, 25);
static const COLORREF kUiCard         = RGB(22, 29, 41);
static const COLORREF kUiCardBorder   = RGB(42, 54, 76);
static const COLORREF kUiConsoleBg    = RGB(10, 13, 19);
static const COLORREF kUiNvidiaGreen  = RGB(118, 185, 0);
static const COLORREF kUiCyan         = RGB(56, 189, 248);
static const COLORREF kUiTextWhite    = RGB(248, 250, 252);
static const COLORREF kUiTextMuted    = RGB(148, 163, 184);

struct UiTranslation { const wchar_t* en; const wchar_t* ru; };
static const UiTranslation kUiTranslations[] = {
    { L"1.8.0 Unified Ada & Blackwell Tuner  /  RTX 40 & 50 Series TDP Unlock", L"1.8.0 Unified Ada & Blackwell Tuner  /  Разблокировка TDP для RTX 40 и 50 серий" },
    { L"ADVANCED BLACKWELL TUNING", L"РАСШИРЕННЫЙ РАЗГОН BLACKWELL" },
    { L"Apply tuning", L"Применить разгон" },
    { L"Reset tuning", L"Сбросить разгон" },
    { L"Restart NVIDIA device", L"Перезапустить NVIDIA" },
    { L"Core offset", L"Смещение Core" },
    { L"Memory offset", L"Смещение памяти" },
    { L"XBAR offset", L"Смещение XBAR" },
    { L"MSVDD offset", L"Смещение MSVDD" },
    { L"NVVDD offset", L"Смещение NVVDD" },
    { L"GPC:XBAR ratio", L"Коэффициент GPC:XBAR" },
    { L"RTX 4090: 150-250 W. 4080: 150-225 W. 4050/4060/4070: 115-150 W. RTX 5050/5060/5070: 115-140 W. 5070 Ti: 145-180 W. 5080/5090: 175-225 W (250 W via XMG v8).", L"RTX 4090: 150-250 Вт. 4080: 150-225 Вт. 4050/4060/4070: 115-150 Вт. RTX 5050/5060/5070: 115-140 Вт. 5070 Ti: 145-180 Вт. 5080/5090: 175-225 Вт (250 Вт через XMG v8)." },
    { L"1.8.0 Unified Ada & Blackwell Tuner  /  Per-Monitor DPI V2 + coherent TGP + writable NvAPI tuning", L"1.8.0 Unified Ada & Blackwell Tuner  /  Per-Monitor DPI V2 + coherent TGP + управление NvAPI" },
    { L"Restart Nvpwr driver", L"Перезапустить драйвер Nvpwr" },
    { L"Driver mode help", L"Режим драйвера" },
    { L"Tuning capability probe:", L"Проверка возможностей разгона:" },
    { L"Core offset: planned via NVIDIA-supported clock controls; runtime range must be queried.", L"Core offset: планируется через интерфейс управления частотами NVIDIA; диапазон должен определяться во время работы." },
    { L"Memory offset: planned; +5000 MHz is enabled only if the selected GPU/driver reports it.", L"Memory offset: +5000 МГц будет доступно только если выбранные GPU/драйвер реально сообщают такой диапазон." },
    { L"XBAR/MSVDD/GPC:XBAR: integration path known, but driver 616.92 must pass a read-only NvAPI layout probe first.", L"XBAR/MSVDD/GPC:XBAR: путь интеграции известен, но драйвер 616.92 сначала должен пройти read-only проверку структуры NvAPI." },
    { L"Entry13/Entry14: no standalone slider; writes are allowed only inside the validated XMG coherent 250 W transaction.", L"Entry13/Entry14: отдельного ползунка нет; запись разрешена только внутри валидированной согласованной транзакции XMG 250 Вт." },
    { L"NVVDD/MSVDD: no arbitrary overvoltage; only driver-reported offset/limit controls with readback.", L"NVVDD/MSVDD: без произвольного overvoltage; только диапазоны/смещения, сообщённые драйвером, с readback." },
    { L"The driver will verify NVIDIA policy readback and roll back on a failed transition, but it cannot guarantee hardware safety.", L"Драйвер проверит чтение политики NVIDIA и попытается вернуть исходное состояние при неудачном переходе. Это не гарантирует безопасность оборудования." },
    { L"5070 Ti: previous physical tests cover 145, 150 and 160 W only. Other targets and 5080/5090 profiles are unvalidated.", L"5070 Ti: физические тесты ранее проводились только для 145, 150 и 160 Вт. Остальные значения и профили 5080/5090 не проверены." },
    { L"EXPERIMENTAL / NOT LOAD-VALIDATED on this GPU profile. This can exceed the laptop OEM electrical and thermal design.", L"ЭКСПЕРИМЕНТАЛЬНО / НЕ ПРОВЕРЕНО ПОД НАГРУЗКОЙ на этом профиле GPU. Значение может превышать расчётные возможности питания и охлаждения ноутбука." },
    { L"140 W restores the original runtime baseline. The driver and power-control logic are unchanged.", L"140 Вт возвращает исходное состояние. Драйвер и алгоритм управления не изменены." },
    { L"If this is an unrecognized external state, reboot Windows to reconstruct stock NVIDIA policy.", L"Если внешнее состояние не распознано, перезагрузите Windows для восстановления штатной политики NVIDIA." },
    { L"This build is test-signed and requires TESTSIGNING ON with Secure Boot disabled.", L"У этой сборки тестовая подпись. Windows не разрешила загрузку. Проверьте код ошибки выше." },
    { L"This is above the factory 140 W ceiling. Monitor GPU temperature and stability.", L"Это выше заводского предела 140 Вт. Контролируйте температуру GPU и стабильность." },
    { L"No further write is attempted. Use Restore stock or reboot if status is mixed.", L"Дальнейшая запись не выполняется. При смешанном состоянии верните штатные настройки или перезагрузите Windows." },
    { L"Choose a target, then click Apply. Selecting a value alone does not apply it.", L"Выбери мощность и нажми «Применить». Сам выбор значения ничего не меняет." },
    { L"Checked next to NvpwrControl.exe and the project dist/driver build folders.", L"Проверены папка NvpwrControl.exe и каталоги сборки dist/driver." },
    { L" is absent. Reboot once, then run NvpwrControl.exe as administrator.", L" отсутствует. Перезагрузите Windows и запустите NvpwrControl.exe от имени администратора." },
    { L"UI refresh 1.5.0  /  Existing driver and control logic unchanged", L"Обновление интерфейса 1.5.0  /  Исходный драйвер и алгоритм не изменены" },
    { L"Driver service stopped while starting. Win32 service exit=", L"Служба драйвера остановилась при запуске. Код Win32=" },
    { L"Run build.ps1 again and make sure Nvpwr.sys was produced.", L"Проверьте результат сборки и наличие файла Nvpwr.sys." },
    { L"UI 1.5  •  Driver compatibility: original 1.4.1 rules", L"Интерфейс 1.5  •  Проверка совместимости: исходные правила 1.4.1" },
    { L"Could not restart stale Nvpwr driver (stop failed):", L"Не удалось остановить ранее загруженный Nvpwr:" },
    { L"Stock 140 W could not be verified after restore.", L"После восстановления не удалось подтвердить штатные 140 Вт." },
    { L"Timed out waiting for driver service state ", L"Истекло время ожидания состояния службы " },
    { L"Run NvpwrControl.exe as administrator.", L"Запустите NvpwrControl.exe от имени администратора." },
    { L"RTX 5070 Ti Laptop  /  NVIDIA 616.92", L"RTX 5070 Ti Laptop  /  NVIDIA 616.92" },
    { L"Normalizing to stock 140 W first...", L"Предварительный возврат к штатным 140 Вт…" },
    { L"Nvpwr service reached RUNNING but ", L"Служба Nvpwr запущена, но " },
    { L"Confirm experimental power target", L"Подтверждение выбранной мощности" },
    { L"Detecting GPU / OEM baseline...", L"Определение GPU / OEM-лимита…" },
    { L"ARMED / staging at OEM ceiling", L"ПОДГОТОВЛЕНО / промежуточный OEM-лимит" },
    { L"Preparing power transition...", L"Подготовка изменения мощности…" },
    { L"QueryServiceStatusEx failed:", L"Ошибка QueryServiceStatusEx:" },
    { L"Existing application profile", L"Профиль исходного приложения" },
    { L"Driver could not be started", L"Не удалось запустить драйвер" },
    { L"MIXED - click Restore stock", L"СМЕШАННОЕ СОСТОЯНИЕ — верните штатные настройки" },
    { L"ChangeServiceConfig failed:", L"Ошибка ChangeServiceConfig:" },
    { L"Last successful status read", L"Последнее успешное чтение" },
    { L"UNSUPPORTED NVIDIA DRIVER", L"НЕПОДДЕРЖИВАЕМЫЙ ДРАЙВЕР NVIDIA" },
    { L"Nvpwr driver is not open.", L"Соединение с драйвером Nvpwr не открыто." },
    { L"Restoring stock 140 W...", L"Возврат к штатным 140 Вт…" },
    { L"ARMED / staging at 140 W", L"ПОДГОТОВЛЕНО / промежуточные 140 Вт" },
    { L"Nvpwr.sys was not found.", L"Файл Nvpwr.sys не найден." },
    { L"Not applied by selection", L"Выбор не применяет настройку" },
    { L"Flags init/elig/amount:", L"Флаги init/elig/amount:" },
    { L"nvlddmkm.sys not found", L"nvlddmkm.sys не найден" },
    { L"PRECONDITION NOT READY", L"ИСХОДНЫЕ УСЛОВИЯ НЕ ГОТОВЫ" },
    { L"GPU context not found", L"Контекст GPU не найден" },
    { L"OpenSCManager failed:", L"Ошибка OpenSCManager:" },
    { L"CreateService failed:", L"Ошибка CreateService:" },
    { L"StartService failed (", L"Ошибка StartService (" },
    { L"Unsupported / unknown", L"Не поддерживается / не определён" },
    { L"Status query failed:", L"Ошибка запроса состояния:" },
    { L"Detected timestamp:", L"Метка времени драйвера:" },
    { L"Confirm power limit", L"Подтверждение мощности" },
    { L"OpenService failed:", L"Ошибка OpenService:" },
    { L"Status and details", L"Состояние и подробности" },
    { L"Starting driver...", L"Запуск драйвера…" },
    { L"Driver unavailable", L"Драйвер недоступен" },
    { L"Restoring stock...", L"Возврат штатных настроек…" },
    { L"Current effective:", L"Действующий лимит:" },
    { L"Set power failed (", L"Ошибка установки мощности (" },
    { L"GPU power settings", L"Настройки мощности GPU" },
    { L"Unknown NVIDIA GPU", L"GPU NVIDIA не определён" },
    { L"GPU POWER CONTROL", L"УПРАВЛЕНИЕ МОЩНОСТЬЮ GPU" },
    { L"Technical details", L"Технические сведения" },
    { L"Base / F7 input:", L"База / вход F7:" },
    { L"Restore failed (", L"Ошибка восстановления (" },
    { L"CONTEXT INVALID", L"КОНТЕКСТ НЕКОРРЕКТЕН" },
    { L"Dynamic amount:", L"Динамическая добавка:" },
    { L"Selected target", L"Выбранное значение" },
    { L"Supported KMD:", L"Поддерживаемый KMD:" },
    { L"Upper ceiling:", L"Верхняя граница:" },
    { L"MAX effective:", L"Действующий MAX:" },
    { L"NVIDIA status:", L"Статус NVIDIA:" },
    { L", driver exit=", L", код драйвера=" },
    { L"No status data", L"Нет данных о состоянии" },
    { L"Restore stock", L"Вернуть штатные" },
    { L"140 W — Stock", L"140 Вт — Штатные" },
    { L"Refreshing...", L"Обновление…" },
    { L"Predicted F7:", L"Расчётный F7:" },
    { L"Unknown error", L"Неизвестная ошибка" },
    { L"NVPWR CONTROL", L"NVPWR CONTROL" },
    { L"OEM baseline:", L"Штатный OEM-лимит:" },
    { L"Target power", L"Выбор мощности" },
    { L"Status error", L"Ошибка чтения состояния" },
    { L"Cannot open ", L"Не удалось открыть " },
    { L"Active limit", L"Действующий лимит" },
    { L"OEM baseline", L"Штатный OEM-лимит" },
    { L"EXPERIMENTAL", L"ЭКСПЕРИМЕНТАЛЬНО" },
    { L" — verified", L" — проверено ранее" },
    { L"STOCK 140 W", L"ШТАТНЫЕ 140 Вт" },
    { L"Current F7:", L"Текущий F7:" },
    { L"Restore OEM", L"Вернуть OEM" },
    { L"    image:", L"    образ:" },
    { L" (current=", L" (текущее=" },
    { L"Applying ", L"Применение " },
    { L"OEM STOCK", L"ШТАТНЫЕ OEM-НАСТРОЙКИ" },
    { L"OEM stock", L"Штатные OEM-настройки" },
    { L"Profile:", L"Профиль:" },
    { L"Refresh", L"Обновить" },
    { L"APPLIED", L"ПРИМЕНЕНО" },
    { L"UNKNOWN", L"НЕИЗВЕСТНО" },
    { L"Apply ", L"Применить " },
    { L"Apply", L"Применить" },
    { L"GPU:", L"GPU:" },
    { L"N/A", L"Нет данных" },
    { L" W", L" Вт" },
};

static std::wstring UiTranslate(const std::wstring& original) {
    if (!g_uiRussian) return original;
    std::wstring result;
    for (size_t pos = 0; pos < original.size();) {
        bool found = false;
        for (const auto& t : kUiTranslations) {
            size_t length = wcslen(t.en);
            bool unitBoundary = wcscmp(t.en, L" W") != 0 || pos + length >= original.size() ||
                !((original[pos + length] >= L'A' && original[pos + length] <= L'Z') || (original[pos + length] >= L'a' && original[pos + length] <= L'z'));
            if (unitBoundary && original.compare(pos, length, t.en) == 0) {
                result += t.ru; pos += length; found = true; break;
            }
        }
        if (!found) result += original[pos++];
    }
    return result;
}

static BOOL UiSetText(HWND h, const wchar_t* value) {
    if (h == g_state && g_summaryValue[0]) {
        for (int i = 0; i < 2; ++i) {
            g_uiText[g_summaryValue[i]] = L"N/A";
            SetWindowTextW(g_summaryValue[i], UiTranslate(L"N/A").c_str());
        }
        g_uiStateColor = RGB(56, 189, 248);
    }
    g_uiText[h] = value ? value : L"";
    return SetWindowTextW(h, UiTranslate(g_uiText[h]).c_str());
}

static int UiMessage(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags) {
    return MessageBoxW(owner, UiTranslate(text ? text : L"").c_str(), UiTranslate(title ? title : L"").c_str(), flags);
}

static int UiScale(int value) { return MulDiv(value, g_uiDpi, 96); }

static std::wstring UiSettingsPath() {
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring folder = std::wstring(base) + L"\\NvpwrControlUI";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    return folder + L"\\settings.ini";
}

static void UiCreateFonts() {
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_uiHeadingFont) DeleteObject(g_uiHeadingFont);
    if (g_uiMetricFont) DeleteObject(g_uiMetricFont);
    if (g_uiStateFont) DeleteObject(g_uiStateFont);
    if (g_uiButtonFont) DeleteObject(g_uiButtonFont);
    if (g_uiMonoFont) DeleteObject(g_uiMonoFont);
    if (g_uiSmallFont) DeleteObject(g_uiSmallFont);

    g_uiFont = CreateFontW(-UiScale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiHeadingFont = CreateFontW(-UiScale(22), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiMetricFont = CreateFontW(-UiScale(28), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiStateFont = CreateFontW(-UiScale(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiButtonFont = CreateFontW(-UiScale(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiMonoFont = CreateFontW(-UiScale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    g_uiSmallFont = CreateFontW(-UiScale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void UiFont(HWND h, HFONT font = nullptr) {
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)(font ? font : g_uiFont), TRUE);
}

static void UiApplyFonts() {
    for (const auto& item : g_uiText) if (IsWindow(item.first)) UiFont(item.first);
    UiFont(g_title, g_uiHeadingFont);
    UiFont(g_state, g_uiStateFont);
    for (int i = 0; i < 3; ++i) {
        UiFont(g_summaryLabel[i], g_uiSmallFont);
        UiFont(g_summaryValue[i], g_uiMetricFont);
        UiFont(g_summaryHint[i], g_uiSmallFont);
    }
    UiFont(g_language);
    UiFont(g_combo);
    UiFont(g_status, g_uiMonoFont);
    UiFont(g_apply, g_uiButtonFont);
    UiFont(g_restore, g_uiButtonFont);
    UiFont(g_refresh, g_uiButtonFont);
    UiFont(g_restart, g_uiButtonFont);
    UiFont(g_driverHelp, g_uiButtonFont);
    UiFont(g_tuneTitle, g_uiStateFont);
    UiFont(g_tuneApply, g_uiButtonFont);
    UiFont(g_tuneReset, g_uiButtonFont);
    UiFont(g_restartNvidia, g_uiButtonFont);
    for (int i = 0; i < 6; ++i) {
        UiFont(g_tuneLabel[i], g_uiSmallFont);
        UiFont(g_tuneEdit[i], g_uiStateFont);
        UiFont(g_tuneRange[i], g_uiSmallFont);
    }
}

static void UiSetDpi(UINT dpi) {
    if (!dpi) dpi = 96;
    if ((int)dpi == g_uiDpi && g_uiFont) return;
    g_uiDpi = (int)dpi;
    UiCreateFonts();
    UiApplyFonts();
}

static void UiInit() {
    HDC dc = GetDC(nullptr);
    if (dc) { g_uiDpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); }
    std::wstring path = UiSettingsPath();
    g_uiRussian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    if (!path.empty()) g_uiRussian = GetPrivateProfileIntW(L"Interface", L"Russian", g_uiRussian ? 1 : 0, path.c_str()) != 0;
    UiCreateFonts();
    g_uiBackground = CreateSolidBrush(kUiBackground);
    g_uiCard = CreateSolidBrush(kUiCard);
    g_uiConsole = CreateSolidBrush(kUiConsoleBg);
    g_uiCardPen = CreatePen(PS_SOLID, 1, kUiCardBorder);
}

static void UiPlace(HWND h, int x, int y, int w, int height) {
    MoveWindow(h, UiScale(x), UiScale(y), UiScale(w), UiScale(height), TRUE);
}

static void UiLayout(HWND hwnd) {
    RECT r{}; GetClientRect(hwnd, &r);
    int w = MulDiv(r.right, 96, g_uiDpi), height = MulDiv(r.bottom, 96, g_uiDpi);

    // Header section
    UiPlace(g_title, 30, 16, w - 240, 32);
    UiPlace(g_language, w - 190, 16, 160, 200);
    UiPlace(g_subtitle, 30, 50, w - 60, 22);
    UiPlace(g_state, 30, 76, w - 60, 26);

    // 3 Stat Cards
    int cardWidth = (w - 80) / 3;
    for (int i = 0; i < 3; i++) {
        int left = 30 + i * (cardWidth + 10);
        UiPlace(g_summaryLabel[i], left + 16, 120, cardWidth - 32, 18);
        UiPlace(g_summaryValue[i], left + 16, 142, cardWidth - 32, 36);
        UiPlace(g_summaryHint[i], left + 16, 180, cardWidth - 32, 16);
    }

    // Section 1: Power Limit Controls
    UiPlace(g_label, 30, 222, w - 60, 20);
    UiPlace(g_hint, 30, 244, w - 60, 18);
    UiPlace(g_combo, 30, 266, w - 60, 320);

    UiPlace(g_apply, 30, 310, 150, 38);
    UiPlace(g_restore, 190, 310, 150, 38);
    UiPlace(g_refresh, 350, 310, 115, 38);
    UiPlace(g_restart, 475, 310, 180, 38);
    UiPlace(g_driverHelp, 665, 310, 160, 38);

    // Section 2: Advanced Tuning
    UiPlace(g_tuneTitle, 30, 364, w - 60, 22);
    const int gap = 12; const int col = (w - 60 - gap * 2) / 3;
    for (int i = 0; i < 6; ++i) {
        int row = i / 3, c = i % 3, x = 30 + c * (col + gap), y = 392 + row * 66;
        UiPlace(g_tuneLabel[i], x, y, col, 18);
        UiPlace(g_tuneEdit[i], x, y + 20, 96, 26);
        UiPlace(g_tuneRange[i], x + 104, y + 23, col - 104, 20);
    }
    UiPlace(g_tuneApply, 30, 528, 140, 34);
    UiPlace(g_tuneReset, 180, 528, 140, 34);
    UiPlace(g_restartNvidia, 330, 528, 180, 34);
    UiPlace(g_tuningInfo, 520, 522, w - 550, 46);

    // Section 3: Technical Details & Terminal
    UiPlace(g_details, 30, 574, w - 60, 20);
    int statusBottom = height - 76;
    int detailHeight = statusBottom - 598;
    if (detailHeight < 80) detailHeight = 80;
    UiPlace(g_status, 30, 598, w - 60, detailHeight);
    UiPlace(g_note, 30, height - 68, w - 60, 34);
    UiPlace(g_footer, 30, height - 28, w - 60, 20);
}

static std::wstring UiWatts(ULONG mw) {
    if (mw == 0xffffffffu) return L"N/A";
    std::wstringstream text; text << std::fixed << std::setprecision(1) << (mw / 1000.0) << L" W"; return text.str();
}

static void UiSelection() {
    LRESULT index = SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    std::wstring text = L"N/A";
    if (index >= 0 && static_cast<size_t>(index) < g_targets.size()) {
        ULONG watts = g_targets[static_cast<size_t>(index)];
        text = watts ? std::to_wstring(watts) + L" W" : L"OEM";
    }
    UiSetText(g_summaryValue[2], text.c_str());
}

static void UiSummary(ULONG current, ULONG baseline, ULONG state) {
    UiSetText(g_summaryValue[0], UiWatts(current).c_str());
    UiSetText(g_summaryValue[1], UiWatts(baseline).c_str());
    UiSetText(g_subtitle, g_gpuName.c_str()); UiSelection();
    g_uiStateColor = (state == NvpwrStateApplied || state == NvpwrStateStockBaseline) ? RGB(118, 185, 0) : RGB(255, 170, 50);
    InvalidateRect(g_state, nullptr, TRUE);
}

static std::vector<std::wstring> g_uiComboEnglish;
static LRESULT UiComboMessage(HWND control, UINT message, WPARAM wp, LPARAM lp) {
    if (message == CB_RESETCONTENT) g_uiComboEnglish.clear();
    if (message == CB_ADDSTRING) {
        std::wstring original = reinterpret_cast<const wchar_t*>(lp);
        g_uiComboEnglish.push_back(original);
        std::wstring display = UiTranslate(original);
        return SendMessageW(control, message, wp, reinterpret_cast<LPARAM>(display.c_str()));
    }
    return SendMessageW(control, message, wp, lp);
}

static void UiTargets() {
    LRESULT selected = SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& item : g_uiComboEnglish) {
        std::wstring text = UiTranslate(item); SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(g_combo, CB_SETCURSEL, selected, 0);
}

static void UiLanguageChanged(HWND hwnd) {
    g_uiRussian = SendMessageW(g_language, CB_GETCURSEL, 0, 0) == 1;
    for (const auto& item : g_uiText) if (IsWindow(item.first)) SetWindowTextW(item.first, UiTranslate(item.second).c_str());
    UiTargets();
    std::wstring path = UiSettingsPath(); if (!path.empty()) WritePrivateProfileStringW(L"Interface", L"Russian", g_uiRussian ? L"1" : L"0", path.c_str());
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void UiDestroy() {
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_uiHeadingFont) DeleteObject(g_uiHeadingFont);
    if (g_uiMetricFont) DeleteObject(g_uiMetricFont);
    if (g_uiStateFont) DeleteObject(g_uiStateFont);
    if (g_uiButtonFont) DeleteObject(g_uiButtonFont);
    if (g_uiMonoFont) DeleteObject(g_uiMonoFont);
    if (g_uiSmallFont) DeleteObject(g_uiSmallFont);
    if (g_uiBackground) DeleteObject(g_uiBackground);
    if (g_uiCard) DeleteObject(g_uiCard);
    if (g_uiConsole) DeleteObject(g_uiConsole);
    if (g_uiCardPen) DeleteObject(g_uiCardPen);
}

