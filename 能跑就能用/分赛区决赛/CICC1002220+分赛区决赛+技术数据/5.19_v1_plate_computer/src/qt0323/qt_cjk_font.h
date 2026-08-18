#ifndef QT_CJK_FONT_H
#define QT_CJK_FONT_H

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QStringList>

/** Load a CJK-capable font so Qt buttons/labels do not show mojibake on the board. */
inline void setupQtCjkFont(QApplication& app) {
    const char* font_files[] = {
        "test_rknn_infer/fonts/platech.ttf",
        "./fonts/platech.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    };
    for (const char* path : font_files) {
        int id = QFontDatabase::addApplicationFont(QString::fromUtf8(path));
        if (id >= 0) {
            QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) {
                QFont f(families.at(0), 11);
                app.setFont(f);
                return;
            }
        }
    }
    const char* families[] = {
        "WenQuanYi Micro Hei",
        "Noto Sans CJK SC",
        "Source Han Sans CN",
        "Droid Sans Fallback",
        "DejaVu Sans",
    };
    for (const char* name : families) {
        if (QFontDatabase().hasFamily(QString::fromUtf8(name))) {
            QFont f(QString::fromUtf8(name), 11);
            app.setFont(f);
            return;
        }
    }
}

#endif
