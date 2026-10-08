// Native Win32 integration coverage. The production window procedure and drawing
// code are compiled unchanged into this console test executable. No mock renderer
// or alternate input handler is used; fixtures only seed the Game's public API.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "../src/main.cpp"

namespace
{
namespace fs = std::filesystem;
struct CheckResult
{
    std::string name;
    bool passed;
};
struct ScreenshotResult
{
    std::string file;
    int width;
    int height;
    std::size_t sampledColors;
    std::uint64_t checksum;
};
std::vector<CheckResult> checks;
std::vector<ScreenshotResult> screenshots;
fs::path evidenceDirectory;
std::string failure;
HWND testWindow = nullptr;

void Check(bool condition, const std::string &name)
{
    checks.push_back({name, condition});
    std::cout << (condition ? "PASS: " : "FAIL: ") << name << std::endl;
    if (!condition)
        throw std::runtime_error(name);
}

void PumpMessages()
{
    MSG message = {};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
            throw std::runtime_error("The window exited unexpectedly during a test");
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

void PumpFor(DWORD milliseconds)
{
    const ULONGLONG startedAt = GetTickCount64();
    do
    {
        PumpMessages();
        Sleep(2);
    } while (GetTickCount64() - startedAt < milliseconds);
    PumpMessages();
}

template <typename Predicate>
bool PumpUntil(Predicate finished, DWORD timeoutMs)
{
    const ULONGLONG startedAt = GetTickCount64();
    while (!finished() && GetTickCount64() - startedAt < timeoutMs)
    {
        PumpMessages();
        Sleep(2);
    }
    return finished();
}

void KeyDown(WPARAM key)
{
    SendMessageA(testWindow, WM_KEYDOWN, key, 1);
}
void KeyUp(WPARAM key)
{
    SendMessageA(testWindow, WM_KEYUP, key,
                 static_cast<LPARAM>(static_cast<ULONG_PTR>(0xC0000001u)));
}
void Tap(WPARAM key)
{
    KeyDown(key);
    KeyUp(key);
}

std::vector<std::pair<int, int>> Cells(const std::vector<Position> &positions)
{
    std::vector<std::pair<int, int>> result;
    for (const Position &position : positions)
        result.emplace_back(position.row, position.column);
    std::sort(result.begin(), result.end());
    return result;
}

bool Shifted(const std::vector<Position> &before, const std::vector<Position> &after,
             int rows, int columns)
{
    auto expected = Cells(before);
    for (auto &position : expected)
    {
        position.first += rows;
        position.second += columns;
    }
    return expected == Cells(after);
}

int MinimumColumn()
{
    int result = 10;
    for (const Position &position : game.GetCurrentBlockCells())
        result = (std::min)(result, position.column);
    return result;
}
int OccupiedCells()
{
    int count = 0;
    for (int row = 0; row < 20; ++row)
        for (int column = 0; column < 10; ++column)
            count += game.GetGrid()[row][column] != 0 ? 1 : 0;
    return count;
}

void Capture(const std::string &name)
{
    // Redraw via WM_PAINT, then copy the real client DC. Do not call DrawGame
    // directly: this checks the same double-buffered paint path users execute.
    RedrawWindow(testWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    GdiFlush();
    RECT client = {};
    if (!GetClientRect(testWindow, &client))
        throw std::runtime_error("Cannot measure screenshot client area");
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC windowDc = GetDC(testWindow);
    HDC copyDc = windowDc ? CreateCompatibleDC(windowDc) : nullptr;
    HBITMAP bitmap = windowDc ? CreateCompatibleBitmap(windowDc, width, height) : nullptr;
    if (!windowDc || !copyDc || !bitmap)
    {
        if (bitmap) DeleteObject(bitmap);
        if (copyDc) DeleteDC(copyDc);
        if (windowDc) ReleaseDC(testWindow, windowDc);
        throw std::runtime_error("Cannot allocate native screenshot resources");
    }
    HGDIOBJ previous = SelectObject(copyDc, bitmap);
    const BOOL copied = BitBlt(copyDc, 0, 0, width, height, windowDc, 0, 0, SRCCOPY);
    SelectObject(copyDc, previous);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // Top-down, 32-bit native BGRA pixels.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    const int scanLines = copied ? GetDIBits(copyDc, bitmap, 0, static_cast<UINT>(height),
                                           pixels.data(), &info, DIB_RGB_COLORS) : 0;
    DeleteObject(bitmap);
    DeleteDC(copyDc);
    ReleaseDC(testWindow, windowDc);
    Check(copied != FALSE && scanLines == height, name + ": client pixels captured");
    std::set<std::uint32_t> colors;
    std::uint64_t checksum = 14695981039346656037ull;
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        const std::uint32_t rgb = pixels[index] & 0x00ffffffu;
        checksum ^= rgb;
        checksum *= 1099511628211ull;
        if (index % 17 == 0) colors.insert(rgb);
    }
    BITMAPFILEHEADER header = {};
    header.bfType = 0x4d42;
    header.bfOffBits = static_cast<DWORD>(sizeof(header) + sizeof(BITMAPINFOHEADER));
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(pixels[0]));
    const std::string fileName = name + ".bmp";
    std::ofstream output(evidenceDirectory / fileName, std::ios::binary);
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char *>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size() * sizeof(pixels[0])));
    output.close();
    Check(output.good(), name + ": BMP evidence saved");
    screenshots.push_back({fileName, width, height, colors.size(), checksum});
    Check(colors.size() >= 12, name + ": rendered client is not blank");
}

