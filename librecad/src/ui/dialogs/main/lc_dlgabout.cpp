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
#include "lc_dlgabout.h"

#include <QClipboard>
#include <QMimeData>
#include <QIcon>
#include <memory>
#include <boost/version.hpp>

#include "main.h"
#include "ui_lc_dlgabout.h"

#if defined(Q_OS_LINUX)
#include <QThread>
#endif

LC_DlgAbout::LC_DlgAbout(QWidget *parent)
    : LC_Dialog(parent, "About")
    , ui(std::make_unique<Ui::LC_DlgAbout>()){
    ui->setupUi(this);
    setWindowTitle(tr("About %1").arg(QString::fromLatin1(LCApplicationDisplayName())));

    // Compiler macro list in Qt source tree
    // Src/qtbase/src/corelib/global/qcompilerdetection.h

    m_info = QString(
        tr("Version: <b>%1</b>").arg(LCVersion()) + "<br/>" +
#if defined(Q_CC_CLANG)
        tr("Compiler: Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__) + "<br/>" +
#elif defined(Q_CC_GNU)
        tr("Compiler: GNU GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__) + "<br/>" +
#elif defined(Q_CC_MSVC)
        tr("Compiler: Microsoft Visual C++") + "<br/>" +
#endif
#ifndef NO_TIMESTAMP
        tr("Compiled on: %1").arg(__DATE__) + "<br/>" +
#endif
        tr("Qt Version: %1").arg(qVersion()) + "<br/>" +
        tr("Boost Version: %1.%2.%3").arg(BOOST_VERSION / 100000).arg(BOOST_VERSION / 100 % 1000).arg(BOOST_VERSION % 100)
        );
    ui->lVersionInfo->setText(m_info);


    ui->lLinks->setText(QString(R"(<a href="https://github.com/fabioman90/FableCAD">%1</a><br/>)"
                                R"(<a href="https://github.com/LibreCAD/LibreCAD/blob/master/LICENSE">%2</a><br/>)"
                                R"(<a href="https://github.com/LibreCAD/LibreCAD/graphs/contributors">%3</a>)")
                            .arg(tr("The Code"), tr("License"), tr("LibreCAD contributors")));

    ui->lblLogo->setPixmap(QIcon(":/images/fablecad.svg").pixmap(QSize(84, 84)));
    connect(ui->pbCopy, &QPushButton::clicked, this, &LC_DlgAbout::copyInfo);
}

LC_DlgAbout::~LC_DlgAbout() = default;

void LC_DlgAbout::copyInfo(){
    const QString htmlText = ui->lVersionInfo->text() + "<br>" + tr("System") + ": " + QSysInfo::prettyProductName();
    QTextDocument doc;
    doc.setHtml(htmlText);
    const QString plainText = doc.toPlainText();

    const auto mimeData = new QMimeData();
    mimeData->setText(plainText);
    mimeData->setHtml(htmlText);

    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setMimeData(mimeData);

#if defined(Q_OS_LINUX)
    QThread::msleep(1); //workaround for copied text not being available...
#endif
}
