/*******************************************************************************
 *
 This file is part of the LibreCAD project, a 2D CAD program

 Copyright (C) 2025 LibreCAD.org
 Copyright (C) 2025 sand1024

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 ******************************************************************************/
#include "lc_customstylehelper.h"

#include <QApplication>
#include <QFile>
#include <QPixmapCache>
#include <QStyleFactory>

#include "lc_iconcolorsoptions.h"
#include "rs_settings.h"

void LC_CustomStyleHelper::loadFromSettings() {
    if (LC_GET_ONE_BOOL("Widgets", "AllowStyle", false)) {
        const QString style = LC_GET_ONE_STR("Widgets", "Style", "");
        QApplication::setStyle(QStyleFactory::create(style));
    }

    if (LC_GET_ONE_BOOL("Widgets", "AllowStyleSheet", true)) {
        loadStyleSheet(LC_GET_ONE_STR("Widgets", "StyleSheet", ""));
    }
}

bool LC_CustomStyleHelper::loadStyleSheet(const QString& path) {
    if (!path.isEmpty() && QFile::exists(path)) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (m_previousPalette) {
                QApplication::setPalette(*m_previousPalette);
                m_previousPalette.reset();
                LC_IconColorsOptions icons;
                icons.loadSettings();
                icons.applyOptions();
                QPixmapCache::clear();
            }
            qApp->setStyleSheet(QString::fromLatin1(file.readAll()));
            m_styleSheetPath = path;
            return true;
        }
    }
    else {
        if (!m_previousPalette) {
            m_previousPalette = QApplication::palette();
        }
        QPalette palette = *m_previousPalette;
        const bool light = LC_GET_ONE_BOOL("Widgets", "FableCADLightTheme", false);
        palette.setColor(QPalette::Window, QColor(light ? "#edf2f5" : "#202e3a"));
        palette.setColor(QPalette::Base, QColor(light ? "#ffffff" : "#17232e"));
        palette.setColor(QPalette::AlternateBase, QColor(light ? "#e0e8ed" : "#263746"));
        palette.setColor(QPalette::Button, QColor(light ? "#f6f9fa" : "#293b49"));
        palette.setColor(QPalette::WindowText, QColor(light ? "#172d3a" : "#e1edf2"));
        palette.setColor(QPalette::Text, QColor(light ? "#172d3a" : "#e1edf2"));
        palette.setColor(QPalette::ButtonText, QColor(light ? "#172d3a" : "#e1edf2"));
        palette.setColor(QPalette::PlaceholderText, QColor(light ? "#526b79" : "#9fb4c1"));
        palette.setColor(QPalette::Mid, QColor(light ? "#a2b3be" : "#405665"));
        palette.setColor(QPalette::Highlight, QColor(light ? "#327570" : "#30605e"));
        palette.setColor(QPalette::HighlightedText, QColor(light ? "#ffffff" : "#f4fcfc"));
        palette.setColor(QPalette::ToolTipBase, QColor(light ? "#f6f9fa" : "#293b49"));
        palette.setColor(QPalette::ToolTipText, QColor(light ? "#172d3a" : "#e1edf2"));
        palette.setColor(QPalette::Link, QColor(light ? "#197269" : "#6cd3c3"));
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
            palette.setColor(QPalette::Disabled, role, QColor(light ? "#697e8b" : "#93a4af"));
        }
        QApplication::setPalette(palette);
        LC_IconColorsOptions icons;
        icons.loadSettings();
        icons.applyOptions();
        QPixmapCache::clear();

        qApp->setStyleSheet(QStringLiteral(R"(
            QMainWindow, QDockWidget, QMenuBar, QMenu, QStatusBar, QToolBar {
                background: palette(window);
                color: palette(window-text);
            }
            QMainWindow::separator {
                background: palette(alternate-base);
                width: 4px;
                height: 4px;
            }
            QMainWindow::separator:hover {
                background: palette(highlight);
            }
            QTabBar::tab {
                background: palette(alternate-base);
                color: palette(window-text);
                border: 1px solid palette(window);
                padding: 4px 8px;
            }
            QTabBar::tab:selected {
                background: palette(button);
                border-bottom-color: palette(link);
            }
            QMenu::item:selected, QMenuBar::item:selected {
                color: palette(highlighted-text);
                background: palette(highlight);
            }
            QLineEdit, QAbstractSpinBox, QComboBox, QTreeView, QListView, QTableView {
                background: palette(base);
                color: palette(text);
                border: 1px solid palette(mid);
                border-radius: 3px;
            }
            QComboBox, QAbstractSpinBox {
                padding: 2px 4px;
            }
            QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
                border-color: palette(link);
            }
            QToolBar {
                spacing: 3px;
                padding: 1px;
            }
            QToolBar QToolButton {
                padding: 2px;
                border: 1px solid transparent;
                border-radius: 4px;
            }
            QToolBar QToolButton:hover {
                background: palette(alternate-base);
                border-color: palette(link);
            }
            QToolBar QToolButton:checked,
            QToolBar QToolButton:pressed {
                background: palette(highlight);
                border-color: palette(link);
            }
            QDockWidget#command_dockwidget QLineEdit#leCommand {
                padding: 4px 7px;
                border: 1px solid palette(mid);
                border-radius: 4px;
            }
            QDockWidget#command_dockwidget QLineEdit#leCommand:focus {
                border-color: palette(link);
            }
            QDockWidget::title {
                background: palette(alternate-base);
                padding: 5px;
            }
            QToolBar#snap_toolbar QToolButton:checked {
                color: palette(highlighted-text);
                background: palette(highlight);
                border-color: palette(link);
                border-bottom-width: 2px;
            }
            QWidget#LC_PropertySheetWidget QWidget#headerSelection,
            QWidget#LC_PropertySheetWidget QWidget#headerAction {
                background: palette(alternate-base);
            }
            QLabel#propertyDescription {
                background: palette(base);
                color: palette(window-text);
                padding: 6px;
                border: 1px solid palette(mid);
            }
            QStatusBar {
                border-top: 1px solid palette(mid);
            }
        )"));
        m_styleSheetPath = path;
    }
    return false;
}

void LC_CustomStyleHelper::reloadStyleSheet() {
    loadStyleSheet(m_styleSheetPath);
}
