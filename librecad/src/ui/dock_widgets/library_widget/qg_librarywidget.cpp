/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2011 Rallaz (rallazz@gmail.com)
** Copyright (C) 2010 R. van Twisk (librecad@rvt.dds.nl)
** Copyright (C) 2001-2003 RibbonSoft. All rights reserved.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software 
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
** This copyright notice MUST APPEAR in all copies of the script!  
**
**********************************************************************/

#include "qg_librarywidget.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageWriter>
#include <QKeyEvent>
#include <QListView>
#include <QLineEdit>
#include <QSet>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "lc_action_block_library_insert.h"
#include "lc_containertraverser.h"
#include "lc_documentsstorage.h"
#include "lc_graphicviewport.h"
#include "lc_printviewportrenderer.h"
#include "qg_actionhandler.h"
#include "rs_actioninterface.h"
#include "rs_debug.h"
#include "rs_graphic.h"
#include "rs_painter.h"
#include "rs_settings.h"
#include "rs_system.h"

namespace {
    // fixme - sand - rework, use reusable class
    void writePng(const QString& pngPath, const QPixmap& pixmap) {
        QImageWriter iio;
        QImage img = pixmap.toImage();
        img = img.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        // iio.setImage(img);
        iio.setFileName(pngPath);
        iio.setFormat("PNG");
        if (!iio.write(img)) {
            RS_DEBUG->print(RS_Debug::D_ERROR, "QG_LibraryWidget::getPathToPixmap: Cannot write thumbnail: '%s'",
                            pngPath.toLatin1().data());
        }
    }
}

/*
 *  Constructs a QG_LibraryWidget as a child of 'parent', with the
 *  name 'name' and widget flags set to 'f'.
 *
 * @author Rallaz
 */
QG_LibraryWidget::QG_LibraryWidget(const QG_ActionHandler* actionHandler, QWidget* parent, const char* name, const Qt::WindowFlags fl)
    : LC_GraphicViewAwareWidget(parent, name, fl), m_actionHandler{actionHandler} {
    const auto vboxLayout = new QVBoxLayout(this);
    vboxLayout->setSpacing(2);
    vboxLayout->setContentsMargins(2, 2, 2, 2);
    m_dirView = new QTreeView(this);
    m_dirView->setRootIsDecorated(true);
    m_dirView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vboxLayout->addWidget(m_dirView);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search library blocks"));
    m_search->setAccessibleName(tr("Search library blocks"));
    m_search->setClearButtonEnabled(true);
    vboxLayout->addWidget(m_search);
    m_ivPreview = new QListView(this);
    m_ivPreview->setViewMode(QListView::IconMode);
    vboxLayout->addWidget(m_ivPreview);
    m_bInsert = new QPushButton(tr("Insert"), this);
    vboxLayout->addWidget(m_bInsert);

    const auto refreshButtonsLayout = new QHBoxLayout();
    m_bRefresh = new QPushButton(tr("Refresh"), this);
    refreshButtonsLayout->addWidget(m_bRefresh);
    m_bRebuild = new QPushButton(tr("Rebuild"), this);
    refreshButtonsLayout->addWidget(m_bRebuild);
    vboxLayout->addLayout(refreshButtonsLayout);

    buildTree();

    connect(m_dirView, &QTreeView::expanded, this, &QG_LibraryWidget::expandView);
    connect(m_dirView, &QTreeView::collapsed, this, &QG_LibraryWidget::collapseView);
    connect(m_dirView, &QTreeView::clicked, this, &QG_LibraryWidget::updatePreview);
    auto* searchDelay = new QTimer(this);
    searchDelay->setSingleShot(true);
    searchDelay->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, searchDelay, qOverload<>(&QTimer::start));
    connect(searchDelay, &QTimer::timeout, this, [this] {
        updatePreview(m_dirView->currentIndex());
    });
    connect(m_ivPreview, &QListView::doubleClicked, this, [this] {
        if (m_bInsert->isEnabled())
            insert();
    });
    connect(m_bInsert, &QPushButton::clicked, this, &QG_LibraryWidget::insert);
    connect(m_bRefresh, &QPushButton::clicked, this, &QG_LibraryWidget::refresh);
    connect(m_bRebuild, &QPushButton::clicked, this, &QG_LibraryWidget::buildTree);

    updateWidgetSettings();
}

QG_LibraryWidget::~QG_LibraryWidget() = default;

void QG_LibraryWidget::setGraphicView([[maybe_unused]] RS_GraphicView* gview) {
    // todo - add further processing later
}

void QG_LibraryWidget::setActionHandler(const QG_ActionHandler* ah) {
    m_actionHandler = ah;
}

/**
 * Escape releases focus.
 */
void QG_LibraryWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Escape: emit escape();
            break;
        default:
            QWidget::keyPressEvent(e);
            break;
    }
}

/**
 * Insert.
 */
