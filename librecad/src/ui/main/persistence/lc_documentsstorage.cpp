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

#include "lc_documentsstorage.h"

#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <set>
#include <cmath>
#include <QScopeGuard>
#include <map>
#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "lc_filedialogservice.h"
#include "lc_entitymetauiutils.h"
#include "qg_filedialog.h"
#include "rs_debug.h"
#include "rs_dialogfactory.h"
#include "rs_dialogfactoryinterface.h"
#include "rs_document.h"
#include "rs_fileio.h"
#include "rs_filterdxfrw.h"
#include "rs_insert.h"
#include "rs_graphic.h"
#include "rs_block.h"
#include "rs_arc.h"
#include "rs_hatch.h"
#include "rs_dimangular.h"
#include "rs_dimlinear.h"
#include "rs_mtext.h"
#include "rs_text.h"
#include "rs_polyline.h"
#include "rs_layer.h"
#include "rs_graphicview.h"
#include "rs_settings.h"

namespace {
#ifdef DWGSUPPORT
    class AcadrustSession final : public QObject {
    public:
        explicit AcadrustSession(RS_GraphicView* parent) : QObject(parent) {
            setObjectName("fablecad-acadrust-session");
        }

        ~AcadrustSession() override {
            process.closeWriteChannel();
            if (process.state() != QProcess::NotRunning && !process.waitForFinished(500)) {
                process.kill();
                process.waitForFinished(500);
            }
        }

        static AcadrustSession* get(const RS_Graphic& drawing) {
            const auto* view = drawing.getGraphicView();
            return view ? dynamic_cast<AcadrustSession*>(
                view->findChild<QObject*>("fablecad-acadrust-session", Qt::FindDirectChildrenOnly)) : nullptr;
        }

        bool open(const QString& helper, const QString& input, const QString& projection) {
            process.start(helper, QStringList{});
            if (!process.waitForStarted(3000)) {
                error = process.errorString();
                return false;
            }
            const auto result = call({{"command", "open"}, {"path", input}, {"projection", projection}});
            if (!result.contains("entities"))
                return false;
            source = QFileInfo(input).canonicalFilePath();
            sourceVersion = result.value("source_version").toString();
            const auto& code = sourceVersion;
            if (code == "AC1015") {
                originalType = RS2::FormatDWG;
                originalRelease = "R2000";
            } else if (code == "AC1018") {
                originalType = RS2::FormatDWG2004;
                originalRelease = "R2004";
            } else if (code == "AC1021") {
                originalType = RS2::FormatDWG2007;
                originalRelease = "R2007";
            } else if (code == "AC1024") {
                originalType = RS2::FormatDWG2010;
                originalRelease = "R2010";
            } else if (code == "AC1027") {
                originalType = RS2::FormatDWG2013;
                originalRelease = "R2013";
            }
            const auto entities = result.value("entities").toObject();
            for (auto it = entities.begin(); it != entities.end(); ++it) {
                bool valid = false;
                const auto handle = it.key().toUInt(&valid);
                if (!valid || handle == 0) {
                    error = QObject::tr("A DWG entity identifier is outside the supported range.");
                    return false;
                }
                dwgGeometry.emplace(handle, it.value().toObject());
            }
            dwgAnnotations = result.value("annotations").toObject();
            dwgBlocks = result.value("blocks").toObject();
            return true;
        }

        bool captureBaseline(RS_Graphic& drawing) {
            for (auto* layer : *drawing.getLayerList()) {
                layerOrder.push_back(layer);
                layerNames.emplace(layer, layer->getName());
            }
            for (auto* entity : drawing) {
                if (!entity || !entity->isAlive() || !supportedType(*entity))
                    continue;
                const auto handle = entity->sourceHandle();
                if (handle == 0) {
                    fallbackEntities.insert(entity->getId());
                    continue;
                }
                const auto raw = dwgGeometry.find(handle);
                const auto projected = geometry(*entity);
                const auto kind = annotationKind(*entity);
                const bool matches = kind.isEmpty()
                    ? raw != dwgGeometry.end() && sameProjection(projected, raw->second)
                    : dwgAnnotations.value(QString::number(handle)).toString() == kind;
                if (!projected.isEmpty() && matches) {
                    editableGeometry.emplace(handle, projected);
                    originalPens.emplace(handle, entity->getPen(false));
                }
            }
            for (unsigned i = 0; i < drawing.countBlocks(); ++i) {
                auto* block = drawing.blockAt(i);
                if (!block || block->isDeleted())
                    continue;
                originalBlocks.insert(block);
                const auto raw = dwgBlocks.value(block->getName()).toObject();
                if (raw.isEmpty() || !sameValue(point(block->getBasePoint()), raw.value("base")))
                    continue;
                const auto rawMembers = raw.value("members").toObject();
                BlockState state;
                state.name = block->getName();
                state.base = point(block->getBasePoint());
                bool compatible = true;
                for (auto* member : *block) {
                    if (!member || !member->isAlive())
                        continue;
                    const auto handle = member->sourceHandle();
                    const auto shape = memberGeometry(*member);
                    const auto source = rawMembers.value(QString::number(handle)).toObject();
                    if (handle == 0 || shape.isEmpty() || source.isEmpty() ||
                        !sameProjection(shape, source) ||
                        !state.pens.emplace(handle, member->getPen(false)).second) {
                        compatible = false;
                        break;
                    }
                    state.members.append(QJsonObject{{"handle", static_cast<qint64>(handle)}, {"geometry", shape}});
                }
                if (compatible && state.members.size() == rawMembers.size())
                    editableBlocks.emplace(block, std::move(state));
            }
            dwgBlocks = {};
            dwgGeometry.clear();
            dwgAnnotations = {};
            std::map<quint32, QJsonObject> captured;
            QJsonArray initialEdits;
            std::vector<RS_Entity*> excluded;
            return collectBlocks(drawing, {}, initialEdits, excluded) && initialEdits.isEmpty() &&
                capture(drawing, captured, baseline, nullptr, nullptr, nullptr, nullptr, excluded) &&
                captured == editableGeometry;
        }

        bool isOriginalFormat(RS2::FormatType type) const {
            return originalType != RS2::FormatUnknown && type == originalType;
        }

        bool protectsSource(const QString& path) const {
            const QFileInfo target(path);
            return target.exists() && target.canonicalFilePath() == source;
        }

        LC_FileDialogService::FileDialogResult chooseCopy(const QString& currentName,
                                                          RS2::FormatType preferred) const {
            const auto format = recommendOriginal ? originalType : preferred;
            const QFileInfo info(currentName);
            const auto name = protectsSource(currentName)
                ? info.dir().filePath(info.completeBaseName() + QObject::tr(" - copy.dwg"))
                : currentName;
            const auto label = originalRelease.isEmpty() ? QString{}
                : QObject::tr("DWG %1 (original format) (*.dwg)").arg(originalRelease);
            return LC_FileDialogService::getFileDetails(LC_FileDialogService::SaveDrawing,
                                                       name, format, originalType, label);
        }

        QString diagnostics(const RS_Graphic& drawing) const {
            std::map<RS2::EntityType, int> limited;
            for (const auto* entity : drawing) {
                if (entity && entity->isAlive() && entity->sourceHandle() &&
                    !editableGeometry.count(entity->sourceHandle()))
                    ++limited[entity->rtti()];
            }
            QStringList lines{QObject::tr("Source format: %1").arg(sourceVersion),
                              QObject::tr("Output format: DWG 2018 (AC1032)")};
            if (limited.empty()) {
                lines.append(QObject::tr("No model-space entities with restricted DWG editing were identified."));
            } else {
                lines.append(QObject::tr("Model-space entities with restricted DWG editing:"));
                for (const auto& [name, type] : LC_EntityMetaUIUtils::getEntityTypeNamesList()) {
                    auto found = limited.find(type);
                    if (found != limited.end()) {
                        lines.append(QObject::tr("%1: %2").arg(name).arg(found->second));
                        limited.erase(found);
                    }
                }
                for (const auto& [type, count] : limited)
                    lines.append(QObject::tr("Entity type %1: %2").arg(static_cast<int>(type)).arg(count));
            }
            lines.append(QObject::tr("The copy is reopened and checked before it is written to the destination."));
            return lines.join(QLatin1Char('\n'));
        }

