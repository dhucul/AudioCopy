// ============================================================================
// InterruptHandler.h - Signal handling and user interrupts
// ============================================================================
#pragma once

#include <windows.h>
#include <iostream>
#include <conio.h>
#include <atomic>

class InterruptHandler {
public:
    static InterruptHandler& Instance() {
        static InterruptHandler instance;
        return instance;
    }

    void Install() {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    }

    bool IsInterrupted() const { return m_interrupted.load(); }
    void SetInterrupted(bool value) { m_interrupted.store(value); }

    bool CheckEscapeKey() {
        if (_kbhit()) {
            int key = _getch();
            if (key == 27) return true;
            if (key == 0 || key == 0xE0) _getch();
        }
        return false;
    }

    bool CheckInterrupt() {
        if (m_interrupted.load() || CheckEscapeKey()) {
            m_interrupted.store(true);
            std::cout << "\n\n*** Operation cancelled by user ***\n";
            std::cout << "Press any key to exit...\n";
            _getch();
            exit(0);
        }
        return false;
    }

    static void PrintExitHelp() {
        std::cout << "\n  [Press ESC or Ctrl+C at any time to exit safely]\n\n";
    }

private:
    InterruptHandler() = default;
    std::atomic<bool> m_interrupted{false};

    static BOOL WINAPI ConsoleHandler(DWORD signal) {
        if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
            Instance().SetInterrupted(true);
            return TRUE;
        }
        return FALSE;
    }
};

// Convenience macro
#define g_interrupt InterruptHandler::Instance()