void QG_LibraryWidget::insert() {
    const QItemSelectionModel* selIconView = m_ivPreview->selectionModel();
    const QModelIndex idx = selIconView->currentIndex();
    const QStandardItem* item = m_iconModel->itemFromIndex(idx);
    if (item == nullptr) {
        return;
    }

    const QString dxfPath = item->data(Qt::UserRole).toString();

    if (QFileInfo(dxfPath).isReadable()) {
        if (m_actionHandler != nullptr) {
            const std::shared_ptr<RS_ActionInterface> a = m_actionHandler->setCurrentAction(RS2::ActionLibraryInsert);
            if (a) {
                const auto* action = static_cast<LC_ActionBlockLibraryInsert*>(a.get());
                action->setFile(dxfPath);
            }
            else {
                RS_DEBUG->print(RS_Debug::D_ERROR, "QG_LibraryWidget::insert:" "Cannot create action RS_ActionLibraryInsert");
            }
        }
    }
    else {
        RS_DEBUG->print(RS_Debug::D_ERROR, "QG_LibraryWidget::insert: Can't read file: '%s'", dxfPath.toLatin1().data());
    }
}

/**
 * Refresh
 */
void QG_LibraryWidget::refresh() {
    scanTree();
    updatePreview(m_dirView->selectionModel()->currentIndex());
}

/**
 * Scan library tree for new items
 */
void QG_LibraryWidget::scanTree() {
    QStringList directoryList = RS_SYSTEM->getDirectoryList("library");
    foreach(auto& directory, directoryList) {
        appendTree(nullptr, directory);
    }

    const QString customPath = LC_GET_ONE_STR("Paths", "Library", "");

    if (customPath.size() > 0) {
        //todo: make the custom path more flexible
        appendTree(nullptr, customPath);
    }
}

/**
 * (Re)build dirModel and iconModel from scratch
 */
void QG_LibraryWidget::buildTree() {
    m_dirModel = std::make_unique<QStandardItemModel>();
    m_iconModel = std::make_unique<QStandardItemModel>();
    scanTree();
    m_dirView->setModel(m_dirModel.get());
    m_ivPreview->setModel(m_iconModel.get());
    m_dirModel->setHorizontalHeaderLabels(QStringList(tr("Directories")));
    if (!m_search->text().trimmed().isEmpty())
        updatePreview({});
}

/**
 * Appends the given directory to the given list view item. Called recursively until all
 * library directories are appended.
 *
 * @author Rallaz
 */
void QG_LibraryWidget::appendTree(QStandardItem* item, const QString& directory) {
    //    QStringList::Iterator it;
    const QDir dir(directory);

    if (!dir.exists()) {
        return;
    }

    // read subdirectories of this directory:
    QStringList lDirectoryList = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);

    if (item == nullptr) {
        item = m_dirModel->invisibleRootItem();
    }

    foreach(const auto& lDirectory, lDirectoryList) {
        QStandardItem* newItem = nullptr;

        // Look for an item already existing and take this
        //   instead of making a new one:
        for (int j = 0; j < item->rowCount(); ++j) {
            QStandardItem* const searchItem = item->child(j);
            if (searchItem->text() == lDirectory) {
                newItem = searchItem;
                break;
            }
        }

        // Create new item if no existing was found:
        if (newItem == nullptr) {
            newItem = new QStandardItem(QIcon(":/icons/folderclosed.lci"), lDirectory);
            item->setChild(item->rowCount(), newItem);
        }
        appendTree(newItem, directory + QDir::separator() + lDirectory);
    }
    item->sortChildren(0, Qt::AscendingOrder);
}

/**
 * Change the icon item when is expanded.
 *
 * @author Rallaz
 */
void QG_LibraryWidget::expandView(const QModelIndex& idx) const {
    QStandardItem* item = m_dirModel->itemFromIndex(idx);
    if (item != nullptr) {
        item->setIcon(QIcon(":/icons/fileopen.lci"));
    }
}

/**
 * Change the icon item when is collapsed.
 *
 * @author Rallaz
 */
void QG_LibraryWidget::collapseView(const QModelIndex& idx) const {
    QStandardItem* item = m_dirModel->itemFromIndex(idx);
    if (item != nullptr) {
        item->setIcon(QIcon(":/icons/folderclosed.lci"));
    }
}

QLayout* QG_LibraryWidget::getTopLevelLayout() const {
    return layout();
}

/**
 * Updates the icon preview.
 *
 * @author Rallaz
 */