        bool save(RS_Graphic& drawing, const QString& destination, bool keepOriginalFormat,
                  bool workingCopy = false) {
            const bool previousRecommendation = recommendOriginal;
            const auto restoreRecommendation = qScopeGuard([&] {
                if (workingCopy)
                    recommendOriginal = previousRecommendation;
            });
            const QFileInfo target(destination);
            QJsonArray layerEdits;
            QHash<QString, QString> renamed;
            if (!collectLayers(drawing, layerEdits, renamed)) {
                error = QObject::tr("The experimental DWG session cannot save these layer changes.");
                return false;
            }
            QJsonArray blockEdits;
            std::vector<RS_Entity*> excludedBlocks;
            if (!collectBlocks(drawing, renamed, blockEdits, excludedBlocks)) {
                error = QObject::tr("The experimental DWG session cannot save these block definition changes.");
                return false;
            }
            QJsonArray operations;
            QByteArray untouched;
            std::map<quint32, QJsonObject> remaining;
            std::vector<RS_Entity*> annotationChanges;
            QJsonArray annotationEdits;
            if (!capture(drawing, remaining, untouched, &operations, &annotationChanges, &annotationEdits, &renamed, excludedBlocks) ||
                untouched != baseline) {
                error = QObject::tr("This experimental DWG session cannot save one or more of these changes.");
                return false;
            }
            for (const auto& entry : remaining) {
                if (!entry.second.contains("annotation") && entry.second != originalShape(entry.first, &renamed)) {
                    operations.append(QJsonObject{{"kind", "update"},
                                                  {"handle", static_cast<qint64>(entry.first)},
                                                  {"geometry", entry.second}});
                }
            }
            for (const auto& entry : editableGeometry) {
                if (remaining.count(entry.first) == 0) {
                    operations.append(QJsonObject{{"kind", "delete"},
                                                  {"handle", static_cast<qint64>(entry.first)}});
                }
            }
            QTemporaryDir staging(target.dir().filePath(".fablecad-acadrust-XXXXXX"));
            if (!staging.isValid()) {
                error = staging.errorString();
                return false;
            }
            const auto path = staging.filePath("candidate.dwg");
            QJsonObject request{{"command", "save_edited"}, {"path", path}, {"operations", operations},
                                {"layer_edits", layerEdits}, {"block_edits", blockEdits},
                                {"original_format", keepOriginalFormat}};
            if (!annotationChanges.empty()) {
                const auto exportPath = staging.filePath("annotations.dxf");
                if (!exportAnnotations(drawing, annotationChanges, exportPath)) {
                    error = QObject::tr("Cannot export the edited annotations.");
                    return false;
                }
                request.insert("annotations", QJsonObject{{"path", exportPath}, {"edits", annotationEdits}});
            }
            if (call(request).isEmpty())
                return false;
            QFile input(path);
            QSaveFile output(destination);
            if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
                error = QObject::tr("Cannot publish the verified DWG.");
                return false;
            }
            QByteArray buffer(64 * 1024, Qt::Uninitialized);
            qint64 length;
            while ((length = input.read(buffer.data(), buffer.size())) > 0) {
                if (output.write(buffer.constData(), length) != length) {
                    error = output.errorString();
                    return false;
                }
            }
            if (length != 0 || !output.commit()) {
                error = length != 0 ? input.errorString() : output.errorString();
                return false;
            }
            recommendOriginal = false;
            return true;
        }

        QString error;

    private:
        QString sourceVersion;
        QJsonObject call(const QJsonObject& request) {
            if (process.state() != QProcess::Running) {
                error = QObject::tr("The experimental DWG process has stopped.");
                return {};
            }
            const auto line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
            if (process.write(line) != line.size()) {
                error = process.errorString();
                return {};
            }
            QElapsedTimer timer;
            timer.start();
            while (!process.canReadLine()) {
                const auto remaining = 30000 - timer.elapsed();
                if (remaining <= 0 || !process.waitForReadyRead(static_cast<int>(remaining))) {
                    process.kill();
                    error = QObject::tr("The experimental DWG process did not return a result.");
                    return {};
                }
            }
            const auto response = QJsonDocument::fromJson(process.readLine()).object();
            if (!response.value("ok").toBool()) {
                error = response.value("error").toString();
                if (error.contains("Cannot encode custom object DATATABLE")) {
                    recommendOriginal = originalType != RS2::FormatUnknown;
                    error = recommendOriginal
                        ? QObject::tr("This DATATABLE requires the original DWG format. In Save As, select DWG %1 (original format) and choose a different file name.").arg(originalRelease)
                        : QObject::tr("The DWG contains a DATATABLE record that cannot be converted to DWG R2018.");
                }
                if (error.isEmpty())
                    error = QObject::tr("The experimental DWG process returned an invalid result.");
                return {};
            }
            return response.value("result").toObject();
        }

        static QJsonArray point(const RS_Vector& value) {
            return {value.x, value.y, value.z};
        }

        static bool supportedType(const RS_Entity& entity) {
            switch (entity.rtti()) {
            case RS2::EntityLine:
            case RS2::EntityCircle:
            case RS2::EntityArc:
            case RS2::EntityPolyline:
            case RS2::EntityInsert:
            case RS2::EntityHatch:
            case RS2::EntityText:
            case RS2::EntityMText:
            case RS2::EntityDimLinear:
            case RS2::EntityDimAngular:
                return true;
            default:
                return false;
            }
        }

        static QJsonArray planarPoint(const RS_Vector& value) {
            return {value.x, value.y};
        }

        static double angle(double value) {
            constexpr double turn = 2.0 * M_PI;
            const double remainder = std::fmod(value, turn);
            return remainder < 0.0 ? remainder + turn : remainder;
        }

        static QString annotationKind(const RS_Entity& entity) {
            switch (entity.rtti()) {
            case RS2::EntityHatch: return QStringLiteral("hatch");
            case RS2::EntityText: return QStringLiteral("text");
            case RS2::EntityMText: return QStringLiteral("mtext");
            case RS2::EntityDimLinear: return QStringLiteral("dim_linear");
            case RS2::EntityDimAngular: return QStringLiteral("dim_angular");
            default: return {};
            }
        }