void Fixture(const Game &seededGame)
{
    // Only fixture arrangement uses direct state access. Every operation under
    // test is sent through the real window procedure, including menu Start.
    KillTimer(testWindow, AppConfig::DropTimerId);
    ResetRunUiState(testWindow);
    game = seededGame;
    settingsOpen = false;
    settingsNameEditActive = false;
    leaderboardOpen = false;
    selectedMainMenuIndex = MainMenuStart;
    selectedPauseMenuIndex = PauseMenuContinue;
    selectedSettingIndex = 0;
    Tap(VK_RETURN);
    Check(game.IsStarted() && !game.IsGameOver(), "fixture starts through the main menu");
}

void TestMenuAndSettings()
{
    Check(IsWindow(testWindow) && IsWindowVisible(testWindow), "real native window is visible");
    Check(!game.IsStarted(), "application initially displays its main menu");
    Capture("01-main-menu");
    Tap(VK_DOWN);
    Check(selectedMainMenuIndex == MainMenuSettings, "Down selects main-menu Settings");
    Tap(VK_RETURN);
    Check(settingsOpen && !game.IsStarted(), "Enter opens settings from the main menu");
    Capture("02-settings");
    const int oldPreset = tuningPreset;
    Tap(VK_RIGHT);
    Check(tuningPreset != oldPreset, "settings keyboard navigation changes tuning preset");
    Tap(VK_ESCAPE);
    Check(!settingsOpen && !game.IsStarted(), "Escape closes settings to the main menu");
    Tap(VK_DOWN);
    Tap(VK_RETURN);
    Check(leaderboardOpen, "main-menu Leaderboard opens");
    Tap(VK_ESCAPE);
    Check(!leaderboardOpen, "Escape closes leaderboard");
    Tap(VK_UP);
    Tap(VK_UP);
    Tap(VK_RETURN);
    Check(game.IsStarted() && !game.IsPaused(), "Enter starts a new game from Start");
    Capture("03-started-game");
    ApplyRuntimeSettings(GetDefaultSettings());
    SetSoundEnabled(false);
}

void TestMovementRotationDropsAndHold()
{
    Fixture(Game(TBlock(), IBlock()));
    const auto original = game.GetCurrentBlockCells();
    Tap(VK_LEFT);
    Check(Shifted(original, game.GetCurrentBlockCells(), 0, -1), "Left moves the active piece one cell");
    Check(!leftHeld && !inputTimerRunning, "Left key-up releases horizontal input and its timer");
    Tap(VK_RIGHT);
    Check(Cells(original) == Cells(game.GetCurrentBlockCells()), "Right moves the active piece back");
    Tap(VK_UP);
    Check(Cells(original) != Cells(game.GetCurrentBlockCells()), "Up rotates clockwise");
    Tap('Z');
    Check(Cells(original) == Cells(game.GetCurrentBlockCells()), "Z rotates counterclockwise back to the original shape");
    Tap('X');
    Check(Cells(original) != Cells(game.GetCurrentBlockCells()), "X also rotates clockwise");
    Tap('Z');
    const int previousScore = game.GetScore();
    Tap(VK_DOWN);
    Check(Shifted(original, game.GetCurrentBlockCells(), 1, 0), "Down performs a soft drop");
    Check(game.GetScore() == previousScore + 1, "soft drop awards one point");
    const int heldId = game.GetCurrentBlockId();
    const int nextId = game.GetNextBlockId();
    Tap('C');
    Check(game.HasHeldBlock() && game.GetHeldBlockId() == heldId, "C stores the active piece in hold");
    Check(game.GetCurrentBlockId() == nextId && !game.CanHold(), "hold advances the queue and disables a second hold");
    Tap(VK_SHIFT);
    Check(game.GetHeldBlockId() == heldId && game.GetCurrentBlockId() == nextId,
          "Shift cannot hold twice before a lock");
    const auto ghost = game.GetGhostBlockCells();
    const auto active = game.GetCurrentBlockCells();
    const int scoreBeforeDrop = game.GetScore();
    const int expectedDistance = ghost.front().row - active.front().row;
    Tap(VK_SPACE);
    Check(OccupiedCells() == 4, "Space hard-drops and locks four cells");
    Check(game.GetScore() == scoreBeforeDrop + 2 * expectedDistance, "hard drop awards two points per row");
    Check(game.CanHold(), "locking a piece enables hold again");
    Tap(VK_SHIFT);
    Check(game.GetCurrentBlockId() == heldId, "Shift swaps the previously held piece back into play");
    Capture("04-movement-drop-hold");
}