void QG_LibraryWidget::updatePreview(const QModelIndex& idx) {
    const auto* item = m_dirModel->itemFromIndex(idx);
    const QString query = m_search->text().trimmed();
    m_iconModel->clear();
    if (!item && query.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList roots = RS_SYSTEM->getDirectoryList("library");
    const auto custom = LC_GET_ONE_STR("Paths", "Library", "");
    if (!custom.isEmpty())
        roots.append(custom);
    const auto directory = getItemDir(item);
    QSet<QString> seen;
    for (const auto& root : roots) {
        const auto searchRoot = query.isEmpty() ? root + directory : root;
        QDirIterator files(searchRoot, QStringList{"*.dxf", "*.DXF"}, QDir::Files | QDir::Readable,
                           query.isEmpty() ? QDirIterator::NoIteratorFlags : QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QFileInfo file(files.next());
            const auto label = file.completeBaseName();
            if ((!query.isEmpty() && !label.contains(query, Qt::CaseInsensitive)) ||
                seen.contains(file.canonicalFilePath()))
                continue;
            seen.insert(file.canonicalFilePath());
            const auto folder = QDir(root).relativeFilePath(file.absolutePath());
            const auto icon = getIcon(file.absoluteFilePath());
            auto* entry = new QStandardItem(icon, label);
            entry->setData(file.absoluteFilePath(), Qt::UserRole);
            entry->setToolTip(folder == "." ? file.fileName() : folder + '/' + file.fileName());
            m_iconModel->appendRow(entry);
        }
    }
    m_iconModel->sort(0);
    QApplication::restoreOverrideCursor();
}

//RLZ change to do-while
/**
 * @return Directory (in terms of the List view) to the given item (e.g. /mechanical/screws)
 */
QString QG_LibraryWidget::getItemDir(const QStandardItem* item) {
    if (item == nullptr) {
        return {};
    }

    const QStandardItem* parent = item->parent();
    return getItemDir(parent) + QDir::separator() + QString("%1").arg(item->text());
}

/**
 * @return Pixmap that serves as icon for the given DXF File.
 * The existing PNG file is returned or created and returned..
 *
 * @param dxfPath Full path to the existing DXF file on disk
 *                          (e.g. /home/tux/.qcad/library/mechanical/screws/screw1.dxf)
 */
QIcon QG_LibraryWidget::getIcon(const QString& dxfPath) {
    const QString pngFile = getPathToPixmap(dxfPath);
    const QFileInfo fiPng(pngFile);

    // found existing thumbnail:
    if (fiPng.isFile()) {
        return QIcon(pngFile);
    }
    // default thumbnail:
    return QIcon(QPixmap(64, 64));
}

// fixme - sand - files - generation of thumbnails should be extracted!!! Do this on general rework of the library

/**
 * @return Path to the thumbnail of the given DXF file. If no thumbnail exists, one is
 * created in the user's home. If no thumbnail can be created, an empty string is returned.
 */
QString QG_LibraryWidget::getPathToPixmap(const QString& dxfPath) {
    const QFileInfo drawing(dxfPath);
    const QFileInfo sibling(drawing.dir().filePath(drawing.completeBaseName() + ".png"));
    if (sibling.isFile() && sibling.lastModified() >= drawing.lastModified())
        return sibling.absoluteFilePath();

    const QDir cache(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!cache.mkpath("iconCache"))
        return {};
    const auto key = QCryptographicHash::hash(drawing.canonicalFilePath().toUtf8(),
                                              QCryptographicHash::Sha256).toHex();
    const QString pngPath = cache.filePath("iconCache/" + QString::fromLatin1(key) + ".png");
    const QFileInfo cached(pngPath);
    if (cached.isFile() && cached.lastModified() >= drawing.lastModified())
        return pngPath;

    QPixmap buffer(128, 128); // fixme - sand - add settings for thumbnail size, generate per setting!
    RS_Painter painter(&buffer);
    painter.setBackground(RS_Color(255, 255, 255));
    painter.eraseRect(0, 0, 128, 128);

    LC_GraphicViewport viewport;
    viewport.setSize(128, 128);

    RS_Graphic graphic;

    LC_DocumentsStorage storage;

    if (!storage.loadDocument(&graphic, dxfPath, RS2::FormatUnknown)) {
        RS_DEBUG->print(RS_Debug::D_ERROR, "QG_LibraryWidget::getPathToPixmap: Cannot open file: '%s'", dxfPath.toLatin1().data());
        return {};
    }

    viewport.setDocument(&graphic);
    viewport.initAfterDocumentOpen();
    viewport.zoomAuto(false);

    LC_PrintViewportRenderer renderer(&viewport, &painter);
    renderer.loadSettings();
    renderer.setupPainter(&painter);

    for (RS_Entity* e : lc::LC_ContainerTraverser{graphic, RS2::ResolveAll}.entities()) {
        if (e != nullptr && e->rtti() != RS2::EntityHatch) {
            RS_Pen pen = e->getPen();
            pen.setColor(Qt::black);
            e->setPen(pen);
            renderer.justDrawEntity(&painter, e);
        }
    }

    // GraphicView deletes painter
    painter.end();

    // Write to PNG
    writePng(pngPath, std::move(buffer));
    LC_LOG << "Writing to " << pngPath << " OK";
    return pngPath;
}