        static QJsonObject annotationGeometry(const RS_Entity& entity, const QString& kind) {
            QJsonObject result{{"annotation", kind}};
            if (kind == "hatch") {
                const auto& hatch = static_cast<const RS_Hatch&>(entity);
                QJsonArray loops;
                for (const auto& loop : hatch.exportLoops()) {
                    if (!loop.edges)
                        return {};
                    QJsonArray edges;
                    for (const auto* edge : *loop.edges) {
                        if (edge->rtti() == RS2::EntityLine) {
                            edges.append(QJsonArray{point(edge->getStartpoint()), point(edge->getEndpoint())});
                        } else if (edge->rtti() == RS2::EntityArc) {
                            const auto& arc = static_cast<const RS_Arc&>(*edge);
                            edges.append(QJsonArray{point(arc.getCenter()), arc.getRadius(),
                                arc.getAngle1(), arc.getAngle2(), arc.isReversed()});
                        } else if (edge->rtti() == RS2::EntityCircle) {
                            edges.append(QJsonArray{point(edge->getCenter()), edge->getRadius()});
                        } else {
                            return {};
                        }
                    }
                    if (edges.isEmpty())
                        return {};
                    loops.append(QJsonArray{loop.depth, edges});
                }
                if (loops.isEmpty())
                    return {};
                result.insert("loops", loops);
                result.insert("solid", hatch.isSolid());
                result.insert("pattern", hatch.getPattern());
                result.insert("transform", QJsonArray{hatch.getAngle(), hatch.getScale()});
            } else if (kind == "text") {
                const auto data = static_cast<const RS_Text&>(entity).getData();
                if (data.textGeneration != RS_TextData::None)
                    return {};
                result.insert("style", data.style);
                result.insert("text", data.text);
                result.insert("position", point(data.insertionPoint));
                result.insert("second", point(data.secondPoint));
                result.insert("size", QJsonArray{data.height, data.widthRel, data.angle});
                result.insert("alignment", QJsonArray{data.valign, data.halign, data.textGeneration});
            } else if (kind == "mtext") {
                const auto data = static_cast<const RS_MText&>(entity).getData();
                if (data.legacyRtlLayout || data.drawingDirection == RS_MTextData::RightToLeft)
                    return {};
                result.insert("style", data.style);
                result.insert("text", data.text);
                result.insert("position", point(data.insertionPoint));
                result.insert("size", QJsonArray{data.height, data.width, data.angle, data.lineSpacingFactor});
                result.insert("alignment", QJsonArray{data.valign, data.halign, data.drawingDirection,
                                                      data.lineSpacingStyle, data.legacyRtlLayout});
            } else {
                const auto& dim = static_cast<const RS_Dimension&>(entity);
                if (dim.getDimStyleOverride() || dim.isFlipArrow1() || dim.isFlipArrow2() || dim.getTextAngle() != 0.0)
                    return {};
                result.insert("style", dim.getStyle());
                result.insert("text", dim.getText());
                result.insert("position", point(dim.getDefinitionPoint()));
                result.insert("alignment", QJsonArray{dim.getHAlign(), dim.getVAlign(), dim.getTextAngle(),
                    dim.getHDir(), dim.getLineSpacingStyle(), dim.getLineSpacingFactor(),
                    dim.hasUserDefinedTextLocation()});
                if (dim.hasUserDefinedTextLocation())
                    result.insert("text_position", point(dim.getMiddleOfText()));
                if (kind == "dim_linear") {
                    const auto& linear = static_cast<const RS_DimLinear&>(entity);
                    result.insert("extension", QJsonArray{point(linear.getExtensionPoint1()),
                        point(linear.getExtensionPoint2()), linear.getAngle(), linear.getOblique()});
                } else {
                    const auto& angular = static_cast<const RS_DimAngular&>(entity);
                    result.insert("extension", QJsonArray{point(angular.getDefinitionPoint1()),
                        point(angular.getDefinitionPoint2()), point(angular.getDefinitionPoint3()),
                        point(angular.getDefinitionPoint4())});
                }
            }
            return result;
        }

        static bool exportAnnotations(RS_Graphic& drawing, const std::vector<RS_Entity*>& entities,
                                      const QString& path) {
            const std::set<RS_Entity*> selected(entities.begin(), entities.end());
            std::vector<RS_Entity*> excluded;
            const auto restore = qScopeGuard([&] {
                for (auto* entity : excluded)
                    entity->delFlag(RS2::FlagDeleted);
            });
            for (auto* entity : drawing) {
                if (entity && entity->isAlive() && selected.count(entity) == 0) {
                    excluded.push_back(entity);
                    entity->setFlag(RS2::FlagDeleted);
                }
            }
            return RS_FileIO::instance()->fileExport(drawing, path, RS2::FormatDXFRW2018);
        }

        static QJsonObject geometry(const RS_Entity& entity) {
            const auto* layer = entity.getLayer(false);
            if (!layer)
                return {};
            const auto kind = annotationKind(entity);
            if (!kind.isEmpty()) {
                auto result = annotationGeometry(entity, kind);
                if (!result.isEmpty())
                    result.insert("layer", layer->getName());
                return result;
            }
            QJsonObject result{{"layer", layer->getName()}};
            switch (entity.rtti()) {
            case RS2::EntityLine:
                result.insert("shape", "line");
                result.insert("start", point(entity.getStartpoint()));
                result.insert("end", point(entity.getEndpoint()));
                break;
            case RS2::EntityCircle:
                result.insert("shape", "circle");
                result.insert("center", planarPoint(entity.getCenter()));
                result.insert("radius", entity.getRadius());
                break;
            case RS2::EntityArc: {
                const auto& arc = static_cast<const RS_Arc&>(entity);
                result.insert("shape", "arc");
                result.insert("center", planarPoint(arc.getCenter()));
                result.insert("radius", arc.getRadius());
                result.insert("start_angle", angle(arc.isReversed() ? arc.getAngle2() : arc.getAngle1()));
                result.insert("end_angle", angle(arc.isReversed() ? arc.getAngle1() : arc.getAngle2()));
                break;
            }
            case RS2::EntityInsert: {
                const auto data = static_cast<const RS_Insert&>(entity).getData();
                if (data.cols != 1 || data.rows != 1 || data.extrusion.x != 0.0 ||
                    data.extrusion.y != 0.0 || data.extrusion.z != 1.0 ||
                    data.spacing.x != 0.0 || data.spacing.y != 0.0)
                    return {};
                result.insert("shape", "insert");
                result.insert("block", data.name);
                result.insert("position", point(data.insertionPoint));
                result.insert("scale", point(data.scaleFactor));
                result.insert("rotation", angle(data.angle));
                break;
            }
            case RS2::EntityPolyline: {
                const auto& polyline = static_cast<const RS_Polyline&>(entity);
                QJsonArray vertices;
                RS_Vector endpoint;
                for (const auto* segment : polyline) {
                    if (!segment || !segment->isAlive())
                        continue;
                    if (segment->rtti() != RS2::EntityLine && segment->rtti() != RS2::EntityArc)
                        return {};
                    const auto start = segment->getStartpoint();
                    if (!vertices.isEmpty() && start.distanceTo(endpoint) > 1e-8)
                        return {};
                    const auto bulge = segment->rtti() == RS2::EntityArc
                        ? static_cast<const RS_Arc*>(segment)->getBulge() : 0.0;
                    vertices.append(QJsonArray{start.x, start.y, bulge});
                    endpoint = segment->getEndpoint();
                }
                if (!polyline.isClosed() && !vertices.isEmpty())
                    vertices.append(QJsonArray{endpoint.x, endpoint.y, 0.0});
                if (vertices.size() < 2)
                    return {};
                if (polyline.isClosed()) {
                    const auto first = vertices.at(0).toArray();
                    if (endpoint.distanceTo(RS_Vector(first.at(0).toDouble(), first.at(1).toDouble())) > 1e-8)
                        return {};
                }
                result.insert("shape", "polyline");
                result.insert("closed", polyline.isClosed());
                result.insert("vertices", vertices);
                break;
            }
            default:
                return {};
            }
            return result;
        }

        static bool sameValue(const QJsonValue& a, const QJsonValue& b) {
            if (a == b)
                return true;
            if (a.isDouble() && b.isDouble())
                return std::abs(a.toDouble() - b.toDouble()) <= 1e-8;
            if (!a.isArray() || !b.isArray())
                return false;
            const auto left = a.toArray();
            const auto right = b.toArray();
            if (left.size() != right.size())
                return false;
            for (qsizetype i = 0; i < left.size(); ++i) {
                if (!sameValue(left.at(i), right.at(i)))
                    return false;
            }
            return true;
        }

        static bool sameProjection(const QJsonObject& a, const QJsonObject& b) {
            if (a.size() != b.size())
                return false;
            for (auto it = a.begin(); it != a.end(); ++it) {
                const auto original = b.value(it.key());
                if (it.key() == "start_angle" || it.key() == "end_angle" || it.key() == "rotation") {
                    if (!original.isDouble() ||
                        std::abs(std::remainder(it.value().toDouble() - original.toDouble(), 2.0 * M_PI)) > 1e-8)
                        return false;
                } else if (!sameValue(it.value(), original)) {
                    return false;
                }
            }
            return true;
        }

        struct BlockState {
            QString name;
            QJsonArray base;
            QJsonArray members;
            std::map<quint32, RS_Pen> pens;
        };

        static QJsonObject memberGeometry(const RS_Entity& member) {
            switch (member.rtti()) {
            case RS2::EntityLine:
            case RS2::EntityCircle:
            case RS2::EntityArc:
            case RS2::EntityPolyline:
                return geometry(member);
            default:
                return {};
            }
        }

        static bool newMemberSupported(const RS_Entity& entity) {
            const auto pen = entity.getPen(false);
            return pen.isColorByLayer() && pen.isWidthByLayer() && pen.isLineTypeByLayer() &&
                pen.isFullyOpaque() && entity.getDrwExtData().empty();
        }