void TestGravityPauseRestartAndRepeat()
{
    Fixture(Game(TBlock(), IBlock()));
    const auto beforeGravity = game.GetCurrentBlockCells();
    Check(PumpUntil([&] { return Cells(beforeGravity) != Cells(game.GetCurrentBlockCells()); }, 2000),
          "real WM_TIMER gravity moves the active piece");
    Tap('P');
    Check(game.IsPaused(), "P opens the pause menu");
    const auto pausedCells = game.GetCurrentBlockCells();
    const int pausedScore = game.GetScore();
    Tap(VK_LEFT);
    Tap(VK_RIGHT);
    Tap('X');
    Tap('Z');
    Tap('C');
    SendMessageA(testWindow, WM_TIMER, AppConfig::DropTimerId, 0);
    PumpFor(static_cast<DWORD>(game.GetDropIntervalMs() * 2 + 60));
    Check(Cells(pausedCells) == Cells(game.GetCurrentBlockCells()), "paused piece ignores movement, rotation, hold, and gravity");
    Check(pausedScore == game.GetScore() && !game.HasHeldBlock(), "paused inputs do not change score or hold");
    Capture("05-paused-game");
    Tap(VK_RETURN);
    Check(!game.IsPaused(), "pause-menu Continue resumes the game");
    const auto beforeSettings = game.GetCurrentBlockCells();
    Tap(VK_F1);
    Check(settingsOpen, "F1 opens settings during play");
    PumpFor(static_cast<DWORD>(game.GetDropIntervalMs() + 60));
    Check(Cells(beforeSettings) == Cells(game.GetCurrentBlockCells()), "settings stop gravity during play");
    Tap(VK_F1);
    Check(!settingsOpen, "F1 closes settings during play");
    Tap(VK_SPACE);
    Tap('C');
    Tap(VK_ESCAPE);
    Check(game.IsPaused(), "Escape opens the pause menu");
    Tap(VK_DOWN);
    Check(selectedPauseMenuIndex == PauseMenuRestart, "pause-menu Down selects Restart");
    Tap(VK_RETURN);
    Check(game.IsStarted() && !game.IsPaused() && !game.IsGameOver(), "pause-menu Restart starts a fresh run");
    Check(game.GetScore() == 0 && OccupiedCells() == 0 && !game.HasHeldBlock(), "Restart clears board, score, and hold");

    Fixture(Game(TBlock(), IBlock()));
    KeyDown(VK_LEFT);
    const auto firstPress = game.GetCurrentBlockCells();
    SendMessageA(testWindow, WM_KEYDOWN, VK_LEFT, static_cast<LPARAM>(0x40000001));
    Check(Cells(firstPress) == Cells(game.GetCurrentBlockCells()), "OS repeat keydown does not add an immediate extra move");
    Check(PumpUntil([] { return MinimumColumn() == 0; }, 2000), "real input timer repeats a held key to the wall");
    KeyUp(VK_LEFT);
    Check(!leftHeld && !rightHeld && !inputTimerRunning, "key release stops held-key repeat");
    PumpFor(120);
    Check(MinimumColumn() == 0, "released key causes no further horizontal movement");
}

