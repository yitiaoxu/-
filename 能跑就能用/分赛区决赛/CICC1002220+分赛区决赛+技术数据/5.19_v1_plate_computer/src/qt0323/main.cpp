#include "mainwindow.h"
#include "qt_cjk_font.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>
#include <atomic>
#include <csignal>
#include <unistd.h>

namespace {
std::atomic<bool> g_sigint_requested{false};
QApplication* g_app = nullptr;

/** Async-signal-safe: only set a flag. Never call Qt/malloc/printf here. */
void handle_sigint(int /*sig*/) {
    g_sigint_requested.store(true, std::memory_order_relaxed);
}
}  // namespace

int main(int argc, char* argv[]) {
    QApplication a(argc, argv);
    g_app = &a;
    a.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal = a.palette();
    pal.setColor(QPalette::Window, QColor(255, 255, 255));
    pal.setColor(QPalette::WindowText, QColor(31, 41, 55));
    pal.setColor(QPalette::Base, QColor(255, 255, 255));
    pal.setColor(QPalette::AlternateBase, QColor(245, 246, 248));
    pal.setColor(QPalette::Button, QColor(255, 255, 255));
    pal.setColor(QPalette::ButtonText, QColor(55, 65, 81));
    pal.setColor(QPalette::Highlight, QColor(107, 155, 209));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    a.setPalette(pal);
    setupQtCjkFont(a);

    // Poll flag from event loop (safe). Old path used qDebug()+quit() inside the
    // signal handler, which can deadlock and make Ctrl+C look like a freeze.
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);
    QTimer sig_timer;
    QObject::connect(&sig_timer, &QTimer::timeout, &a, [&]() {
        if (!g_sigint_requested.exchange(false, std::memory_order_relaxed)) return;
        const char msg[] = "\n[INFO] Ctrl+C / SIGTERM, exiting...\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        a.quit();
    });
    sig_timer.start(50);

    MainWindow w;
    w.show();
    return a.exec();
}