        bool collectBlocks(RS_Graphic& drawing, const QHash<QString, QString>& renamed,
                           QJsonArray& edits, std::vector<RS_Entity*>& excluded) const {
            std::size_t existingCount = 0;
            for (unsigned i = 0; i < drawing.countBlocks(); ++i) {
                auto* block = drawing.blockAt(i);
                if (!block || block->isDeleted())
                    continue;
                const bool create = originalBlocks.count(block) == 0;
                const auto found = editableBlocks.find(block);
                if (!create)
                    ++existingCount;
                if (!create && found == editableBlocks.end())
                    continue;
                const auto name = block->getName();
                const auto base = point(block->getBasePoint());
                const auto* original = create ? nullptr : &found->second;
                if (original && (name != original->name || base != original->base))
                    return false;
                QJsonArray members;
                QSet<quint32> seen;
                for (auto* member : *block) {
                    if (!member || !member->isAlive())
                        continue;
                    const auto shape = memberGeometry(*member);
                    if (shape.isEmpty())
                        return false;
                    QJsonObject item{{"geometry", shape}};
                    const auto handle = member->sourceHandle();
                    if (original && handle != 0) {
                        const auto stored = original->pens.find(handle);
                        const auto pen = member->getPen(false);
                        if (stored == original->pens.end() || seen.contains(handle) ||
                            pen != stored->second || pen.getAlpha() != stored->second.getAlpha() ||
                            pen.getLineTypeId() != stored->second.getLineTypeId())
                            return false;
                        seen.insert(handle);
                        item.insert("handle", static_cast<qint64>(handle));
                    } else if (!newMemberSupported(*member)) {
                        return false;
                    }
                    members.append(item);
                    if (!create)
                        excluded.push_back(member);
                }
                if (create) {
                    if (name.startsWith('*') || block->getInsertionUnits() != 0)
                        return false;
                    excluded.push_back(block);
                } else {
                    QJsonArray reference;
                    for (const auto& value : original->members) {
                        auto item = value.toObject();
                        auto shape = item.value("geometry").toObject();
                        const auto layer = shape.value("layer").toString();
                        if (renamed.contains(layer)) {
                            shape.insert("layer", renamed.value(layer));
                            item.insert("geometry", shape);
                        }
                        reference.append(item);
                    }
                    if (members == reference)
                        continue;
                }
                edits.append(QJsonObject{{"name", name}, {"base", base}, {"create", create}, {"members", members}});
            }
            return existingCount == originalBlocks.size();
        }

        bool collectLayers(RS_Graphic& drawing, QJsonArray& edits,
                           QHash<QString, QString>& renamed) const {
            auto* layers = drawing.getLayerList();
            QSet<QString> originalNames;
            QSet<QString> currentNames;
            for (const auto& layer : layerNames)
                originalNames.insert(layer.second.toCaseFolded());
            for (const auto* layer : *layers) {
                const auto key = layer->getName().toCaseFolded();
                if (currentNames.contains(key))
                    return false;
                currentNames.insert(key);
            }
            for (auto* layer : layerOrder) {
                if (!layers->contains(layer))
                    return false;
                const auto original = layerNames.at(layer);
                const auto current = layer->getName();
                if (original == current)
                    continue;
                if (original.compare(current, Qt::CaseInsensitive) != 0 &&
                    originalNames.contains(current.toCaseFolded()))
                    return false;
                renamed.insert(original, current);
                edits.append(QJsonObject{{"kind", "rename"}, {"old", original}, {"name", current}});
            }
            for (const auto* layer : *layers) {
                if (layerNames.count(layer))
                    continue;
                if (originalNames.contains(layer->getName().toCaseFolded()))
                    return false;
                const auto pen = layer->getPen();
                const auto color = pen.getColor();
                if (pen.getLineType() != RS2::SolidLine || layer->isConstruction() ||
                    !color.isValid() || !pen.isFullyOpaque())
                    return false;
                const auto hidden = layer->getHiddenAs();
                edits.append(QJsonObject{{"kind", "add"}, {"name", layer->getName()},
                    {"rgb", QJsonArray{color.red(), color.green(), color.blue()}},
                    {"width", pen.getWidth()}, {"locked", layer->isLocked()}, {"plot", layer->isPrint()},
                    {"frozen", layer->isFrozen() && hidden != RS_Layer::HiddenAs::Off},
                    {"off", layer->isFrozen() && hidden != RS_Layer::HiddenAs::Frozen}});
            }
            return true;
        }

        QJsonObject originalShape(quint32 handle, const QHash<QString, QString>* renamed) const {
            auto result = editableGeometry.at(handle);
            if (renamed) {
                const auto layer = result.value("layer").toString();
                if (renamed->contains(layer))
                    result.insert("layer", renamed->value(layer));
            }
            return result;
        }

        bool exportSnapshot(RS_Graphic& drawing, const QString& path) const {
            std::vector<std::pair<RS_Layer*, QString>> renamed;
            const auto restore = qScopeGuard([&] {
                for (const auto& layer : renamed)
                    layer.first->setName(layer.second);
            });
            for (auto* layer : layerOrder) {
                const auto& original = layerNames.at(layer);
                if (layer->getName() != original) {
                    renamed.emplace_back(layer, layer->getName());
                    layer->setName(original);
                }
            }
            return RS_FilterDXFRW::exportLayerSubset(drawing, path, layerOrder);
        }

        static QByteArray drawingContent(const QByteArray& bytes) {
            const auto rows = bytes.split('\n');
            QByteArray content;
            content.reserve(bytes.size());
            bool extents = false;
            bool activeLayer = false;
            bool viewport = false;
            bool active = false;
            for (qsizetype i = 0; i + 1 < rows.size(); i += 2) {
                const auto code = rows[i].trimmed();
                const auto value = rows[i + 1].trimmed();
                if (code == "9") {
                    extents = value == "$EXTMIN" || value == "$EXTMAX";
                    activeLayer = value == "$CLAYER";
                }
                if (code == "0") {
                    extents = false;
                    activeLayer = false;
                    viewport = value == "VPORT";
                    active = false;
                }
                if (viewport && code == "2")
                    active = value == "*ACTIVE";
                // Extents derive from geometry. The active view derives from the editor viewport.
                const bool generatedValue = (activeLayer && code == "8") || (extents && (code == "10" || code == "20" || code == "30")) ||
                    (active && (code == "12" || code == "22" || code == "40" || code == "41"));
                content += rows[i] + '\n';
                if (!generatedValue)
                    content += rows[i + 1];
                content += '\n';
            }
            if (rows.size() % 2 != 0)
                content += rows.back();
            return content;
        }

        bool capture(RS_Graphic& drawing, std::map<quint32, QJsonObject>& entities,
                     QByteArray& otherContent, QJsonArray* additions,
                     std::vector<RS_Entity*>* annotationChanges = nullptr,
                     QJsonArray* annotationEdits = nullptr,
                     const QHash<QString, QString>* renamed = nullptr,
                     std::vector<RS_Entity*> excluded = {}) {
            for (auto* entity : excluded)
                entity->setFlag(RS2::FlagDeleted);
            const auto restore = qScopeGuard([&] {
                for (auto* entity : excluded)
                    entity->delFlag(RS2::FlagDeleted);
            });
            for (auto* entity : drawing) {
                if (!entity || !entity->isAlive() || !supportedType(*entity))
                    continue;
                const auto handle = entity->sourceHandle();
                if (handle != 0 && editableGeometry.count(handle) == 0)
                    continue;
                if (handle == 0 && fallbackEntities.count(entity->getId()) != 0)
                    continue;
                const auto shape = geometry(*entity);
                if (shape.isEmpty())
                    return false;
                const auto pen = entity->getPen(false);
                if (handle != 0) {
                    const auto& originalPen = originalPens.at(handle);
                    if (pen != originalPen || pen.getAlpha() != originalPen.getAlpha() ||
                        pen.getLineTypeId() != originalPen.getLineTypeId() ||
                        !entities.emplace(handle, shape).second)
                        return false;
                } else if (additions) {
                    if (!pen.isColorByLayer() || !pen.isWidthByLayer() ||
                        !pen.isLineTypeByLayer() || !pen.isFullyOpaque() ||
                        !entity->getDrwExtData().empty())
                        return false;
                    if (!shape.contains("annotation"))
                        additions->append(QJsonObject{{"kind", "add"}, {"geometry", shape}});
                }
                if (annotationChanges && shape.contains("annotation") &&
                    (handle == 0 || shape != originalShape(handle, renamed))) {
                    if (shape.value("text").toString().contains("%<"))
                        return false;
                    annotationChanges->push_back(entity);
                    QJsonObject edit{{"kind", shape.value("annotation")}, {"preserve_style", false}};
                    if (handle != 0) {
                        edit.insert("handle", static_cast<qint64>(handle));
                        edit.insert("preserve_style", shape.value("style") == editableGeometry.at(handle).value("style"));
                    }
                    if (shape.value("annotation") == "mtext")
                        edit.insert("wrap_width", static_cast<const RS_MText*>(entity)->getWidth());
                    annotationEdits->append(edit);
                }
                excluded.push_back(entity);
                entity->setFlag(RS2::FlagDeleted);
            }
            QTemporaryDir temp;
            const auto path = temp.filePath("projection.dxf");
            if (!temp.isValid() ||
                !exportSnapshot(drawing, path))
                return false;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return false;
            otherContent = drawingContent(file.readAll());
            return file.error() == QFileDevice::NoError;
        }