void TestClearAndLockTimers()
{
    Grid board;
    for (int column = 0; column < 10; ++column)
        if (column < 3 || column > 6) board.grid[19][column] = 2;
    Fixture(Game(IBlock(), TBlock(), board, 9));
    Tap(VK_SPACE);
    Check(game.IsLineClearPending() && game.GetLastClearLines() == 1,
          "hard drop fills a line and starts the animated clear");
    Check(observedClearEventId > 0 && game.GetLinesCleared() == 10, "window procedure observes the clear event");
    Check(game.GetLevel() == 2 && observedLevelUpEventId > 0, "line clear triggers level-up in the window procedure");
    Capture("06-line-clear-flash");
    Check(PumpUntil([] { return !game.IsLineClearPending(); }, 3000),
          "real clear-flash timer completes the animation without direct FinishLineClear calls");
    Check(OccupiedCells() == 0 && game.GetCurrentBlockId() == 6, "clear completion removes the row and spawns the next piece");
    Check(game.GetScore() > 0 && game.WasLastClearPerfectClear(), "line-clear score and perfect-clear status are retained");
    Capture("07-line-clear-complete");

    OBlock grounded;
    grounded.Move(18, 0);
    Fixture(Game(grounded, TBlock()));
    Tap(VK_DOWN);
    Check(game.IsLockDelayActive() && lockDelayTimerRunning, "ground contact starts the native lock-delay timer");
    Check(PumpUntil([] { return OccupiedCells() == 4; }, 3000), "real lock-delay timer locks the grounded piece");
    Check(game.GetCurrentBlockId() == 6 && !game.IsLockDelayActive(), "lock completion spawns the next piece and clears delay state");
}

void TestGameOverAndNameEntry()
{
    Grid board;
    board.grid[0][4] = 7; // Blocks the upcoming T, but not the left-hand O fixture.
    OBlock starting;
    starting.Move(0, -4);
    leaderboard.clear();
    Fixture(Game(starting, TBlock(), board));
    Tap(VK_SPACE);
    Check(game.IsGameOver(), "a blocked next spawn enters game over");
    Check(leaderboardNameEntryActive && pendingLeaderboardScore > 0, "qualifying game over opens name entry");
    Capture("08-game-over-name-entry");
    while (!leaderboardNameInput.empty()) Tap(VK_BACK);
    Tap('U');
    Tap('I');
    Tap(VK_RETURN);
    Check(leaderboardNameEntryActive, "name entry rejects a name shorter than three characters");
    Tap('T');
    Tap('E');
    Tap('S');
    Tap('T');
    const int recordedScore = pendingLeaderboardScore;
    Tap(VK_RETURN);
    Check(!leaderboardNameEntryActive && gameOverScoreRecorded, "Enter submits a valid leaderboard name");
    Check(!leaderboard.empty() && leaderboard.front().name == "UITEST" && leaderboard.front().score == recordedScore,
          "the submitted name and score reach the leaderboard");
    const auto saved = LoadLeaderboard(AppConfig::LeaderboardFile);
    Check(!saved.empty() && saved.front().name == "UITEST" && saved.front().score == recordedScore,
          "leaderboard entry is persisted in the isolated test directory");
    Check(LoadHighScore(AppConfig::HighScoreFile) == game.GetHighScore(), "high score is persisted after name submission");
    Capture("09-game-over-recorded");
    Tap('R');
    Check(!game.IsGameOver() && game.IsStarted() && game.GetScore() == 0, "R restarts after game over");
}

void TestRepeatedActionsAndPaintResources()
{
    const DWORD beforeGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 30; ++iteration)
    {
        Tap('R');
        Tap(VK_LEFT);
        Tap(VK_RIGHT);
        Tap('X');
        Tap('Z');
        Tap(VK_DOWN);
        Tap('C');
        Tap(VK_SPACE);
        Tap('P');
        Tap('P');
        Tap(VK_F1);
        Tap(VK_ESCAPE);
        RedrawWindow(testWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        Check(game.IsStarted() && !game.IsPaused() && !game.IsGameOver() && !settingsOpen,
              "repeated action cycle " + std::to_string(iteration + 1) + " remains playable");
    }
    for (int paint = 0; paint < 120; ++paint)
        RedrawWindow(testWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    GdiFlush();
    const DWORD afterGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    Check(afterGdi <= beforeGdi + 8, "repeated WM_PAINT calls do not leak GDI objects");
    Check(!leftHeld && !rightHeld && !inputTimerRunning, "repeated actions leave no stuck horizontal input");
    Capture("10-repeated-actions-final");
}

std::string Json(const std::string &value)
{
    std::string escaped;
    for (unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(character));
        }
        else if (character == '\n') escaped += "\\n";
        else if (character == '\r') escaped += "\\r";
        else if (character == '\t') escaped += "\\t";
        else if (character < 32) escaped += "?";
        else escaped.push_back(static_cast<char>(character));
    }
    return '"' + escaped + '"';
}