        QProcess process;
        QString source;
        RS2::FormatType originalType = RS2::FormatUnknown;
        QString originalRelease;
        bool recommendOriginal = false;
        std::vector<RS_Layer*> layerOrder;
        std::map<const RS_Layer*, QString> layerNames;
        QJsonObject dwgBlocks;
        std::set<const RS_Block*> originalBlocks;
        std::map<const RS_Block*, BlockState> editableBlocks;
        QJsonObject dwgAnnotations;
        std::map<quint32, QJsonObject> dwgGeometry;
        std::map<quint32, QJsonObject> editableGeometry;
        std::map<quint32, RS_Pen> originalPens;
        std::set<unsigned long long> fallbackEntities;
        QByteArray baseline;
    };

    // Use the same document model and import/export filters as the editor.
    // Stage beside the target so relative references retain their base path.
    bool writeDwgWorkingCopy(RS_Graphic& drawing, const QString& target, bool replace) {
        const QDir directory = QFileInfo(target).absoluteDir();
        QTemporaryFile dxf(directory.filePath(".fablecad-XXXXXX.dxf"));
        QTemporaryFile dwg(directory.filePath(".fablecad-XXXXXX.dwg"));
        if (!dxf.open() || !dwg.open())
            return false;
        const QString dxfPath = dxf.fileName();
        const QString dwgPath = dwg.fileName();
        // The writers replace their destination. Release the native handles,
        // including on Windows, and clean the regenerated paths on return.
        if (!dxf.remove() || !dwg.remove())
            return false;
        const auto cleanup = qScopeGuard([&] { QFile::remove(dxfPath); QFile::remove(dwgPath); });
        auto* io = RS_FileIO::instance();
        const auto refusePartialRead = [](bool, const QString&) { return false; };
        RS_Graphic editable, reopened;
        if (!io->fileExport(drawing, dxfPath, RS2::FormatDXFRW2018) ||
            !io->fileImport(editable, dxfPath, RS2::FormatUnknown, refusePartialRead))
            return false;
        if (!io->fileExport(editable, dwgPath, RS2::FormatDWG2018) ||
            !io->fileImport(reopened, dwgPath, RS2::FormatUnknown, refusePartialRead))
            return false;
        const auto entitiesByType = [](const RS_EntityContainer& container) {
            std::map<RS2::EntityType, unsigned> counts;
            for (const RS_Entity* entity : container) {
                if (entity && entity->isAlive())
                    ++counts[entity->rtti()];
            }
            return counts;
        };
        const auto matchesDrawing = [&](RS_Graphic& other) {
            if (entitiesByType(drawing) != entitiesByType(other))
                return false;
            for (unsigned i = 0; i < drawing.countBlocks(); ++i) {
                const auto* block = drawing.blockAt(i);
                if (block && !block->isDeleted()) {
                    const auto* copy = other.findBlock(block->getName());
                    if (!copy || entitiesByType(*block) != entitiesByType(*copy))
                        return false;
                }
            }
            for (unsigned i = 0; i < drawing.countLayers(); ++i) {
                const auto* layer = drawing.layerAt(i);
                if (layer && !other.findLayer(layer->getName()))
                    return false;
            }
            return true;
        };
        if (!matchesDrawing(editable) || !matchesDrawing(reopened))
            return false;
        if (!replace)
            return QFile::rename(dwgPath, target); // never overwrite an existing drawing
        QFile input(dwgPath);
        QSaveFile output(target);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            return false;
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        qint64 size;
        while ((size = input.read(buffer.data(), buffer.size())) > 0) {
            if (output.write(buffer.constData(), size) != size)
                return false;
        }
        return size == 0 && output.commit();
    }
#endif

    bool writeVerifiedDxf(RS_Graphic& graphic, const QString& destination, RS2::FormatType format) {
        const auto info = QFileInfo(destination);
        QTemporaryFile staging(info.dir().filePath(QStringLiteral(".fablecad-XXXXXX.dxf")));
        if (!staging.open())
            return false;
        const auto path = staging.fileName();
        if (!staging.remove())
            return false;
        const auto cleanup = qScopeGuard([&] { QFile::remove(path); });

        auto* io = RS_FileIO::instance();
        RS_Graphic verification;
        const auto refusePartialRead = [](bool, const QString&) { return false; };
        if (!io->fileExport(graphic, path, format) ||
            !io->fileImport(verification, path, RS2::FormatUnknown, refusePartialRead))
            return false;

        QFile input(path);
        QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            return false;
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        qint64 size;
        while ((size = input.read(buffer.data(), buffer.size())) > 0) {
            if (output.write(buffer.constData(), size) != size)
                return false;
        }
        return size == 0 && output.commit();
    }

    QString validatedFileAffix(const QString& affix, const QString& fallback) {
        for (const QChar character : affix) {
            if (character.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(character)) {
                return fallback;
            }
        }
        return affix;
    }

    QString backupFilePath(const QString& path) {
#ifndef Q_OS_WIN
        const QFileInfo info(path);
        if (LC_GET_ONE_BOOL("Defaults", "HideBackupFiles", true) && !info.fileName().startsWith('.')) {
            return info.dir().filePath("." + info.fileName());
        }
#endif
        return path;
    }

    void hideBackupFile([[maybe_unused]] const QString& path) {
#ifdef Q_OS_WIN
        if (!LC_GET_ONE_BOOL("Defaults", "HideBackupFiles", true)) {
            return;
        }
        QString nativePath = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        if (!nativePath.startsWith("\\\\?\\")) {
            if (nativePath.startsWith("\\\\")) {
                nativePath = "\\\\?\\UNC\\" + nativePath.mid(2);
            } else {
                nativePath.prepend("\\\\?\\");
            }
        }
        const auto* fileName = reinterpret_cast<const wchar_t*>(nativePath.utf16());
        const DWORD attributes = GetFileAttributesW(fileName);
        if (attributes == INVALID_FILE_ATTRIBUTES
            || !SetFileAttributesW(fileName, (attributes & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_HIDDEN)) {
            const DWORD error = GetLastError();
            LC_ERR << "Could not hide backup file " << path << ": Windows error " << error;
        }
#endif
    }

    /**
     * Whether Save can write the drawing back to its file, in the format the
     * file was read from. It cannot for a format LibreCAD only reads, such as
     * a Shapefile or a DWG older than R2000.
     */
    bool canSaveInPlace(const RS_Graphic* graphic) {
#ifdef DWGSUPPORT
        if (const auto* session = AcadrustSession::get(*graphic);
            session && session->isOriginalFormat(graphic->getFormatType()))
            return true;
#endif
        return RS_FileIO::instance()->canExport(graphic->getFormatType());
    }

    QString recoveryCopyName(const QString& fileName) {
        const QFileInfo info(fileName);
        return info.dir().filePath(info.completeBaseName() + QStringLiteral("_recovered.dxf"));
    }
}

LC_DocumentsStorage::LC_DocumentsStorage() = default;

bool LC_DocumentsStorage::saveDocument(RS_Document* document, RS_GraphicView * graphicView,  bool &cancelled) {
    bool result = false;
    cancelled = false;

    if (document != nullptr) {
        document->setGraphicView(graphicView);
        RS_Graphic* graphic = document->getGraphic();
        const auto fileName = graphic->getFilename();
        // Keep the drawing view (and its DWG session) when saving from a block editor.
        if (document != graphic && graphic->getGraphicView())
            graphicView = graphic->getGraphicView();
        if (graphic->isDxfRecoverySourceFile(fileName)) {
            RS_DIALOGFACTORY->commandMessage(
                tr("The original legacy DXF is protected. Choose a different name for the recovered drawing."));
            result = doSaveGraphicAs(graphic, graphicView, cancelled,
                                     recoveryCopyName(fileName), graphic->getFormatType());
        } else if (fileName.isEmpty()) {
            result = doSaveGraphicAs(graphic, graphicView, cancelled);
        } else if (!canSaveInPlace(graphic)) {
            // writing another format under the file's name would destroy it
            RS_DIALOGFACTORY->commandMessage(
                tr("FableCAD cannot save \"%1\" in its own format. Choose a name to save the drawing as.")
                    .arg(QFileInfo(fileName).fileName()));
            result = doSaveGraphicAs(graphic, graphicView, cancelled, fileName, RS2::FormatDXFRW);
        } else {
            const QFileInfo info(fileName);
            if (!info.isWritable()) {
                return false;
            }
            QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
            result = doSave(graphic, true);
            QApplication::restoreOverrideCursor();
        }
    }
    return result;
}

bool LC_DocumentsStorage::doSaveGraphicAs(RS_Graphic* graphic, RS_GraphicView *graphicView, bool &cancelled, const QString& currentFileName,
                                          const RS2::FormatType preferredType){
#ifdef DWGSUPPORT
    const auto* session = AcadrustSession::get(*graphic);
    auto dialogResult = session ? session->chooseCopy(currentFileName, preferredType)
                                : askSaveFileDetails(currentFileName, preferredType);
#else
    auto dialogResult = askSaveFileDetails(currentFileName, preferredType);
#endif

    const QString fileName = dialogResult.filePath;
    RS2::FormatType saveFormat = dialogResult.fileType;

    bool result = false;
    if (fileName.isEmpty()) {
        // cancel is not an error - returns true
        result = true;
        cancelled = true;
    } else {
        QApplication::setOverrideCursor( QCursor(Qt::WaitCursor));
        graphic->setGraphicView(graphicView);
        result = saveGraphicAs(graphic, fileName, saveFormat, true);
        QApplication::restoreOverrideCursor();
    }
    return result;
}

LC_FileDialogService::FileDialogResult LC_DocumentsStorage::askSaveFileDetails(const QString& currentFileName,
                                                                               const RS2::FormatType preferredType) {
    return LC_FileDialogService::getFileDetails(LC_FileDialogService::SaveDrawing, currentFileName, preferredType);
}

bool LC_DocumentsStorage::autoSaveDocument(RS_Document* document, RS_GraphicView * graphicView, QString& autosaveFileName){
    bool result = false;
    if (document != nullptr) {
        document->setGraphicView(graphicView);
        result = autoSaveGraphic(document->getGraphic(), autosaveFileName);
    }
    return result;
}

bool LC_DocumentsStorage::saveBlockAs(RS_Graphic *block, const QString &fileName){
    bool result = false;
    if (!fileName.isEmpty()) {
        result = RS_FileIO::instance()->fileExport(*block, fileName, RS2::FormatDXFRW);
    }
    return result;
}

bool LC_DocumentsStorage::saveDocumentAs(const RS_Document* document, RS_GraphicView * graphicView, bool &cancelled) {
    cancelled = false;
    bool result = false;
    if (document != nullptr) {
        RS_Graphic* graphic = document->getGraphic();
        auto fileName = graphic->getFilename();
        if (document != graphic && graphic->getGraphicView())
            graphicView = graphic->getGraphicView();
        if (graphic->isDxfRecoverySourceFile(fileName))
            fileName = recoveryCopyName(fileName);
        auto preferredType = RS2::FormatUnknown;
#ifdef DWGSUPPORT
        const auto format = graphic->getFormatType();
        const auto* session = AcadrustSession::get(*graphic);
        if (format == RS2::FormatDWG2018 || (session && session->isOriginalFormat(format)))
            preferredType = format;
#endif
        result = doSaveGraphicAs(graphic, graphicView, cancelled, fileName, preferredType);
    }
    return result;
}

bool LC_DocumentsStorage::loadDocument(const RS_Document *document, const QString &fileName) const {
    return loadDocument(document, fileName, RS2::FormatUnknown);
}

bool LC_DocumentsStorage::loadDocument(const RS_Document* document, const QString& fileName, const RS2::FormatType type,
                                      const RS2::DxfTextEncoding encoding) const {
    bool result = false;
    if (document != nullptr && !fileName.isEmpty()) {
        // cosmetics..
        qApp->processEvents(QEventLoop::AllEvents, 1000);
        result = loadGraphic(document->getGraphic(), fileName, type, encoding);
    } else {
        //statusBar()->showMessage(tr("Opening aborted"), 2000);
    }
    return result;
}

bool LC_DocumentsStorage::loadDocumentFromTemplate(const RS_Document* document, const RS_GraphicView * graphicView, const QString& fileName, const RS2::FormatType type) const {
    bool result = false;

    if (document==nullptr || fileName.isEmpty()) {
        return result;
    }

    result = loadGraphicFromTemplate(document->getGraphic(), fileName, type);

    if (result) {
        graphicView->zoomAuto(false);
    }
    return result;
}

/**
 * Loads the given file into this graphic.
 */
bool LC_DocumentsStorage::loadGraphicFromTemplate(RS_Graphic* graphic, const QString &templateFileName, const RS2::FormatType type) const {
    const QString autosaveFilename = createAutoSaveFileName(QFileInfo(QDir::tempPath() + "/" + tr("Unnamed") + ".dxf"));

    // clean all:
    graphic->initForNewDocument();

    // import template file:
    const bool ret = RS_FileIO::instance()->fileImport(*graphic, templateFileName, type);
    // a drawing made from a template is a new drawing, saved as DXF
    graphic->setFormatType(RS2::FormatDXFRW);

    const QFileInfo finfo;
    graphic->markSaved(finfo.lastModified());
    graphic->setAutosaveFileName(autosaveFilename);
    return ret;
}

bool LC_DocumentsStorage::loadGraphic(RS_Graphic* graphic, const QString &filename, const RS2::FormatType type,
                                     const RS2::DxfTextEncoding encoding) const {
    graphic->initForNewDocument();

    QString input = filename;
#ifdef DWGSUPPORT
    delete AcadrustSession::get(*graphic);
    std::unique_ptr<AcadrustSession> experimental;
    auto helper = qEnvironmentVariable("FABLECAD_ACADRUST_HELPER");
#ifdef Q_OS_WIN
    if (helper.isEmpty()) {
        const QFileInfo bundled(QDir(QCoreApplication::applicationDirPath()).filePath("fablecad-acadrust.exe"));
        if (bundled.isFile())
            helper = bundled.absoluteFilePath();
    }
#endif
    const auto directory = QFileInfo(filename).absoluteDir();
    QTemporaryFile projection(directory.filePath(".fablecad-acadrust-XXXXXX.dwg"));
    QString projectionPath;
    const auto cleanup = qScopeGuard([&] {
        if (!projectionPath.isEmpty())
            QFile::remove(projectionPath);
    });
    if (graphic->getGraphicView() && !helper.isEmpty() &&
        filename.endsWith(".dwg", Qt::CaseInsensitive)) {
        if (!projection.open())
            return false;
        projectionPath = projection.fileName();
        if (!projection.remove())
            return false;
        experimental = std::make_unique<AcadrustSession>(graphic->getGraphicView());
        if (!experimental->open(helper, filename, projectionPath)) {
            RS_DIALOGFACTORY->commandMessage(experimental->error);
            return false;
        }
        input = projectionPath;
    }
#endif
    const bool ret = RS_FileIO::instance()->fileImport(*graphic, input, type, nullptr, encoding);

    if (ret) {
        graphic->onLoadingCompleted();
        const QFileInfo finfo(filename);
        const auto autosaveFileName = createAutoSaveFileName(finfo);
        graphic->setAutosaveFileName(autosaveFileName);
        graphic->setFilename(filename);
        // markSaved clears modified; re-dirty when import repairs rewrote coords
        // so Save does not silently rewrite the user's file without notice.
        const bool importMutated = graphic->takeImportGeometryMutated();
        graphic->markSaved(finfo.lastModified());
        if (importMutated || !graphic->dxfRecoverySourceFile().isEmpty())
            graphic->setModified(true);
#ifdef DWGSUPPORT
        if (experimental) {
            if (!experimental->captureBaseline(*graphic)) {
                RS_DIALOGFACTORY->commandMessage(tr("The experimental DWG projection could not be verified."));
                return false;
            }
            graphic->setFormatType(RS2::FormatDWG2018);
            experimental.release(); // the graphic view owns the session
        }
#endif
    }
    return ret;
}