bool WriteReport()
{
    std::ofstream output(evidenceDirectory / "windows-ui-results.json");
    const auto passed = std::count_if(checks.begin(), checks.end(), [](const CheckResult &check) { return check.passed; });
    output << "{\n  \"status\": " << Json(failure.empty() ? "passed" : "failed")
           << ",\n  \"harness\": \"real Win32 HWND, production WindowProc and WM_PAINT, SendMessage keyboard input\""
           << ",\n  \"timer_delivery\": \"native message queue pumping\""
           << ",\n  \"screenshots\": \"BitBlt of real client DC, 32-bit BMP\""
           << ",\n  \"physical_keyboard_or_human_visual_review\": false"
           << ",\n  \"assertions_passed\": " << passed
           << ",\n  \"assertions_total\": " << checks.size()
           << ",\n  \"failure\": " << Json(failure)
           << ",\n  \"checks\": [\n";
    for (std::size_t index = 0; index < checks.size(); ++index)
        output << "    {\"name\": " << Json(checks[index].name) << ", \"passed\": "
               << (checks[index].passed ? "true" : "false") << "}" << (index + 1 < checks.size() ? "," : "") << '\n';
    output << "  ],\n  \"captures\": [\n";
    for (std::size_t index = 0; index < screenshots.size(); ++index)
    {
        const auto &capture = screenshots[index];
        output << "    {\"file\": " << Json(capture.file) << ", \"width\": " << capture.width
               << ", \"height\": " << capture.height << ", \"sampled_colors\": " << capture.sampledColors
               << ", \"pixel_checksum\": " << Json(std::to_string(capture.checksum)) << "}"
               << (index + 1 < screenshots.size() ? "," : "") << '\n';
    }
    output << "  ]\n}\n";
    output.close();
    return output.good();
}
} // namespace

int main(int argc, char **argv)
{
    const char className[] = "TetrisNativeUiIntegrationTests";
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    fs::path originalDirectory;
    bool registered = false;
    try
    {
        originalDirectory = fs::current_path();
        evidenceDirectory = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("windows-ui-evidence"));
        fs::create_directories(evidenceDirectory);
        const fs::path isolatedDirectory = evidenceDirectory / ("isolated-state-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(isolatedDirectory / "Font");
        const fs::path font = originalDirectory / "Font" / "monogram.ttf";
        if (fs::exists(font))
            fs::copy_file(font, isolatedDirectory / "Font" / "monogram.ttf", fs::copy_options::overwrite_existing);
        fs::current_path(isolatedDirectory);
        ApplyRuntimeSettings(GetDefaultSettings());
        SetSoundEnabled(false); // GUI/input/timers are tested; audio playback is not asserted.
        LoadGameFont();
        SetProcessDPIAware();
        WNDCLASSA windowClass = {};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        Check(RegisterClassA(&windowClass) != 0, "register production window procedure");
        registered = true;
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT bounds = {0, 0, AppConfig::WindowWidth, AppConfig::WindowHeight};
        Check(AdjustWindowRect(&bounds, style, FALSE) != FALSE, "calculate native client dimensions");
        testWindow = CreateWindowExA(0, className, "Tetris native UI integration tests", style,
                                    0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                    nullptr, nullptr, instance, nullptr);
        Check(testWindow != nullptr, "create the native GUI window");
        ShowWindow(testWindow, SW_SHOWNORMAL);
        SetWindowPos(testWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(testWindow);
        PumpMessages();
        TestMenuAndSettings();
        TestMovementRotationDropsAndHold();
        TestGravityPauseRestartAndRepeat();
        TestClearAndLockTimers();
        TestGameOverAndNameEntry();
        TestRepeatedActionsAndPaintResources();
    }
    catch (const std::exception &error)
    {
        failure = error.what();
        std::cerr << "UI TEST FAILURE: " << failure << std::endl;
        if (testWindow && IsWindow(testWindow))
        {
            try { Capture("failure-state"); } catch (...) {}
        }
    }
    if (testWindow && IsWindow(testWindow)) DestroyWindow(testWindow);
    ShutdownSound();
    UnloadGameFont();
    if (registered) UnregisterClassA(className, instance);
    if (!originalDirectory.empty())
    {
        std::error_code ignored;
        fs::current_path(originalDirectory, ignored);
    }
    if (evidenceDirectory.empty() || !WriteReport())
    {
        std::cerr << "Unable to save windows-ui-results.json" << std::endl;
        return 1;
    }
    std::cout << checks.size() << " native UI assertions; " << screenshots.size()
              << " screenshots; report: " << (evidenceDirectory / "windows-ui-results.json").string() << std::endl;
    return failure.empty() ? 0 : 1;
}