QString LC_DocumentsStorage::dwgDiagnostics(const RS_Graphic& graphic) const {
#ifdef DWGSUPPORT
    if (const auto* session = AcadrustSession::get(graphic))
        return session->diagnostics(graphic);
#else
    Q_UNUSED(graphic);
#endif
    return tr("Detailed editing diagnostics require the Acadrust DWG reader. "
              "You can save an editable DWG 2018 copy for review. Advanced DWG objects may be omitted.");
}

bool LC_DocumentsStorage::saveVerifiedDwgCopy(RS_Graphic& graphic, const QString& destination,
                                             QString& error) const {
#ifdef DWGSUPPORT
    if (QFileInfo::exists(destination)) {
        error = tr("Choose a new file name for the DWG copy.");
        return false;
    }
    if (auto* session = AcadrustSession::get(graphic)) {
        QTemporaryDir staging(QFileInfo(destination).dir().filePath(".fablecad-copy-XXXXXX"));
        if (!staging.isValid()) {
            error = staging.errorString();
            return false;
        }
        const auto candidate = staging.filePath("verified.dwg");
        if (!session->save(graphic, candidate, false, true)) {
            error = session->error;
            return false;
        }
        if (QFile::rename(candidate, destination))
            return true;
    } else if (writeDwgWorkingCopy(graphic, destination, false)) {
        return true;
    }
    error = tr("The verified DWG copy could not be saved.");
#else
    Q_UNUSED(graphic);
    Q_UNUSED(destination);
    error = tr("This build does not support saving DWG files.");
#endif
    return false;
}

bool LC_DocumentsStorage::doSave(RS_Graphic* graphic, const bool sameFile) {
    bool result = false;
    const RS2::FormatType actualType = graphic->getFormatType();

//	- This is not an AutoSave operation.  This is a manual
//	  save operation.  So, ...
//		- Set working file name to the drawing file name.
//		- Backup drawing file (if necessary).
//	------------------------------------------------------
    const QString filename = graphic->getFilename();

    // Refuse before making a backup or changing either on-disk file.
    if (graphic->isDxfRecoverySourceFile(filename)) {
        RS_DIALOGFACTORY->commandMessage(
            tr("The original legacy DXF is protected. Save the recovered drawing to a different file."));
        return false;
    }

    const QFileInfo finfo(filename);
    const QDateTime fileLastModifyTime = finfo.lastModified();
    const QDateTime graphicLastSaveTime = graphic->getLastSaveTime();
    //bug#3414993

    //modifiedTime should only be used for the same filename
    if (sameFile && graphicLastSaveTime.isValid() && fileLastModifyTime != graphicLastSaveTime) {
        //file modified by others
        RS_DIALOGFACTORY->commandMessage(
            QObject::tr("File on disk modified. Please save to another file to avoid data loss! File modified: %1").arg(filename));
        return false;
    }

    QString actualName = filename;
#ifdef DWGSUPPORT
    auto* experimental = AcadrustSession::get(*graphic);
    const bool originalFormat = experimental && experimental->isOriginalFormat(actualType);
    if (experimental && (originalFormat || actualType == RS2::FormatDWG2018) &&
        experimental->protectsSource(actualName)) {
        QMessageBox::warning(QApplication::activeWindow(), tr("Experimental DWG"),
                             tr("Choose a different file name to preserve the original DWG."));
        return false;
    }
#endif
    if (LC_GET_ONE_BOOL("Defaults","AutoBackupDocument", true)) {
        backupDrawingFile(filename, graphic);
    }

    /*	Save drawing file if able to created associated object. */
    if (!actualName.isEmpty()) {
        graphic->prepareForSave();
        const auto* fileIO = RS_FileIO::instance();
        const bool isDxf = actualName.endsWith(QLatin1String(".dxf"), Qt::CaseInsensitive);
        const auto saveDrawing = [&] {
            return isDxf ? writeVerifiedDxf(*graphic, actualName, actualType)
                         : fileIO->fileExport(*graphic, actualName, actualType);
        };
#ifdef DWGSUPPORT
        const bool workingCopy = actualType == RS2::FormatDWG2018 &&
            graphic->dwgWorkingCopy() == QFileInfo(actualName).absoluteFilePath();
        if (experimental && (originalFormat || actualType == RS2::FormatDWG2018)) {
            result = experimental->save(*graphic, actualName, originalFormat);
            if (!result) {
                QMessageBox::warning(QApplication::activeWindow(), tr("Experimental DWG"),
                                     experimental->error);
            }
        } else {
            result = workingCopy ? writeDwgWorkingCopy(*graphic, actualName, true)
                                 : saveDrawing();
        }
        if (!experimental && !result && !workingCopy && actualType == RS2::FormatDWG2018 &&
            !fileIO->lastExportReport().isEmpty()) {
            QMessageBox choice(QMessageBox::Warning, tr("Save an editable DWG"),
                tr("Some DWG data cannot be preserved. You can save the editable drawing in a separate DWG 2018 file. Advanced objects such as table styles may be omitted."),
                QMessageBox::Cancel, QApplication::activeWindow());
            choice.setTextFormat(Qt::PlainText);
            choice.setDetailedText(fileIO->lastExportReport());
            auto* create = choice.addButton(tr("Save DWG copy"), QMessageBox::AcceptRole);
            choice.exec();
            if (choice.clickedButton() == create) {
                QString target = actualName;
                if (QFileInfo::exists(target)) {
                    const QFileInfo info(target);
                    target = QFileDialog::getSaveFileName(QApplication::activeWindow(),
                        tr("Save DWG copy"), info.dir().filePath(info.completeBaseName() + " - FableCAD.dwg"),
                        LC_FileDialogService::dwg2018NameFilter);
                }
                if (!target.isEmpty()) {
                    if (!target.endsWith(".dwg", Qt::CaseInsensitive))
                        target += ".dwg";
                    if (QFileInfo::exists(target)) {
                        QMessageBox::warning(QApplication::activeWindow(), tr("Save DWG copy"),
                            tr("Choose a new file name to keep the existing drawing."));
                    } else if (writeDwgWorkingCopy(*graphic, target, false)) {
                        actualName = target;
                        graphic->setFilename(target);
                        graphic->setDwgWorkingCopy(QFileInfo(target).absoluteFilePath());
                        graphic->setAutosaveFileName(createAutoSaveFileName(QFileInfo(target)));
                        result = true;
                    } else {
                        QMessageBox::warning(QApplication::activeWindow(), tr("Save DWG copy"),
                            tr("The DWG copy could not be written and reopened."));
                    }
                }
            }
        }
#else
        result = saveDrawing();
#endif
#ifdef DWGSUPPORT
        if (!experimental && !result && actualName.endsWith(QLatin1String(".dwg"), Qt::CaseInsensitive)) {
            const QString report = fileIO->lastExportReport();
            if (!report.isEmpty()) {
                RS_DIALOGFACTORY->commandMessage(report);
                if (actualType != RS2::FormatDWG2018) {
                    QMessageBox warning(QMessageBox::Warning, tr("DWG save blocked"),
                                        report, QMessageBox::Ok, QApplication::activeWindow());
                    warning.setTextFormat(Qt::PlainText);
                    warning.exec();
                }
            }
        }
#endif
        if (isDxf && !result)
            RS_DIALOGFACTORY->commandMessage(tr("The DXF could not be verified and saved. The destination was preserved."));

        // a drawing that failed to save still has changes to lose
        if (result) {
            const QFileInfo actualFileInfo(actualName);
            graphic->markSaved(actualFileInfo.lastModified());
        }
    }

    /*	Remove AutoSave file after user has successfully saved file.*/
    if (result) {
        /*	Autosave file object	*/
        const QString autosaveFilename = graphic->getAutoSaveFileName();
        QFile autosaveFile(autosaveFilename);

        if (autosaveFile.exists() && !graphic->isDxfRecoverySourceFile(autosaveFilename)) {
            autosaveFile.remove();
        }
    }
    return result;
}

bool LC_DocumentsStorage::autoSaveGraphic(RS_Graphic* graphic, QString& fileName) {
    bool ret = false;
    RS_DEBUG->print("LC_DocumentsStorage::autoSaveGraphic: isModified=%d", graphic->isModified());
    if (graphic->isModified()) {
        // Autosave writes DXF, whatever format the drawing is saved in: DXF is
        // what LibreCAD writes most reliably, and it can write it for every
        // drawing. createAutoSaveFileName() names the file *.dxf.
        const RS2::FormatType actualType = RS2::FormatDXFRW;
        const QString autosaveFileName = graphic->getAutoSaveFileName();
        RS_DEBUG->print("LC_DocumentsStorage::autoSaveGraphic: autosaveFileName='%s'", autosaveFileName.toLatin1().data());
        if (!autosaveFileName.isEmpty()) {
            RS_DEBUG->print("LC_DocumentsStorage::autoSaveGraphic: attempting export to '%s'", autosaveFileName.toLatin1().data());
            ret = writeVerifiedDxf(*graphic, autosaveFileName, actualType);
            if (ret) {
                hideBackupFile(autosaveFileName);
            }
            RS_DEBUG->print("LC_DocumentsStorage::autoSaveGraphic: export result=%d", ret);
            /*
             fixme - sand - don't mark file as non-modified on auto-save.
             *QFileInfo finfo(autosaveFileName);
            graphic->markSaved(finfo.lastModified());
            */
        } else {
            RS_DEBUG->print("LC_DocumentsStorage::autoSaveGraphic: autosaveFileName is empty, skipping export");
        }
        fileName = autosaveFileName;
    } else {
        // file not modified
        RS_DEBUG->print("LC_DocumentsStorage::autoSaveGraphic: file not modified, skipping");
        ret = true;
    }
    return ret;
}

bool LC_DocumentsStorage::exportGraphics(RS_Graphic* graphic, const QString& fileName, const RS2::FormatType formatType) {
    if (graphic->isDxfRecoverySourceFile(fileName))
        return false;
    graphic->setFilename(fileName);
    graphic->setFormatType(formatType);
    if (!fileName.isEmpty()) {
        const bool result = RS_FileIO::instance()->fileExport(*graphic, fileName, formatType);
        if (result) {
            const QFileInfo finfo(fileName);
            graphic->markSaved(finfo.lastModified());
        }
        return result;
    }
    return false;
}

bool LC_DocumentsStorage::saveGraphicAs(RS_Graphic* graphic, const QString &filename, const RS2::FormatType type, const bool forceSave) {
    if (graphic->isDxfRecoverySourceFile(filename)) {
        RS_DIALOGFACTORY->commandMessage(
            tr("The original legacy DXF is protected. Save the recovered drawing to a different file."));
        return false;
    }
    bool ret = false;

    // Check/memorize if file name we want to use as new file
   // name is the same as the actual file name.

    const auto filenameSaved = graphic->getFilename();
    const auto autosaveFilenameSaved = graphic->getAutoSaveFileName();
    bool filenameIsSame = filename == filenameSaved;
    const auto formatTypeSaved = graphic->getFormatType();

    graphic->setFilename(filename);
    graphic->setFormatType(type);

    const QFileInfo finfo(filename);

    // Keep the recovery copy beside the destination drawing.
    QString autosaveFileName = createAutoSaveFileName(finfo);
    // A source literally named #copy.dxf must not also be the corrected
    // copy's autosave. Retain a DXF extension and avoid any existing aliases.
    QString recoveryPrefix = QStringLiteral("#recovered_");
    while (graphic->isDxfRecoverySourceFile(autosaveFileName)) {
        autosaveFileName = createAutoSaveFileName(finfo, recoveryPrefix);
        recoveryPrefix.prepend('#');
    }
    graphic->setAutosaveFileName(autosaveFileName);

    // When drawing is saved using a different name than the actual
    // drawing file name, make LibreCAD think that drawing file
    // has been modified, to make sure the drawing file saved.
    if (!filenameIsSame || forceSave) {
        graphic->setModified(true);
        filenameIsSame = false;
    }
    ret = doSave(graphic, filenameIsSame);

    if (ret) {
        // Save was successful, remove old autosave file.
        QFile autoSaveFile(autosaveFilenameSaved);
        if (autoSaveFile.exists() && !graphic->isDxfRecoverySourceFile(autosaveFilenameSaved)) {
            autoSaveFile.remove();
        }
    } else {
        //do not modify filenames:
        graphic->setFilename(filenameSaved);
        graphic->setAutosaveFileName(autosaveFilenameSaved);
        graphic->setFormatType(formatTypeSaved);
    }
    return ret;
}

bool LC_DocumentsStorage::backupDrawingFile(const QString &drawingFileName, const RS_Graphic* graphic) {
    const QString backupFileSuffix = LC_GET_ONE_STR("Defaults", "BackupFileSuffix", "~");
    return backupDrawingFile(drawingFileName, backupFileSuffix, graphic);
}

bool LC_DocumentsStorage::backupDrawingFile(const QString &drawingFileName, const QString& backupSuffix,
                                           const RS_Graphic* graphic) {
    bool ret = false;
    const QString suffix = validatedFileAffix(backupSuffix, QStringLiteral("~"));
    if (!drawingFileName.isEmpty() && !suffix.isEmpty()) {
        const auto backupFileName = backupFilePath(drawingFileName + suffix);
        if (graphic != nullptr && graphic->isDxfRecoverySourceFile(backupFileName)) {
            RS_DIALOGFACTORY->commandMessage(
                tr("Skipped a backup that would replace the protected original legacy DXF."));
            return false;
        }
        auto drawingFile = QFile(drawingFileName);
        if (drawingFile.exists()) {
            auto backupFile = QFile(backupFileName);
            if (backupFile.exists()) {
                backupFile.remove();
            }
            ret = drawingFile.copy(backupFileName);
            if (ret) {
                hideBackupFile(backupFileName);
            }
        }
    }
    return ret;
}

QString LC_DocumentsStorage::createAutoSaveFileName(const QFileInfo &fileInfo) const {
    const QString autosaveFilePrefix = LC_GET_ONE_STR("Defaults", "AutosaveFilePrefix", "#");
    QString autosaveFileName = createAutoSaveFileName(fileInfo, autosaveFilePrefix);
    // autosave writes DXF: name the file so that it opens as DXF
    if (fileInfo.suffix().compare("dxf", Qt::CaseInsensitive) != 0) {
        autosaveFileName += ".dxf";
    }
    return autosaveFileName;
}

QString LC_DocumentsStorage::createAutoSaveFileName(const QFileInfo &fileInfo, const QString &filePrefix) const {
    return createAutoSaveFileName(fileInfo.path(), filePrefix, fileInfo.fileName());
}

QString LC_DocumentsStorage::createAutoSaveFileName(const QString& path, const QString &filePrefix,  const QString& fileName) const {
    const QString prefix = validatedFileAffix(filePrefix.isEmpty() ? QStringLiteral("#") : filePrefix,
                                            QStringLiteral("#"));
    return backupFilePath(QDir(path).filePath(prefix + fileName));
}
